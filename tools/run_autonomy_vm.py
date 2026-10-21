#!/usr/bin/env python3
"""Run the unchanged autonomy gate in a disposable, offline Debian guest.

No sudo, KVM, host mounts, network devices, persistent guests, or host security
changes. Host dependencies are installed separately using the OS package manager.
Only a clean checked-out Git revision qualifies. See ci/README-autonomy-vm.md.
"""
import argparse
import base64
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
import uuid


ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'tools/ci/autonomy-vm.lock.json'
GUEST = ROOT / 'tools/ci/autonomy-guest.sh'
VM_SECONDS = 3600
MAX_LOG_BYTES = 32 * 1024 * 1024
STAGES = {
    b'JASH_VM_STAGE=provisioning': 'installing pinned guest dependencies',
    b'JASH_VM_STAGE=preflight': 'nonroot isolation preflight and build configuration',
    b'JASH_VM_STAGE=modules': 'building native guest modules',
    b'JASH_VM_STAGE=original-gate': 'original autonomy gate (includes cold build and LTO)',
    b'JASH_VM_STAGE=complete': 'guest commands finished; validating complete evidence next',
}


def digest(path, algorithm='sha256'):
    value = hashlib.new(algorithm)
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def run(argv, **kwargs):
    print('+ ' + ' '.join(map(str, argv)), flush=True)
    return subprocess.run(list(map(str, argv)), check=True, **kwargs)


def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args], text=True).strip()


def load_lock():
    lock = json.loads(LOCK.read_text())
    if lock['version'] != 1 or lock['archive'] != 'https://deb.debian.org/debian' or \
            lock['suite'] != 'trixie' or lock['component'] != 'main':
        raise ValueError('unsupported dependency lock')
    image = lock['image']
    if not image['url'].startswith('https://cloud.debian.org/images/cloud/trixie/') or \
            '/latest/' in image['url'] or not re.fullmatch('[0-9a-f]{128}', image['sha512']):
        raise ValueError('image must be pinned to an official dated build and SHA512')
    seen = set()
    for item in [lock['keyring'], *lock['packages']]:
        if item['package'] in seen or not re.fullmatch('[a-z0-9][a-z0-9+.-]*', item['package']):
            raise ValueError('invalid/duplicate package in lock')
        seen.add(item['package'])
        if item['architecture'] not in ('all', 'amd64') or \
                not re.fullmatch('[0-9a-f]{64}', item['sha256']) or \
                not re.fullmatch(r'pool/main/[A-Za-z0-9+./_~-]+\.deb', item['filename']) or \
                '..' in Path(item['filename']).parts:
            raise ValueError('invalid package archive path/hash/architecture')
    return lock


def fetch(url, target, expected, algorithm='sha256'):
    if target.is_symlink():
        raise ValueError(f'refusing symlink in artifact cache: {target}')
    if target.exists():
        if digest(target, algorithm) != expected:
            raise ValueError(f'cached artifact hash mismatch: {target}')
        return
    print(f'Downloading {url}', flush=True)
    temporary = target.with_suffix(target.suffix + '.partial')
    created = False
    try:
        with temporary.open('xb') as stream:
            created = True
            with urllib.request.urlopen(url, timeout=60) as response:
                if urllib.parse.urlsplit(response.url).scheme != 'https':
                    raise ValueError('refusing non-HTTPS download redirect')
                shutil.copyfileobj(response, stream)
        if digest(temporary, algorithm) != expected:
            raise ValueError(f'download hash mismatch: {url}')
        temporary.replace(target)
    finally:
        if created:
            temporary.unlink(missing_ok=True)


def paragraphs(text):
    for paragraph in text.split('\n\n'):
        result = {}
        for line in paragraph.splitlines():
            if not line.startswith((' ', '\t')) and ': ' in line:
                key, value = line.split(': ', 1)
                result[key.lower()] = value
        if result:
            yield result


