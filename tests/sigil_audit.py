#!/usr/bin/env python3
"""Executable black-box audit for Jash sigil levels.

The audit deliberately exercises the built binary in fresh processes.  It does
not import Jash code and it never uses the real HOME or repository as a cwd.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable, Dict, List, Optional


SECRET = "sigil-audit-secret-" + os.urandom(12).hex()
HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")


def check(ok: bool, **details: object) -> Dict[str, object]:
    return {"status": "PASS" if ok else "FAIL", **details}


class Audit:
    def __init__(self, jash: Path, report_path: Optional[Path]) -> None:
        self.jash = jash.resolve()
        self.root = Path(tempfile.mkdtemp(prefix="jash-sigil-audit-"))
        self.module_root = self.jash.parent.parent
        self.results: List[Dict[str, object]] = []
        self.report_path = report_path
        self.cases: List[Path] = []

    def new_case(self, name: str, modules: bool = True) -> Dict[str, Path]:
        case = self.root / name
        home = case / "home"
        tmp = case / "tmp"
        repo = case / "repo"
        home.mkdir(parents=True)
        tmp.mkdir()
        repo.mkdir()
        (repo / ".git").mkdir()
        if modules:
            module_dir = home / ".jash" / "modules"
            module_dir.mkdir(parents=True)
            source_dir = self.module_root / "modules"
            for source in source_dir.glob("*/*.modjash"):
                shutil.copy2(source, module_dir / source.name)
            os.chmod(home / ".jash", 0o700)
            os.chmod(module_dir, 0o700)
            for module in module_dir.glob("*.modjash"):
                os.chmod(module, 0o600)
        self.cases.append(case)
        return {"case": case, "home": home, "tmp": tmp, "repo": repo}

    @staticmethod
    def snapshot(case: Path) -> Dict[str, bytes]:
        found: Dict[str, bytes] = {}
        for path in case.rglob("*"):
            if not path.is_file() and not path.is_symlink():
                continue
            rel = path.relative_to(case).as_posix()
            try:
                found[rel] = path.read_bytes()
            except OSError:
                found[rel] = b"<unreadable>"
        return found

    @staticmethod
    def text(data: bytes) -> str:
        return data.decode("utf-8", "replace")

    def env(self, ctx: Dict[str, Path], level: int) -> Dict[str, str]:
        # Do not inherit credentials or Jash configuration from the host.
        env = {
            "PATH": "/usr/bin:/bin",
            "LANG": "C",
            "LC_ALL": "C",
            "HOME": str(ctx["home"]),
            "TMPDIR": str(ctx["tmp"]),
            "USER": "sigil-audit",
            "LOGNAME": "sigil-audit",
            "JASH_SIGIL": str(level),
            "JASH_SIGIL_AUDIT_SECRET": SECRET,
            "JASH_NUDGE": "0",
            "JASH_BRIEF": "0",
            "JASH_LOOPGUARD": "off",
            "JASH_MEMO": "0",
            "JASH_BANNER": "0",
            "JASH_QUIET": "1",
        }
        return env

    def run(
        self,
        ctx: Dict[str, Path],
        name: str,
        level: int,
        command: str,
        expected_exit: int,
        receipt: Optional[Callable[[Dict[str, Path], str, str, int, Dict[str, bytes]], Dict[str, Dict[str, object]]]] = None,
    ) -> Dict[str, object]:
        before = self.snapshot(ctx["case"])
        stdout = ""
        stderr = ""
        exit_code: Optional[int] = None
        error: Optional[str] = None
        try:
            completed = subprocess.run(
                [str(self.jash), "-c", command],
                cwd=ctx["repo"],
                env=self.env(ctx, level),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=30,
                check=False,
            )
            exit_code = completed.returncode
            stdout = self.text(completed.stdout)
            stderr = self.text(completed.stderr)
        except subprocess.TimeoutExpired as exc:
            exit_code = -124
            stdout = self.text(exc.stdout or b"")
            stderr = self.text(exc.stderr or b"")
            error = "process timeout"
        except OSError as exc:
            error = str(exc)

        after = self.snapshot(ctx["case"])
        created = sorted(set(after) - set(before))
        receipt_checks: Dict[str, Dict[str, object]] = {}
        if receipt is not None:
            try:
                receipt_checks = receipt(ctx, stdout, stderr, exit_code or 0, after)
            except Exception as exc:  # Keep one broken probe from hiding others.
                receipt_checks = {"probe_exception": check(False, error=str(exc))}

        secret_bytes = SECRET.encode()
        leaked = [path for path, data in after.items() if secret_bytes in data]
        output_leak = SECRET in stdout or SECRET in stderr
        receipt_checks["no_raw_secret"] = check(
            not leaked and not output_leak,
            artifact_paths=leaked,
            output_leak=output_leak,
        )

        unexpected_repo = [
            path for path in created if path.startswith("repo/") and not path.startswith("repo/.git/")
        ]
        allowed = [
            path.startswith("home/") or path.startswith("tmp/") or path.startswith("repo/.git/")
            for path in created
        ]
        receipt_checks["sandbox_persistence"] = check(
            all(allowed) and not unexpected_repo,
            created_outside_expected=[path for path, ok in zip(created, allowed) if not ok],
            repo_mutations=unexpected_repo,
        )

        passed = exit_code == expected_exit and all(
            value.get("status") == "PASS" for value in receipt_checks.values()
        )
        record = {
            "probe": name,
            "level": level,
            "command": command,
            "exit": exit_code,
            "expected_exit": expected_exit,
            "stdout": stdout.replace(SECRET, "[REDACTED]"),
            "stderr": stderr.replace(SECRET, "[REDACTED]"),
            "files_created": created,
            "receipt_checks": receipt_checks,
            "status": "PASS" if passed and error is None else "FAIL",
        }
        if error:
            record["error"] = error.replace(SECRET, "[REDACTED]")
        self.results.append(record)
        return record

    @staticmethod
    def chain_paths(ctx: Dict[str, Path]) -> List[Path]:
        return sorted((ctx["home"] / ".jash").glob("chain*.log"))

    @staticmethod
    def ratchet_paths(ctx: Dict[str, Path]) -> List[Path]:
        return sorted((ctx["home"] / ".jash").glob("ratchet*.key"))

    def chain_receipt(
        self,
        marker: Optional[str] = None,
        return_code: Optional[int] = None,
        require_chain: bool = True,
    ) -> Callable[[Dict[str, Path], str, str, int, Dict[str, bytes]], Dict[str, Dict[str, object]]]:
        def verify(ctx: Dict[str, Path], stdout: str, stderr: str, exit_code: int, after: Dict[str, bytes]) -> Dict[str, Dict[str, object]]:
            chains = self.chain_paths(ctx)
            result: Dict[str, Dict[str, object]] = {}
            result["chain_created"] = check(bool(chains) == require_chain, paths=[p.name for p in chains])
            if not chains:
                return result
            data = chains[0].read_bytes()
            lines = data.splitlines(keepends=True)
            shaped = []
            for line in lines:
                text = self.text(line)
                shaped.append(
                    bool(line.endswith(b"\n"))
                    and "tag=" in text
                    and " rc=" in text
                    and " cmd=" in text
                    and len(text.split()) >= 7
                    and HEX64.match(text.split()[2]) is not None
                )
            result["chain_wire_format"] = check(bool(lines) and all(shaped), line_count=len(lines))
            if marker is not None:
                result["command_recorded"] = check(marker in self.text(data), marker=marker)
            if return_code is not None:
                result["exit_code_recorded"] = check(
                    f"rc={return_code}" in self.text(data), return_code=return_code
                )
            ratchets = self.ratchet_paths(ctx)
            ratchet_ok = False
            for path in ratchets:
                content = self.text(path.read_bytes()).strip().split()
                mode_ok = (path.stat().st_mode & 0o777) == 0o600
                ratchet_ok = ratchet_ok or (
                    len(content) in (2, 3)
                    and HEX64.match(content[0]) is not None
                    and content[1].isdigit()
                    and (len(content) == 2 or bool(re.match(r"^[0-9a-fA-F]{32}$", content[2])))
                    and mode_ok
                )
            result["ratchet_receipt"] = check(ratchet_ok, paths=[p.name for p in ratchets])
            return result

        return verify

    def proof_receipt(self, require_dsse: bool = False) -> Callable[[Dict[str, Path], str, str, int, Dict[str, bytes]], Dict[str, Dict[str, object]]]:
        def verify(ctx: Dict[str, Path], stdout: str, stderr: str, exit_code: int, after: Dict[str, bytes]) -> Dict[str, Dict[str, object]]:
            proof = ctx["home"] / ".jash" / "session.jashproof"
            result: Dict[str, Dict[str, object]] = {}
            if proof.exists():
                body = self.text(proof.read_bytes())
                fields = {
                    "jashproof/1": "jashproof/1" in body,
                    "pubkey": bool(re.search(r"^pubkey: [0-9a-fA-F]{64}$", body, re.MULTILINE)),
                    "signature": bool(re.search(r"^sig: [0-9a-fA-F]{128}$", body, re.MULTILINE)),
                    "algorithm": "sig_algo: ed25519" in body,
                    "chain_entries": bool(re.search(r"^chain_entries: [0-9]+$", body, re.MULTILINE)),
                }
                result["text_receipt"] = check(all(fields.values()), fields=fields)
                result["text_permissions"] = check((proof.stat().st_mode & 0o777) == 0o600)
            else:
                result["text_receipt"] = check(False, reason="session.jashproof missing")
            seed = ctx["home"] / ".jash" / "keys" / "ed25519.seed"
            result["signing_key"] = check(
                seed.is_file() and (seed.stat().st_mode & 0o777) == 0o600,
                generated_when_absent=True,
            )
            dsse = ctx["home"] / ".jash" / "session.jashproof.dsse"
            if require_dsse or dsse.exists():
                valid = False
                detail: object = "missing"
                if dsse.exists():
                    try:
                        envelope = json.loads(dsse.read_text())
                        valid = (
                            envelope.get("payloadType") == "application/vnd.jash.jashproof.v1+text"
                            and isinstance(envelope.get("payload"), str)
                            and len(envelope.get("signatures", [])) == 1
                            and len(envelope["signatures"][0].get("keyid", "")) == 64
                        )
                        detail = envelope
                    except (OSError, ValueError, KeyError, TypeError) as exc:
                        detail = str(exc)
                result["dsse_receipt"] = check(valid, detail=detail)
            return result

        return verify

    @staticmethod
    def json_verdict(stdout: str) -> Optional[Dict[str, object]]:
        for line in reversed(stdout.splitlines()):
            line = line.strip()
            if line.startswith("{"):
                try:
                    value = json.loads(line)
                    if isinstance(value, dict):
                        return value
                except ValueError:
                    pass
        return None

    def verify_receipt(self, expected_valid: bool) -> Callable[[Dict[str, Path], str, str, int, Dict[str, bytes]], Dict[str, Dict[str, object]]]:
        def verify(ctx: Dict[str, Path], stdout: str, stderr: str, exit_code: int, after: Dict[str, bytes]) -> Dict[str, Dict[str, object]]:
            verdict = self.json_verdict(stdout)
            valid = verdict is not None and verdict.get("valid") is expected_valid
            result: Dict[str, Dict[str, object]] = {"json_verdict": check(valid, verdict=verdict)}
            if expected_valid and verdict is not None:
                checks = verdict.get("checks", {})
                result["honest_receipt"] = check(
                    all(key in checks for key in ("hash_chain", "attested_seq_monotonicity", "ratchet_position"))
                    and "not_verified" in verdict
                    and "limits" in verdict,
                    fields=list(verdict),
                )
            return result

        return verify

    def execute(self) -> None:
        secret_cmd = "printf '%s' \"$JASH_SIGIL_AUDIT_SECRET\" >/dev/null; "

        off = self.new_case("off")
        self.run(
            off, "off_success", 0, secret_cmd + "printf OFF_SUCCESS; true", 0,
            self.chain_receipt(require_chain=False),
        )
        self.run(
            off, "off_failure", 0, secret_cmd + "printf OFF_FAILURE; false", 1,
            self.chain_receipt(require_chain=False),
        )

        log = self.new_case("log")
        self.run(log, "log_success", 1, secret_cmd + "printf LOG_SUCCESS; true", 0, self.chain_receipt("LOG_SUCCESS", 0))
        self.run(log, "log_failure", 1, secret_cmd + "printf LOG_FAILURE; false", 1, self.chain_receipt("LOG_FAILURE", 1))
        self.run(log, "log_verify", 0, "sigil verify --json", 0, self.verify_receipt(True))

        sign = self.new_case("sign")
        self.run(sign, "sign_success", 2, secret_cmd + "printf SIGN_SUCCESS; true", 0, self.chain_receipt("SIGN_SUCCESS", 0))
        self.run(sign, "sign_emit_proof", 0, "sigil proof emit", 0, self.proof_receipt())
        self.run(sign, "sign_verify_chain", 0, "sigil verify --json", 0, self.verify_receipt(True))
        self.run(sign, "sign_verify_proof", 0, "sigil proof verify", 0,
                 lambda c, o, e, x, a: {"proof_verification": check("chain_tip matches live chain" in o and ("signature" in o.lower() or "assinatura" in o.lower()))})
        self.run(sign, "sign_emit_dsse", 0, "sigil proof dsse", 0, self.proof_receipt(True))
        dsse_path = sign["home"] / ".jash" / "session.jashproof.dsse"
        self.run(sign, "sign_verify_dsse", 0, f"sigil proof verify {dsse_path}", 0,
                 lambda c, o, e, x, a: {"dsse_verification": check(("signature" in o.lower() or "assinatura" in o.lower()) and "invalid" not in o.lower())})

        seal = self.new_case("seal")
        self.run(seal, "seal_success", 3, secret_cmd + "printf SEAL_SUCCESS; true", 0, self.chain_receipt("SEAL_SUCCESS", 0))
        self.run(seal, "seal_failure", 3, secret_cmd + "printf SEAL_FAILURE; false", 1, self.chain_receipt("SEAL_FAILURE", 1))
        self.run(seal, "seal_emit_proof", 0, "sigil proof emit", 0, self.proof_receipt())
        self.run(seal, "seal_verify", 0, "sigil verify --json", 0, self.verify_receipt(True))

        transition = self.new_case("transitions")
        transition_command = (
            "sigil off\nprintf TRANS_OFF\nsigil log\nprintf TRANS_LOG\n"
            "sigil sign\nprintf TRANS_SIGN\nsigil seal\nprintf TRANS_SEAL\nfalse"
        )
        def transition_receipt(ctx: Dict[str, Path], stdout: str, stderr: str, exit_code: int, after: Dict[str, bytes]) -> Dict[str, Dict[str, object]]:
            chain = self.chain_paths(ctx)
            data = chain[0].read_text() if chain else ""
            return {
                "transition_output": check(all(token in stdout for token in ("TRANS_OFF", "TRANS_LOG", "TRANS_SIGN", "TRANS_SEAL"))),
                "off_not_recorded": check("TRANS_OFF" not in data),
                "later_levels_recorded": check(all(token in data for token in ("TRANS_LOG", "TRANS_SIGN", "TRANS_SEAL"))),
            }
        self.run(transition, "runtime_level_transitions", 1, transition_command, 1, transition_receipt)

        missing_module = self.new_case("missing-module", modules=False)
        self.run(
            missing_module, "missing_module_guardrail", 1, secret_cmd + "printf SHOULD_NOT_RUN; true", 125,
            lambda c, o, e, x, a: {
                "refusal": check("refusing to run without attestation module" in e),
                "command_not_run": check("SHOULD_NOT_RUN" not in o),
                "no_chain": check(not self.chain_paths(c)),
            },
        )

        missing_ratchet = self.new_case("missing-ratchet")
        self.run(missing_ratchet, "missing_ratchet_seed", 1, "true", 0, self.chain_receipt(require_chain=True))
        chains = self.chain_paths(missing_ratchet)
        chain_before = hashlib.sha256(chains[0].read_bytes()).hexdigest() if chains else ""
        for path in self.ratchet_paths(missing_ratchet):
            path.unlink()
        def missing_ratchet_receipt(ctx: Dict[str, Path], stdout: str, stderr: str, exit_code: int, after: Dict[str, bytes]) -> Dict[str, Dict[str, object]]:
            current = self.chain_paths(ctx)
            current_hash = hashlib.sha256(current[0].read_bytes()).hexdigest() if current else ""
            return {
                "append_blocked": check("missing_scoped_ratchet" in stdout or "missing_scoped_ratchet" in stderr),
                "chain_preserved": check(current_hash == chain_before),
                "ratchet_still_absent": check(not self.ratchet_paths(ctx)),
            }
        self.run(missing_ratchet, "missing_scoped_ratchet_guardrail", 1, "printf GUARD_MISSING_RATCHET; true", 0, missing_ratchet_receipt)

        missing_proof = self.new_case("missing-proof")
        self.run(missing_proof, "missing_proof_seed", 2, "true", 0, self.chain_receipt(require_chain=True))
        self.run(missing_proof, "missing_proof_emit_seed", 0, "sigil proof emit", 0, self.proof_receipt())
        proof = missing_proof["home"] / ".jash" / "session.jashproof"
        if proof.exists():
            proof.unlink()
        self.run(
            missing_proof, "missing_attestation_guardrail", 0, "sigil proof verify", 1,
            lambda c, o, e, x, a: {
                "refusal": check("cannot open" in o.lower()),
                "receipt_absent": check(not (c["home"] / ".jash" / "session.jashproof").exists()),
            },
        )

        missing_key = self.new_case("invalid-signing-key")
        self.run(missing_key, "invalid_key_seed", 1, "true", 0, self.chain_receipt(require_chain=True))
        key_dir = missing_key["home"] / ".jash" / "keys"
        key_dir.mkdir(parents=True, exist_ok=True)
        (key_dir / "ed25519.seed").mkdir()
        self.run(
            missing_key, "invalid_signing_key_guardrail", 2, "sigil proof emit", 1,
            lambda c, o, e, x, a: {
                "refusal": check("could not load Ed25519 signing key" in o),
                "seed_not_exposed": check((key_dir / "ed25519.seed").is_dir()),
            },
        )

    def report(self) -> Dict[str, object]:
        failures = [result["probe"] for result in self.results if result["status"] != "PASS"]
        return {
            "schema": "jash-sigil-audit/v1",
            "binary": str(self.jash),
            "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "probe_count": len(self.results),
            "status": "PASS" if not failures else "FAIL",
            "failures": failures,
            "probes": self.results,
        }

    def close(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the isolated Jash sigil audit")
    root = Path(__file__).resolve().parents[1]
    parser.add_argument("--jash", type=Path, default=root / "build" / "jash")
    parser.add_argument("--report", type=Path, help="write JSON to this path instead of stdout")
    args = parser.parse_args()
    audit = Audit(args.jash, args.report)
    try:
        if not audit.jash.is_file() or not os.access(audit.jash, os.X_OK):
            raise SystemExit(f"jash binary is not executable: {audit.jash}")
        audit.execute()
        document = audit.report()
        encoded = json.dumps(document, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(encoded)
        else:
            sys.stdout.write(encoded)
        return 0 if document["status"] == "PASS" else 1
    finally:
        audit.close()


if __name__ == "__main__":
    raise SystemExit(main())
