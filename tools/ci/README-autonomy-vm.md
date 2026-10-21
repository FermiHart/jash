# Explicit disposable-kernel autonomy gate

`tests/test_suite.sh` still registers all 351 gates. Its default `native`
backend and canonical argv list are unchanged. Setting
`JASH_AUTONOMY_BACKEND=qemu-tcg` changes only the existing **experimental
verifiable autonomy (Linux integration)** entry to `tools/run_autonomy_vm.py`.
An unknown backend is an error, not a fallback or skip. The other 350 gates
run on the native host. A guest pass proves guest isolation, not host support.

The VM executes the original, unmodified `tests/test_autonomy.sh`, including
its original sandbox checks, build, unit/integration tests and demonstration.
It additionally runs the original sandbox preflight. The host does not run
an incompatible isolation preflight and pretend it qualified the guest.

## Host requirements and replay

Use an ordinary nonroot Linux x86-64 host account with `python3`, `git`,
`apt-get`, `apt-cache`, `dpkg-deb`, `qemu-system-x86_64`, `qemu-img`,
`genisoimage` and `debugfs`. On an Ubuntu CI runner these come from its signed
distribution repositories (`qemu-system-x86 qemu-utils genisoimage e2fsprogs`).
No KVM, privileged container, host mounts, host security/sysctl changes or
additional capabilities are required. The runner never invokes sudo.

From a clean committed checkout:

```sh
python3 -B tools/run_autonomy_vm.py --prepare --output /tmp/vm-prepare
JASH_AUTONOMY_BACKEND=qemu-tcg \
JASH_AUTONOMY_VM_OUTPUT=/tmp/vm-result \
bash tests/test_suite.sh --timeout 4200 --output /tmp/suite-result
```

Output directories must not already exist. `--prepare` only downloads and
verifies assets; its result explicitly says `qualification: false`. To run
the full original gate by itself, invoke `python3 -B tools/run_autonomy_vm.py`.
The default reusable cache is `/tmp/jash-autonomy-vm-cache-<uid>`; override it with
`--cache` or `JASH_AUTONOMY_VM_CACHE`. Its contents are hash-checked on every
use and a nonblocking lock prevents concurrent runs using the same cache.
The cache must be owned by the host user with mode 0700 and cannot be a symlink.
No success is reused from a previous guest.

The checked-out 40-hex `HEAD` must be clean for tracked files, and the runner,
guest script and lock must match that commit. If `GITHUB_SHA` exists, it must
equal `HEAD`. `git archive HEAD` supplies the guest source, not live host files
or previous build outputs. The recorded revision, tree, source-archive SHA256
and commit timestamp bind the run; `SOURCE_DATE_EPOCH` applies throughout the
guest build. Untracked files are not part of the tested source archive.

## Supply chain and execution

`autonomy-vm.lock.json` pins the official Debian 13 generic amd64 image build
20261017-2634 by SHA512, its extracted kernel/initrd by SHA256, and every guest
dependency by name/version/architecture/archive path/SHA256. The original
lock was checked against Debian's signed trixie package index.

Ubuntu's Debian keyring package may be too old for Debian 13. The runner
bootstraps the official `debian-archive-keyring` 2025.1 package with its reviewed,
committed SHA256, extracts it locally, and uses it solely for a temporary APT
configuration. It does not change the host's package configuration or trust
store. APT must authenticate current Debian trixie metadata and every pinned
package, including the keyring itself, must match that signed metadata before
guest packages are downloaded or used. A removed version or hash mismatch
fails closed. A maintainer must review and update the lock to refresh it;
the runner never silently substitutes newer packages.

The image is verified before extraction or boot. A read-only `debugfs` pass
extracts its pinned kernel/initrd for direct boot. The root filesystem uses a
fresh disposable qcow2 overlay. Source and packages arrive on read-only ISOs.
QEMU uses TCG software emulation, two vCPUs and 2 GiB RAM, no NIC, no host-file
shares and a 3600-second runtime bound. A normal guest root setup installs only
the locked distro packages (never downgrading newer image security updates),
then the original build/tests run as uid/gid 1000 with no supplementary groups.
No original bubblewrap flags, policies, test assertions or test files change.

## Evidence and failure semantics

Guest output first goes to a regular logfile, avoiding serial getty resets.
Root-owned stage notifications expose provisioning, nonroot preflight, modules
and original-gate phases in live CI output without granting the test user access
to the serial device. The host prints only allowlisted stage hints; those hints
never qualify tests. Its monotonic whole-emulator deadline kills and reaps the
owned QEMU process on timeout or interruption.
At shutdown a unique per-run envelope carries base64 log bytes, their byte
count and SHA256, source revision/archive hash and guest exit code. The host
requires normal QEMU exit, exactly one matching completion, a complete valid
log, exactly one start/end pair for the original gate, zero exit and no skipped
qualification. Exit 77, lost/truncated/interleaved logs, revision mismatch,
timeout and shutdown without test completion all fail. Transport unit tests
are not VM qualification and never replace the original gate.

The evidence directory retains serial output, verified guest output/result
when recoverable, provenance, exact emulator command, dependency lock,
authenticated InRelease, package metadata and APT output. The workflow uploads
these and the existing suite manifest/events/logs even on failure. The runner
does not invent per-test JUnit results. Disks and payloads are ephemeral and
are not uploaded. `result.json` must say `passed` with `qualification: true`;
absence, incomplete status or preparation-only status is not success.

The suite job permits 120 minutes overall and explicitly gives each gate a
4200-second ceiling because this one gate includes a fresh emulated build.
The suite runner's native default timeout remains unchanged at 600 seconds.
The 3600-second whole-guest budget accounts for the measured cold TCG/LTO
build: a prototype's 1800-second build-and-test budget expired during workflow
tests after compilation, isolation, assertions and libjef passed. That partial
run did not qualify the full gate; publication/resource/demo tests still need
to finish inside the bounded final run.
Independent review and an actual GitHub Actions pass for the final committed
implementation are required before adoption or merge; a local prototype pass
or these harness tests alone do not establish that validation.