def authenticate_packages(lock, work, output):
    """APT verifies current signed Debian indexes; every lock entry must match.

    All APT state/config lives in the temporary directory, not the host setup.
    Missing pinned versions fail closed instead of silently resolving upgrades.
    """
    apt = work / 'apt'
    for name in ('etc/empty', 'state/lists/partial', 'cache/archives/partial'):
        (apt / name).mkdir(parents=True)
    (apt / 'state/status').touch()
    # Ubuntu's own Debian keyring can predate Debian 13. Bootstrap from the
    # reviewed SHA256-pinned official keyring package, without installing it or
    # changing the host trust store. APT then authenticates every dependency.
    keyring_package = work / 'debian-archive-keyring.deb'
    fetch(lock['archive'] + '/' + lock['keyring']['filename'],
          keyring_package, lock['keyring']['sha256'])
    run(['dpkg-deb', '-x', keyring_package, work / 'keyring'], timeout=30)
    keyring = work / 'keyring/usr/share/keyrings/debian-archive-keyring.gpg'
    (apt / 'etc/sources.list').write_text(
        f'deb [arch=amd64 signed-by={keyring}] {lock["archive"]} trixie main\n')
    config = apt / 'apt.conf'
    config.write_text(f'''Dir::Etc "{apt}/etc";
Dir::Etc::sourcelist "sources.list";
Dir::Etc::sourceparts "empty";
Dir::Etc::parts "empty";
Dir::Etc::main "empty.conf";
Dir::Etc::preferencesparts "empty";
Dir::State "{apt}/state";
Dir::State::status "{apt}/state/status";
Dir::Cache "{apt}/cache";
APT::Architecture "amd64";
APT::Get::List-Cleanup "0";
Acquire::Languages "none";
Acquire::Retries "2";
Acquire::https::Timeout "60";
Acquire::AllowInsecureRepositories "false";
APT::Get::AllowUnauthenticated "false";
''')
    env = dict(os.environ, APT_CONFIG=str(config))
    with (output / 'apt-update.log').open('wb') as log:
        run(['apt-get', 'update', '--error-on=any'], env=env, stdout=log,
            stderr=subprocess.STDOUT, timeout=300)
    all_packages = [lock['keyring'], *lock['packages']]
    wanted = [f'{p["package"]}={p["version"]}' for p in all_packages]
    metadata = subprocess.check_output(['apt-cache', 'show', *wanted], env=env,
                                       text=True, timeout=120)
    candidates = {(p.get('package'), p.get('version')): p for p in paragraphs(metadata)}
    for package in all_packages:
        actual = candidates.get((package['package'], package['version']))
        if actual is None or any(actual.get(key) != value for key, value in package.items()):
            raise ValueError(f'pinned package missing or changed in signed index: {package["package"]}')
    releases = list((apt / 'state/lists').glob('*InRelease'))
    if len(releases) != 1:
        raise ValueError('expected one authenticated Debian InRelease')
    shutil.copyfile(releases[0], output / 'debian.InRelease')
    (output / 'debian-packages.txt').write_text(metadata)


def extract_boot(image, spec, work, cache):
    kernel, initrd = cache / 'vmlinuz', cache / 'initrd.img'
    if kernel.is_symlink() or initrd.is_symlink():
        raise ValueError('refusing symlinked boot artifact')
    if kernel.exists() and initrd.exists():
        if digest(kernel) != spec['kernel_sha256'] or digest(initrd) != spec['initrd_sha256']:
            raise ValueError('cached boot artifact hash mismatch')
        return kernel, initrd
    raw, partition = work / 'image.raw', work / 'root.raw'
    run(['qemu-img', 'convert', '-f', 'qcow2', '-O', 'raw', image, raw], timeout=180)
    with raw.open('rb') as source:
        source.seek(512)
        header = source.read(92)
        if header[:8] != b'EFI PART':
            raise ValueError('pinned image has no GPT')
        table, count, size = struct.unpack_from('<QII', header, 72)
        if count > 256 or size != 128:
            raise ValueError('unexpected GPT table')
        source.seek(table * 512)
        entries = [source.read(size) for _ in range(count)]
        matches = [entry for entry in entries
                   if str(uuid.UUID(bytes_le=entry[16:32])) == spec['root_partuuid']]
        if len(matches) != 1:
            raise ValueError('pinned root partition missing or ambiguous')
        start, end = struct.unpack_from('<QQ', matches[0], 32)
        remaining = (end - start + 1) * 512
        if start > end or (end + 1) * 512 > raw.stat().st_size:
            raise ValueError('invalid root partition extent')
        source.seek(start * 512)
        with partition.open('wb') as destination:
            while remaining:
                block = source.read(min(1024 * 1024, remaining))
                if not block:
                    raise ValueError('truncated partition')
                if block.count(0) == len(block):
                    destination.seek(len(block), 1)
                else:
                    destination.write(block)
                remaining -= len(block)
            destination.truncate()
    # debugfs is read-only by default. No mounting, loop device or root needed.
    for source, target, expected in (
        (f'/boot/vmlinuz-{spec["kernel"]}', kernel, spec['kernel_sha256']),
        (f'/boot/initrd.img-{spec["kernel"]}', initrd, spec['initrd_sha256']),
    ):
        run(['debugfs', '-R', f'dump {source} {target}', partition], timeout=120)
        if not target.is_file() or digest(target) != expected:
            raise ValueError(f'extracted boot artifact hash mismatch: {target}')
    raw.unlink()
    partition.unlink()
    return kernel, initrd


