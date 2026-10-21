#!/bin/bash
# Runs only in the disposable guest. This file is rendered into cloud-init.
exec >/var/log/jash-autonomy.log 2>&1
set -euo pipefail

finish() {
    result=$?
    trap - EXIT
    set +e
    sync
    # Framing plus length/digest makes truncated or interleaved serial output
    # an error, never an apparently successful but incomplete test log.
    python3 - "$result" <<'PY'
import base64, hashlib, json, pathlib, sys
data = pathlib.Path('/var/log/jash-autonomy.log').read_bytes()
result = dict(version=1, revision='@REVISION@', source_sha256='@SOURCE_SHA256@',
              exit=int(sys.argv[1]), log_bytes=len(data),
              log_sha256=hashlib.sha256(data).hexdigest())
with open('/dev/ttyS0', 'wb', buffering=0) as serial:
    encoded = base64.b64encode(data)
    for start in range(0, len(encoded), 76):
        serial.write(b'JASH_VM_LOG_@NONCE@ ' + encoded[start:start+76] + b'\n')
    serial.write(b'JASH_VM_RESULT_@NONCE@ ' + json.dumps(result, sort_keys=True).encode() + b'\n')
PY
    poweroff
}
trap finish EXIT

echo 'JASH_VM_PROVISION_BEGIN'
printf '%s\n' 'JASH_VM_STAGE=provisioning' >/dev/ttyS0
uname -a
cat /etc/os-release
mkdir -p /mnt/jash /home/runner/jash /tmp/jash-packages
mount -o ro /dev/disk/by-label/JASHDATA /mnt/jash
cd /mnt/jash
sha256sum --check packages.sha256
echo '@SOURCE_SHA256@  jash.tar' | sha256sum --check -
export DEBIAN_FRONTEND=noninteractive
packages=()
for package in debs/*.deb; do
    name=$(dpkg-deb -f "$package" Package)
    wanted=$(dpkg-deb -f "$package" Version)
    installed=$(dpkg-query -W -f='${Version}' "$name" 2>/dev/null || true)
    # The image may already contain a newer security update. Never downgrade.
    if [[ -z "$installed" ]] || dpkg --compare-versions "$wanted" gt "$installed"; then
        cp "$package" /tmp/jash-packages/
        packages+=("/tmp/jash-packages/${package##*/}")
    fi
done
if (( ${#packages[@]} )); then
    dpkg -i "${packages[@]}"
fi
dpkg --audit
dpkg-query -W -f='${Package}\t${Version}\t${Architecture}\n'
tar -xf jash.tar -C /home/runner/jash
chown -R runner:runner /home/runner/jash
echo 'JASH_VM_PROVISION_END'

# No guest-root test, inherited host environment, host shares, or network.
run_as_runner() {
    runuser -u runner -- env -i HOME=/home/runner USER=runner LOGNAME=runner \
        PATH=/usr/bin:/bin LANG=C.UTF-8 SOURCE_DATE_EPOCH=@EPOCH@ \
        bash --noprofile --norc -c 'set -euo pipefail; cd /home/runner/jash; '"$1"
}
printf '%s\n' 'JASH_VM_STAGE=preflight' >/dev/ttyS0
run_as_runner '
        test "$(id -u)" = 1000
        test "$(id -G)" = 1000
        id
        /usr/bin/bwrap --version
        stat -c "%U:%G %a %n" /usr/bin/bwrap
        printf "%s\n" "JASH_VM_REVISION=@REVISION@"
        python3 -B tests/test_autonomy_sandbox.py
        cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DJASH_SOURCE_REVISION=@REVISION@
    '
printf '%s\n' 'JASH_VM_STAGE=modules' >/dev/ttyS0
run_as_runner 'cmake --build build --target modules --parallel 2'
printf '%s\n' 'JASH_VM_STAGE=original-gate' >/dev/ttyS0
run_as_runner '
        echo JASH_VM_ORIGINAL_GATE_BEGIN
        bash tests/test_autonomy.sh
        echo JASH_VM_ORIGINAL_GATE_END
    '
printf '%s\n' 'JASH_VM_STAGE=complete' >/dev/ttyS0
