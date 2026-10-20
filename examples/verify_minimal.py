#!/usr/bin/env python3
"""verify_minimal.py — a from-scratch reference verifier for jash attestation,
following ../SPEC.md. Proves the format is interoperable (not jash-specific):
no jash, no libjef — just standard crypto libraries.

    pip install blake3 pynacl
    ./verify_minimal.py chain session-01/chain.log
    ./verify_minimal.py proof session-01/session.jashproof
    ./verify_minimal.py proof session-01/session.jashproof.dsse

NOTE: the canonical, CI-tested verifier is `jef_verify` (tests/test_libjef_verify.sh).
This file is a spec reference for porting to any language.
"""
import sys, json, base64, re
from blake3 import blake3
from nacl.signing import VerifyKey
from nacl.exceptions import BadSignatureError

def verify_chain(path):
    prev = b"\x00" * 32
    n = 0
    for line in open(path, "rb"):
        line = line.rstrip(b"\n")
        if not line:
            continue
        # "<seq> <sec>.<nsec> <hash> tag=<tag> rc=<rc> cmd=<text>"
        m = re.match(rb"(\d+) (\d+)\.(\d+) ([0-9a-f]{64}) tag=[0-9a-f]{64} rc=(-?\d+) cmd=(.*)$", line, re.S)
        if not m:
            return f"malformed line near #{n}"
        sec, nsec, rec_hash, rc, cmd = int(m[2]), int(m[3]), m[4], int(m[5]), m[6]
        ts_ns = sec * 1_000_000_000 + nsec
        payload = ts_ns.to_bytes(8, "big") + rc.to_bytes(4, "little", signed=True) + cmd
        h = blake3(prev + payload).digest()
        if h.hex().encode() != rec_hash:
            return f"CHAIN BROKEN at seq {int(m[1])}"
        prev = h
        n += 1
    return None if n else "empty chain"

def _ed25519_ok(pub_hex, msg, sig):
    try:
        VerifyKey(bytes.fromhex(pub_hex)).verify(msg, sig)
        return True
    except (BadSignatureError, ValueError):
        return False

def verify_proof(path):
    raw = open(path, "rb").read()
    s = raw.lstrip()
    if s[:1] == b"{":                                   # DSSE envelope
        env = json.loads(raw)
        ptype = env["payloadType"].encode()
        payload = base64.b64decode(env["payload"])
        sigb = base64.b64decode(env["signatures"][0]["sig"])
        keyid = env["signatures"][0]["keyid"]
        pae = (b"DSSEv1 " + str(len(ptype)).encode() + b" " + ptype + b" "
               + str(len(payload)).encode() + b" " + payload)
        return _ed25519_ok(keyid, pae, sigb)
    # text jashproof/1: body = bytes up to and including the '\n' before "sig: "
    idx = raw.find(b"\nsig: ")
    if idx < 0:
        return False
    body = raw[:idx + 1]
    sig_hex = raw[idx + 6:].split(b"\n", 1)[0].strip()
    if b"sig_algo: ed25519" not in body:
        return False                                    # sovereign verifier: ed25519 only
    pk = re.search(rb"pubkey: ([0-9a-f]{64})", body)
    if not pk:
        return False
    return _ed25519_ok(pk[1].decode(), body, bytes.fromhex(sig_hex.decode()))

def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("chain", "proof"):
        print("usage: verify_minimal.py <chain|proof> <file>", file=sys.stderr)
        return 2
    kind, path = sys.argv[1], sys.argv[2]
    if kind == "chain":
        err = verify_chain(path)
        print("chain OK" if err is None else f"chain BROKEN: {err}")
        return 0 if err is None else 1
    ok = verify_proof(path)
    print("proof VALID (ed25519, pubkey-only)" if ok else "proof INVALID")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