def prepare(lock, work, cache, output):
    cache.mkdir(parents=True, exist_ok=True)
    authenticate_packages(lock, work, output)
    image = cache / 'debian.qcow2'
    fetch(lock['image']['url'], image, lock['image']['sha512'], 'sha512')
    debs = cache / 'debs'
    debs.mkdir(exist_ok=True)
    for package in lock['packages']:
        fetch(lock['archive'] + '/' + package['filename'],
              debs / Path(package['filename']).name, package['sha256'])
    kernel, initrd = extract_boot(image, lock['image'], work, cache)
    return image, kernel, initrd


def source_archive(payload):
    revision = git('rev-parse', 'HEAD')
    if not re.fullmatch('[0-9a-f]{40}', revision):
        raise ValueError('expected a 40-hex checked-out commit')
    if os.environ.get('GITHUB_SHA') and os.environ['GITHUB_SHA'] != revision:
        raise ValueError('checked-out revision does not match GITHUB_SHA')
    if git('status', '--porcelain', '--untracked-files=no'):
        raise ValueError('VM qualification requires a clean tracked checkout')
    # New/untracked runner code must not qualify an older committed revision.
    for path in (Path(__file__).resolve(), LOCK, GUEST):
        committed = subprocess.check_output(['git', '-C', str(ROOT), 'show',
                                            f'{revision}:{path.relative_to(ROOT)}'])
        if committed != path.read_bytes():
            raise ValueError('runner and dependency inputs must match the tested commit')
    run(['git', '-C', ROOT, 'archive', '--format=tar', '-o', payload / 'jash.tar', revision])
    return dict(revision=revision, tree=git('rev-parse', f'{revision}^{{tree}}'),
                epoch=int(git('show', '-s', '--format=%ct', revision)),
                source_sha256=digest(payload / 'jash.tar'))


def write_seed(work, source, nonce):
    script = GUEST.read_text()
    for key, value in dict(REVISION=source['revision'], SOURCE_SHA256=source['source_sha256'],
                           EPOCH=source['epoch'], NONCE=nonce).items():
        script = script.replace(f'@{key}@', str(value))
    if re.search(r'@[A-Z_]+@', script):
        raise ValueError('unresolved guest template variable')
    seed = work / 'seed'
    seed.mkdir()
    (seed / 'meta-data').write_text(f'instance-id: jash-{nonce}\nlocal-hostname: jash-ci\n')
    # JSON is valid YAML and avoids interpolating shell text into YAML syntax.
    config = dict(users=[dict(name='runner', uid=1000, lock_passwd=True,
                              shell='/bin/bash', sudo=False)],
                  ssh_pwauth=False, disable_root=True,
                  write_files=[dict(path='/root/jash-autonomy.sh', permissions='0700', content=script)],
                  runcmd=[['bash', '/root/jash-autonomy.sh']])
    (seed / 'user-data').write_text('#cloud-config\n' + json.dumps(config) + '\n')
    run(['genisoimage', '-quiet', '-output', work / 'seed.iso', '-volid', 'cidata',
         '-joliet', '-rock', seed], timeout=60)


