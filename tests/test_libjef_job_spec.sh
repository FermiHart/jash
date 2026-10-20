#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${JASH_BUILD_DIR:-"$ROOT/build"}
BIN=${JASH_AGENTD_BIN:-"$BUILD/jash-agentd"}
VERIFY=${JASH_VERIFIER_BIN:-"$BUILD/jef_verify"}
PYTHON_BIN=${PYTHON_BIN:-python3}

if [[ ${JASH_SKIP_BUILD:-0} != 1 ]]; then
    cmake --build "$BUILD" --target jash-agentd jef_verify -j4
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

"$PYTHON_BIN" - "$BIN" "$VERIFY" "$TMP" "$ROOT" <<'PY'
import base64
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

binary, verifier, root, source = sys.argv[1:]
root = Path(root)
home = root / "home"
home.mkdir()
proc = subprocess.Popen(
    [binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.PIPE, text=True, env={**os.environ, "HOME": str(home)},
)
checks = 0
api_checks = 12

def check(label, args, expected=0):
    global checks
    result = subprocess.run([verifier, "job-spec", *map(str, args)],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == expected, (label, result.returncode,
                                           result.stdout, result.stderr)
    checks += 1

def write(name, data):
    path = root / name
    path.write_bytes(data.encode() if isinstance(data, str) else data)
    return path

def wire(body):
    return json.dumps(body, separators=(",", ":"))

def exchange(body, encoded=None):
    encoded = wire(body) if encoded is None else encoded
    proc.stdin.write(encoded + "\n")
    proc.stdin.flush()
    raw = proc.stdout.readline()
    assert raw, "agent exited without a response"
    response = json.loads(raw)
    assert "receipt_ref" in response, response
    return response, [write(body["id"] + "-request.json", encoded + "\n"),
                      write(body["id"] + "-response.json", raw),
                      home / ".jash/agent_receipts" / response["receipt_ref"]]

def job(name, spec, *, digest=True, env=None, rc=0, encoded=None):
    body = {"id": name, "protocol_version": 2, "op": "job.start",
            "argv": [sys.executable, "-c",
                     f"import os;os.write(1,b'OUT\\x00');os.write(2,b'ERR');exit({rc})"],
            "background": True, "caps": ["effect:exec", "job:create"]}
    if digest:
        body["execution_spec_sha256"] = "sha256:" + hashlib.sha256(spec).hexdigest()
    if env is not None:
        body["env"] = env
    start, start_paths = exchange(body, encoded(body) if encoded else None)
    terminal, terminal_paths = exchange({
        "id": name + "-wait", "protocol_version": 2, "op": "job.wait",
        "job_id": start["job_id"], "wait_ms": 5000, "caps": ["job:read"],
    })
    assert terminal["terminal"] and terminal["quiescent"], terminal
    return [pubkey, write(name + ".spec", spec), *start_paths, *terminal_paths]

try:
    handshake = json.loads(proc.stdout.readline())
    pubkey = handshake["receipts"]["pubkey"]
    spec = b'{"version":"caller/v1","context":"original","expected_rc":99}\n'
    args = job("bound", spec)
    # Prove real start/terminal fixtures work before testing the new interface.
    for phase, paths in (("start", args[2:5]), ("terminal", args[5:8])):
        subprocess.run([verifier, "job-receipt", pubkey, phase, *map(str, paths)],
                       check=True, capture_output=True, timeout=10)
    check("exact opaque spec and real receipts", args)

    check("missing CLI argument", args[:-1], 2)
    check("extra CLI argument", [*args, "extra"], 2)
    for key in ("", "0" * 63, "g" * 64, "0" * 65):
        check("malformed key", [key, *args[1:]], 2)
    check("foreign key", ["0" * 64, *args[1:]], 1)
    check("uppercase expected key", [pubkey.upper(), *args[1:]])
    for name, changed in (
        ("context", spec.replace(b"original", b"tampered")),
        ("whitespace", spec + b" "), ("newline", spec[:-1]),
        ("nul", spec + b"\x00"), ("substitution", b"another contract"),
    ):
        check(name + " substitution", [pubkey, write(name + ".spec", changed),
                                      *args[2:]], 1)
    check("missing spec", [pubkey, root / "absent", *args[2:]], 2)
    check("directory spec", [pubkey, root, *args[2:]], 2)
    os.mkfifo(root / "fifo")
    check("FIFO spec fails without blocking", [pubkey, root / "fifo", *args[2:]], 2)

    check("missing digest in valid signed start", job("missing", spec, digest=False), 1)
    check("nested digest is not a request digest", job(
        "nested", spec, digest=False,
        env={"execution_spec_sha256": "sha256:" + hashlib.sha256(spec).hexdigest()}), 1)
    check("escaped digest key", job("escaped", spec, encoded=lambda body:
          wire(body).replace('"execution_spec_sha256"', '"execution_spec_\\u0073ha256"')))
    check("nonzero rc is provenance, not failure", job("nonzero", spec, rc=7))
    check("binary opaque contract", job("binary", b"\xff\x00caller/v2\r\n"))
    check("empty opaque contract", job("empty", b""))
    check("exact 1 MiB allowed", job("limit", b"x" * (1024 * 1024)))
    check("1 MiB plus one rejected even with matching digest",
          job("oversize", b"x" * (1024 * 1024 + 1)), 2)

    other = job("other", spec)
    check("different job's valid start", [*other[:5], *args[5:]], 1)
    check("different job's valid terminal", [*args[:5], *other[5:]], 1)
    check("terminal request is not the start request", [*args[:5], args[2], *args[6:]], 1)
    check("nonterminal start cannot stand in for terminal", [*args[:5], *args[2:5]], 1)
    start = json.loads(args[3].read_text())
    for op, extra in (("job.get", {}), ("job.cancel", {"grace_ms": 0})):
        _, paths = exchange({"id": op, "protocol_version": 2, "op": op,
                             "job_id": start["job_id"], **extra,
                             "caps": ["job:signal" if op == "job.cancel" else "job:read"]})
        check(op + " terminal phase", [*args[:5], *paths])

    live, live_paths = exchange({
        "id": "live", "protocol_version": 2, "op": "job.start",
        "argv": [sys.executable, "-c", "import time;time.sleep(30)"],
        "execution_spec_sha256": "sha256:" + hashlib.sha256(spec).hexdigest(),
        "background": True, "caps": ["effect:exec", "job:create"],
    })
    exchange({"id": "kill-live", "protocol_version": 2, "op": "job.cancel",
              "job_id": live["job_id"], "grace_ms": 0, "caps": ["job:signal"]})
    cancelled, cancel_paths = exchange({
        "id": "wait-live", "protocol_version": 2, "op": "job.wait",
        "job_id": live["job_id"], "wait_ms": 5000, "caps": ["job:read"],
    })
    assert cancelled["terminal"] and cancelled["rc"] != 0 and cancelled["cancel_requested"]
    check("cancelled nonzero job is valid provenance", [*args[:2], *live_paths, *cancel_paths])

    for index in (2, 3, 4, 5, 6, 7):
        original = args[index].read_bytes()
        try:
            args[index].write_bytes(original + b'\x00{"trailing":true}\n')
            check("tampered artifact " + str(index), args, 1)
        finally:
            args[index].write_bytes(original)
    terminal = json.loads(args[6].read_text())
    cas = home / ".jash/overflow" / terminal["stdout_ref"]
    original = cas.read_bytes()
    try:
        cas.write_bytes(b"X" + original[1:])
        check("tampered CAS stream", args, 1)
    finally:
        cas.write_bytes(original)

    # Use libjef itself, not a second crypto implementation, to create signed
    # adversarial starts and exercise the public API's return contract.
    library = root / "libjef-test.so"
    subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC", "-O2",
                    "-I" + source + "/c", "-I" + source + "/libjef",
                    *[source + "/" + path for path in
                      ("libjef/jef_verify.c", "libjef/ed25519.c",
                       "c/module/jash_crypto.c", "c/module/blake3.c")],
                    "-o", str(library)], check=True)
    lib = ctypes.CDLL(str(library))
    lib.jef_verify_job_spec.argtypes = [ctypes.c_char_p] * 8
    lib.jef_verify_job_spec.restype = ctypes.c_int
    lib.jash_blake3.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
    lib.ed25519_sign.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_uint32, ctypes.c_void_p]
    lib.jef_proof_pubkey_hex.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_uint]
    secret = (home / ".jash/keys/ed25519.seed").read_bytes() + bytes.fromhex(pubkey)

    def blake3(data):
        out = ctypes.create_string_buffer(32)
        lib.jash_blake3(data, len(data), out)
        return out.raw.hex()

    def sign_response(response, payload, envelope):
        base = {k: v for k, v in response.items() if not k.startswith("receipt_")}
        payload["response_blake3"] = blake3((wire(base) + "\n").encode())
        encoded = wire(payload).encode()
        media = envelope["payloadType"].encode()
        pae = b"DSSEv1 %d " % len(media) + media + b" %d " % len(encoded) + encoded
        sig = ctypes.create_string_buffer(64)
        assert lib.ed25519_sign(sig, pae, len(pae), secret) == 0
        envelope["payload"] = base64.b64encode(encoded).decode()
        envelope["signatures"][0]["sig"] = base64.b64encode(sig.raw).decode()
        receipt = (wire(envelope) + "\n").encode()
        response["receipt_blake3"] = blake3(receipt)
        return receipt

    def signed_start(name, request, response=None):
        response = dict(start if response is None else response)
        envelope = json.loads(args[4].read_text())
        payload = json.loads(base64.b64decode(envelope["payload"]))
        payload["request_blake3"] = blake3(request.encode())
        receipt = sign_response(response, payload, envelope)
        response["receipt_ref"] = payload["request_blake3"] + ".job-start.dsse"
        directory = root / name
        directory.mkdir()
        receipt_path = directory / response["receipt_ref"]
        receipt_path.write_bytes(receipt)
        return [write(name + "-request", request + "\n"),
                write(name + "-response", wire(response) + "\n"), receipt_path]

    def linked_terminal(name, start_paths):
        response = dict(terminal)
        envelope = json.loads(args[7].read_text())
        payload = json.loads(base64.b64decode(envelope["payload"]))
        new_start = json.loads(start_paths[1].read_text())
        link = {"start_request_blake3": blake3(start_paths[0].read_bytes()[:-1]),
                "start_receipt_ref": new_start["receipt_ref"],
                "start_receipt_blake3": new_start["receipt_blake3"]}
        response.update(link)
        payload.update(link)
        receipt = sign_response(response, payload, envelope)
        response["receipt_ref"] = (payload["request_blake3"] + "." +
                                   payload["response_blake3"] + ".job-terminal.dsse")
        receipt_path = start_paths[2].parent / response["receipt_ref"]
        receipt_path.write_bytes(receipt)
        return [args[5], write(name + "-terminal", wire(response) + "\n"), receipt_path]

    (root / "overflow").symlink_to(home / ".jash/overflow", target_is_directory=True)

    request = args[2].read_text().removesuffix("\n")
    # Each alternate start independently verifies and has the SAME job ID.
    # One differs only in receipt bytes; one also has a new request/ref.
    for name, changed_request, response in (
        ("same-job-hash", request, {**start, "id": "another-start"}),
        ("same-job-ref", request + " ", start),
    ):
        paths = signed_start(name, changed_request, response)
        subprocess.run([verifier, "job-receipt", pubkey, "start", *map(str, paths)],
                       check=True, capture_output=True)
        check(name, [*args[:2], *paths, *args[5:]], 1)

    for name, changed in (
        ("linked-control", request + " "),
        ("duplicate", request[:-1] + ',"execution_spec_sha256":"sha256:' + "0" * 64 + '"}'),
        ("escaped-duplicate", request[:-1] + ',"execution_spec_\\u0073ha256":"sha256:' + "0" * 64 + '"}'),
        ("identical-duplicate", request[:-1] + ',"execution_spec_sha256":"sha256:' + hashlib.sha256(spec).hexdigest() + '"}'),
        ("wrong-digest-type", wire({**json.loads(request), "execution_spec_sha256": None})),
        ("bad-digest-prefix", request.replace("sha256:", "SHA256:")),
        ("short-digest", request.replace(hashlib.sha256(spec).hexdigest(), "0")),
        ("malformed", request[:-1] + ',}'),
    ):
        paths = signed_start(name, changed)
        terminal_paths = linked_terminal(name, paths)
        # Duplicate spec digests are not checked by the phase verifier, but
        # all signatures, job IDs, hashes, and links here independently pass.
        if name != "malformed":
            for phase, phase_paths in (("start", paths), ("terminal", terminal_paths)):
                subprocess.run([verifier, "job-receipt", pubkey, phase,
                                *map(str, phase_paths)], check=True, capture_output=True)
        check(name + " signed request", [*args[:2], *paths, *terminal_paths],
              0 if name == "linked-control" else 1)

    # No signing here: retain the authentic signature, hide its payload under
    # a shadow member, and repair only public response/receipt hashes.
    authentic = json.loads(args[7].read_text())
    forged_response = dict(terminal, rc=23)
    forged_payload = json.loads(base64.b64decode(authentic["payload"]))
    forged_payload["rc"] = 23
    base = {k: v for k, v in forged_response.items() if not k.startswith("receipt_")}
    forged_payload["response_blake3"] = blake3((wire(base) + "\n").encode())
    forged_response["receipt_ref"] = (forged_payload["request_blake3"] + "." +
                                       forged_payload["response_blake3"] + ".job-terminal.dsse")
    forged = base64.b64encode(wire(forged_payload).encode()).decode()
    shadow_results = []
    for top_forged in (True, False):
        for nested_first in (False, True):
            name = f"dsse-shadow-{top_forged}-{nested_first}"
            top_key = '"pay\\u006coad"' if top_forged else '"payload"'
            nested_key = '"payload"' if top_forged else '"pay\\u006coad"'
            top = top_key + ":" + wire(forged if top_forged else authentic["payload"])
            nested = '"shadow":{' + nested_key + ":" + wire(authentic["payload"] if top_forged else forged) + "}"
            members = [nested, top] if nested_first else [top, nested]
            text = ('{"payloadType":' + wire(authentic["payloadType"]) + "," +
                    ",".join(members) + ',"signatures":' + wire(authentic["signatures"]) + "}\n")
            response = dict(forged_response if top_forged else terminal)
            response["receipt_blake3"] = blake3(text.encode())
            receipt_path = args[7].parent / response["receipt_ref"]
            original = receipt_path.read_bytes() if receipt_path.exists() else None
            try:
                receipt_path.write_text(text)
                response_path = write(name + "-response", wire(response) + "\n")
                commands = [
                    ["proof-pinned", pubkey, receipt_path],
                    ["job-receipt", pubkey, "terminal", args[5], response_path, receipt_path],
                    ["job-spec", *args[:5], args[5], response_path, receipt_path],
                ]
                results = [subprocess.run([verifier, *map(str, command)],
                                          capture_output=True, timeout=10).returncode
                           for command in commands]
                key = ctypes.create_string_buffer(65)
                results.append(lib.jef_proof_pubkey_hex(os.fsencode(receipt_path), key, len(key)))
                shadow_results.append((name, results))
            finally:
                if original is not None:
                    receipt_path.write_bytes(original)
    print("DSSE shadow rejection (proof/job/spec/key):", shadow_results, flush=True)
    assert all(results == [1, 1, 1, 1] for _, results in shadow_results), shadow_results
    checks += 12
    api_checks += 4

    def proof_check(name, text, expected=0, key_expected=None):
        global checks, api_checks
        path = write(name + ".dsse", text)
        for command in (["proof", path], ["proof-pinned", pubkey, path]):
            result = subprocess.run([verifier, *map(str, command)],
                                    capture_output=True, timeout=10)
            assert result.returncode == expected, (name, command, result.stdout)
            checks += 1
        out = ctypes.create_string_buffer(65)
        rc = lib.jef_proof_pubkey_hex(os.fsencode(path), out, len(out))
        assert rc == (expected if key_expected is None else key_expected), (name, rc)
        if rc == 0:
            assert out.value.decode() == pubkey, (name, out.value)
        api_checks += 1

    proof_check("standard-job-envelope", wire(authentic))
    proof_check("unsigned-top-level-substitution", wire({**authentic, "payload": forged}),
                1, key_expected=0)
    proof_check("unsigned-type-substitution", wire({**authentic, "payloadType": "foreign/type"}),
                1, key_expected=0)
    # Escapes and member order are valid JSON, not a reason to reject an
    # otherwise unambiguous, authentic envelope.
    aliases = {"payloadType": "payload\\u0054ype", "payload": "pay\\u006coad",
               "signatures": "signat\\u0075res", "keyid": "key\\u0069d", "sig": "s\\u0069g"}
    escaped = wire({"signatures": authentic["signatures"],
                    "payload": authentic["payload"], "payloadType": authentic["payloadType"]})
    for key, alias in aliases.items():
        escaped = escaped.replace(wire(key), '"' + alias + '"')
    proof_check("escaped-envelope", escaped)

    signature = authentic["signatures"][0]
    alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    noncanonical_sig = signature["sig"][:-3] + alphabet[alphabet.index(signature["sig"][-3]) | 1] + "=="
    bad_envelopes = [
        ("empty-signatures", wire({**authentic, "signatures": []})),
        ("two-signatures", wire({**authentic, "signatures": [signature, signature]})),
        ("object-not-array", wire({**authentic, "signatures": signature})),
        ("nested-signature", wire({**authentic, "signatures": [[signature]]})),
        ("shadow-signature", wire({**authentic, "signatures": [{"shadow": signature}]})),
        ("signature-extra-member", wire({**authentic, "signatures": [{**signature, "shadow": signature}]})),
        ("nested-envelope", wire({"shadow": authentic})),
        ("nul-envelope", wire(authentic) + '\x00{"trailing":true}'),
        ("trailing-envelope", wire(authentic) + "{}"),
        ("trailing-comma", wire(authentic)[:-1] + ",}"),
        ("oversized-envelope", wire(authentic) + " " * 8192),
        ("short-key", wire({**authentic, "signatures": [{**signature, "keyid": "0"}]})),
        ("long-key", wire({**authentic, "signatures": [{**signature, "keyid": pubkey + "0"}]})),
        ("bad-key", wire({**authentic, "signatures": [{**signature, "keyid": "g" * 64}]})),
        ("bad-base64-payload", wire({**authentic, "payload": authentic["payload"] + "!"})),
        ("bad-base64-sig", wire({**authentic, "signatures": [{**signature, "sig": signature["sig"] + "!"}]})),
        ("noncanonical-base64-sig", wire({**authentic, "signatures": [{**signature, "sig": noncanonical_sig}]})),
        ("unpadded-base64-sig", wire({**authentic, "signatures": [{**signature, "sig": signature["sig"].rstrip("=")}]})),
    ]
    for key in ("payloadType", "payload", "signatures"):
        bad_envelopes.append(("missing-" + key, wire({k: v for k, v in authentic.items() if k != key})))
        for alias in (key, aliases[key]):
            bad_envelopes.append(("duplicate-" + alias,
                                  wire(authentic)[:-1] + ',"' + alias + '":' + wire(authentic[key]) + "}"))
    for key in ("keyid", "sig"):
        for alias in (key, aliases[key]):
            duplicated = wire(signature)[:-1] + ',"' + alias + '":' + wire(signature[key]) + "}"
            bad_envelopes.append(("duplicate-" + alias,
                                  wire(authentic).replace(wire(signature), duplicated)))
    for name, text in bad_envelopes:
        proof_check(name, text, 1)

    # The same shared decoder also protects runtime receipts. Changing an
    # envelope's spelling alone remains valid once its public hash is updated.
    runtime, runtime_paths = exchange({"id": "runtime-proof", "argv": ["echo", "OK"],
                                       "caps": ["effect:read"]})
    runtime_envelope = json.loads(runtime_paths[2].read_text())
    proof_check("standard-runtime-envelope", wire(runtime_envelope))
    for phase, paths, response in (("terminal", args[5:8], terminal),
                                    ("runtime", runtime_paths, runtime)):
        original = paths[2].read_bytes()
        original_response = paths[1].read_bytes()
        hash_member = b'"receipt_blake3":' + wire(response["receipt_blake3"]).encode()
        assert original_response.count(hash_member) == 1
        envelope = json.loads(original)
        for shadow in (False, True):
            text = wire(envelope).replace('"payload"', '"pay\\u006coad"')
            if shadow:
                text = text[:-1] + ',"shadow":{"payload":' + wire(envelope["payload"]) + "}}"
            changed_response = {**response, "receipt_blake3": blake3(text.encode())}
            # Only the public receipt hash changes. Re-encoding the entire
            # response would also change signed bytes (for example \\u000a to
            # \\n in stdout), which the independent verifier must reject.
            changed_raw = original_response.replace(
                hash_member,
                b'"receipt_blake3":' + wire(changed_response["receipt_blake3"]).encode(),
                1,
            )
            assert json.loads(changed_raw) == changed_response
            try:
                paths[2].write_text(text)
                response_path = write(f"{phase}-shadow-{shadow}", changed_raw)
                command = (["runtime-receipt", pubkey] if phase == "runtime" else
                           ["job-receipt", pubkey, phase])
                result = subprocess.run([verifier, *command, str(paths[0]), str(response_path),
                                         str(paths[2])], capture_output=True, timeout=10)
                assert result.returncode == int(shadow), (phase, shadow, result.stdout)
                checks += 1
                if phase == "terminal":
                    check("escaped/shadow job spec", [*args[:5], paths[0], response_path, paths[2]], int(shadow))
                elif not shadow:
                    # Equivalent JSON values are not equivalent signed bytes.
                    assert changed_raw.count(b'"stdout":"OK') == 1
                    reencoded = changed_raw.replace(b'"stdout":"OK',
                                                    b'"stdout":"\\u004fK', 1)
                    assert json.loads(reencoded) == changed_response
                    altered_path = write("runtime-response-reencoded", reencoded)
                    altered = subprocess.run(
                        [verifier, *command, str(paths[0]), str(altered_path), str(paths[2])],
                        capture_output=True, timeout=10)
                    assert altered.returncode == 1, altered.stdout
                    checks += 1
            finally:
                paths[2].write_bytes(original)

    # Preserve the shipped text proof format as well as ordinary DSSE text.
    text_body = ("jashproof/1\nchain_entries: 0\nchain_tip: " + "0" * 64 +
                 "\nsig_algo: ed25519\npubkey: " + pubkey + "\nuser: fixture\n").encode()
    sig = ctypes.create_string_buffer(64)
    assert lib.ed25519_sign(sig, text_body, len(text_body), secret) == 0
    text_proof = text_body + b"sig: " + sig.raw.hex().encode() + b"\n"
    proof_check("text-proof", text_proof)
    proof_check("tampered-text-proof", text_proof.replace(b"fixture", b"changed"), 1, key_expected=0)
    text_envelope = {"payloadType": "application/vnd.jash.jashproof.v1+text",
                     "payload": base64.b64encode(text_body).decode(), "signatures": [dict(signature)]}
    media = text_envelope["payloadType"].encode()
    pae = b"DSSEv1 %d " % len(media) + media + b" %d " % len(text_body) + text_body
    assert lib.ed25519_sign(sig, pae, len(pae), secret) == 0
    text_envelope["signatures"][0]["sig"] = base64.b64encode(sig.raw).decode()
    proof_check("standard-text-envelope", wire(text_envelope))

    def api(values):
        return lib.jef_verify_job_spec(*[None if v is None else os.fsencode(v) for v in values])

    assert api(args) == 0
    assert api(["g" * 64, *args[1:]]) == -2
    assert api(["0" * 64, *args[1:]]) == 1
    assert api([pubkey, root / "absent", *args[2:]]) == -1
    for index in range(8):
        changed = list(args)
        changed[index] = None
        assert api(changed) == (-2 if index == 0 else -1)
    print(f"libjef job spec: PASS ({checks} CLI checks, {api_checks} API checks)")
finally:
    proc.stdin.close()
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    proc.stdout.close()
    proc.stderr.close()
PY