def validate_result(serial, nonce, expected, qemu_returncode):
    if qemu_returncode != 0:
        raise ValueError(f'emulator did not exit normally: {qemu_returncode}')
    if len(serial) > 2 * MAX_LOG_BYTES:
        raise ValueError('serial evidence exceeds the size bound')
    log_prefix = f'JASH_VM_LOG_{nonce} '.encode()
    result_prefix = f'JASH_VM_RESULT_{nonce} '.encode()
    chunks, results = [], []
    for line in serial.splitlines():
        if line.startswith(log_prefix):
            chunks.append(line[len(log_prefix):])
        elif line.startswith(result_prefix):
            results.append(json.loads(line[len(result_prefix):]))
    if len(results) != 1 or not chunks:
        raise ValueError('missing/duplicate guest completion or missing log')
    result = results[0]
    if not isinstance(result, dict) or type(result.get('exit')) is not int:
        raise ValueError('invalid guest completion record')
    if result.get('version') != 1 or any(result.get(key) != expected[key]
                                        for key in ('revision', 'source_sha256')):
        raise ValueError('guest completion does not bind the expected source')
    if not isinstance(result.get('log_bytes'), int) or not 0 < result['log_bytes'] <= MAX_LOG_BYTES:
        raise ValueError('guest log size outside bound')
    log = base64.b64decode(b''.join(chunks), validate=True)
    if len(log) != result['log_bytes'] or hashlib.sha256(log).hexdigest() != result.get('log_sha256'):
        raise ValueError('guest log lost, changed, or truncated')
    return result, log


def qualify(result, log):
    if result['exit'] != 0:
        raise ValueError(f'guest gate failed with exit {result["exit"]} (77 is failure, not qualification)')
    for marker in (b'JASH_VM_ORIGINAL_GATE_BEGIN', b'JASH_VM_ORIGINAL_GATE_END'):
        if log.splitlines().count(marker) != 1:
            raise ValueError('unchanged original autonomy gate did not complete exactly once')
    if re.search(rb'OK \(skipped=|\bSKIP(?:PED)?\b|\b[1-9][0-9]* skipped\b', log):
        raise ValueError('a skipped qualification cannot pass the VM gate')


def run_qemu(argv, serial_path, seconds):
    """Expose only fixed progress hints; qualification still needs framed logs.

    The whole emulator has one monotonic deadline. Timeout, interruption and
    observer errors kill and reap only this owned process before propagating.
    """
    with serial_path.open('wb') as serial, serial_path.open('rb') as observer:
        process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=serial,
                                   stderr=subprocess.STDOUT)
        deadline = time.monotonic() + seconds
        pending, seen = b'', set()
        try:
            while True:
                pending += observer.read(65536)
                *lines, pending = pending.split(b'\n')
                for line in lines:
                    line = line.rstrip(b'\r')
                    if line in STAGES and line not in seen:
                        print('VM stage: ' + STAGES[line], flush=True)
                        seen.add(line)
                if len(pending) > 4096:
                    pending = b''  # Non-stage output is retained in serial.log.
                status = process.poll()
                if status is not None:
                    return status
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise subprocess.TimeoutExpired(argv, seconds)
                time.sleep(min(1, remaining))
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()


def execute(lock, work, cache, output, assets):
    payload = work / 'payload'
    payload.mkdir()
    source = source_archive(payload)
    nonce = secrets.token_hex(16)
    (output / 'provenance.json').write_text(json.dumps(dict(source, nonce=nonce,
        backend='qemu-tcg', image=lock['image'], dependency_lock_sha256=digest(LOCK),
        host_uid=os.getuid(), vm_seconds=VM_SECONDS, vcpus=2, memory_mib=2048), indent=2) + '\n')
    (payload / 'debs').mkdir()
    checksums = []
    for package in lock['packages']:
        name = Path(package['filename']).name
        os.link(cache / 'debs' / name, payload / 'debs' / name)
        checksums.append(f'{package["sha256"]}  debs/{name}\n')
    (payload / 'packages.sha256').write_text(''.join(checksums))
    write_seed(work, source, nonce)
    run(['genisoimage', '-quiet', '-output', work / 'payload.iso', '-volid', 'JASHDATA',
         '-joliet', '-rock', payload], timeout=120)
    image, kernel, initrd = assets
    overlay = work / 'root.qcow2'
    run(['qemu-img', 'create', '-f', 'qcow2', '-F', 'qcow2', '-b', image, overlay, '12G'], timeout=30)
    argv = ['qemu-system-x86_64', '-accel', 'tcg,thread=multi', '-machine', 'pc',
            '-cpu', 'qemu64', '-smp', '2', '-m', '2048', '-kernel', str(kernel),
            '-initrd', str(initrd), '-append',
            f'root=PARTUUID={lock["image"]["root_partuuid"]} ro console=ttyS0,115200 consoleblank=0',
            '-drive', f'file={overlay},if=virtio,format=qcow2',
            '-drive', f'file={work}/seed.iso,media=cdrom,readonly=on',
            '-drive', f'file={work}/payload.iso,media=cdrom,readonly=on',
            '-nic', 'none', '-vga', 'none', '-nographic', '-monitor', 'none', '-no-reboot']
    (output / 'qemu-command.json').write_text(json.dumps(argv, indent=2) + '\n')
    (output / 'qemu-version.txt').write_text(subprocess.check_output(
        ['qemu-system-x86_64', '--version'], text=True, timeout=10))
    print(f'Running unchanged autonomy gate in offline QEMU TCG guest (limit {VM_SECONDS}s).', flush=True)
    serial_path = output / 'serial.log'
    returncode = run_qemu(argv, serial_path, VM_SECONDS)
    result, log = validate_result(serial_path.read_bytes(), nonce, source, returncode)
    (output / 'guest.log').write_bytes(log)
    (output / 'guest-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(log.decode('utf-8', errors='replace'), flush=True)
    qualify(result, log)
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare', action='store_true', help='authenticate/download assets only; never qualifies tests')
    parser.add_argument('--cache', type=Path, default=Path(os.environ.get('JASH_AUTONOMY_VM_CACHE',
                        str(Path(tempfile.gettempdir()) / f'jash-autonomy-vm-cache-{os.getuid()}'))))
    parser.add_argument('--output', type=Path, default=None)
    args = parser.parse_args()
    if sys.platform != 'linux' or os.getuid() == 0:
        parser.error('requires an ordinary nonroot Linux host user')
    output = args.output or Path(os.environ.get('JASH_AUTONOMY_VM_OUTPUT',
                        str(Path(tempfile.gettempdir()) / ('jash-autonomy-vm-' + secrets.token_hex(8)))))
    output = output.absolute()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    print(f'VM evidence: {output}', flush=True)
    status = dict(status='incomplete', backend='qemu-tcg')
    try:
        lock = load_lock()
        shutil.copyfile(LOCK, output / 'dependency-lock.json')
        for command in ('qemu-system-x86_64', 'qemu-img', 'genisoimage', 'debugfs',
                        'apt-get', 'apt-cache', 'dpkg-deb', 'git'):
            if shutil.which(command) is None:
                raise RuntimeError(f'missing host dependency: {command}')
        cache = args.cache.absolute()
        # Cache and payload share a filesystem so package hardlinks need no copies.
        cache.mkdir(mode=0o700, parents=True, exist_ok=True)
        metadata = cache.lstat()
        if not stat.S_ISDIR(metadata.st_mode) or metadata.st_uid != os.getuid() or metadata.st_mode & 0o077:
            raise ValueError('cache must be a private mode-0700 directory owned by the current user')
        descriptor = os.open(cache / '.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        with os.fdopen(descriptor, 'w') as cache_lock:
            # Never start two guests against the shared preparation cache.
            fcntl.flock(cache_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with tempfile.TemporaryDirectory(prefix='run-', dir=cache) as temporary:
                work = Path(temporary)
                assets = prepare(lock, work, cache, output)
                if args.prepare:
                    status.update(status='prepared_only', qualification=False)
                else:
                    source = execute(lock, work, cache, output, assets)
                    status.update(status='passed', qualification=True, **source)
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        status.update(status='failed', qualification=False, error=str(error))
        print(f'Autonomy VM failed closed: {error}', file=sys.stderr, flush=True)
        return 1
    finally:
        (output / 'result.json').write_text(json.dumps(status, indent=2) + '\n')


if __name__ == '__main__':
    raise SystemExit(main())
