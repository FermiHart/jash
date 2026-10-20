#!/usr/bin/env python3
"""Phase gate: 1,000 independently verified persistent-runtime requests."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import select
import statistics
import subprocess
import tempfile
import time
from typing import BinaryIO
from proc_observation import direct_children


def sealed(payload: bytes, label: str) -> int:
    required = (
        fcntl.F_SEAL_WRITE
        | fcntl.F_SEAL_GROW
        | fcntl.F_SEAL_SHRINK
        | fcntl.F_SEAL_SEAL
    )
    fd = os.memfd_create(label, os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
    try:
        view = memoryview(payload)
        while view:
            written = os.write(fd, view)
            if written <= 0:
                raise RuntimeError(f"short write while sealing {label}")
            view = view[written:]
        os.lseek(fd, 0, os.SEEK_SET)
        fcntl.fcntl(fd, fcntl.F_ADD_SEALS, required)
        return fd
    except Exception:
        os.close(fd)
        raise


def read_line(stream: BinaryIO, label: str, timeout: float = 10.0) -> bytes:
    ready, _, _ = select.select([stream.fileno()], [], [], timeout)
    if not ready:
        raise RuntimeError(f"{label} response deadline exceeded")
    line = stream.readline()
    if not line:
        raise RuntimeError(f"{label} closed its response channel")
    return line


def read_fd(fd: int, limit: int) -> bytes:
    chunks: list[bytes] = []
    total = 0
    while True:
        chunk = os.read(fd, min(8192, limit + 1 - total))
        if not chunk:
            return b"".join(chunks)
        chunks.append(chunk)
        total += len(chunk)
        if total > limit:
            raise RuntimeError(f"artifact exceeds {limit} bytes")


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def write_report(path: Path, rendered: str) -> None:
    temporary = path.with_name(f".{path.name}.tmp.{os.getpid()}")
    try:
        with temporary.open("x", encoding="utf-8") as output:
            output.write(rendered + "\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        directory_fd = os.open(path.parent, os.O_RDONLY | os.O_CLOEXEC)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        temporary.unlink(missing_ok=True)


def rss_kb(pid: int) -> int:
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    raise RuntimeError(f"VmRSS absent for pid {pid}")


def fd_count(pid: int) -> int:
    return len(list(Path(f"/proc/{pid}/fd").iterdir()))


def children(pid: int) -> str:
    return " ".join(str(child) for child in direct_children(pid))


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def stop_process(process: subprocess.Popen[bytes] | None) -> None:
    if process is None:
        return
    if process.stdin is not None and not process.stdin.closed:
        try:
            process.stdin.close()
        except OSError:
            pass
    if process.poll() is not None:
        return
    try:
        process.wait(timeout=5)
        return
    except subprocess.TimeoutExpired:
        process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)


def workload(index: int, cwd: Path, counter: Path) -> tuple[dict, str, bool]:
    number = index + 1
    base = {"id": f"soak-{index:06d}", "cwd": str(cwd)}
    if number % 250 == 0:
        return ({**base, "argv": ["head", "-c", "40000", "/dev/zero"],
                 "caps": ["effect:exec"]}, "overflow", False)
    if number % 100 == 0:
        return ({**base,
                 "argv": ["sh", "-c", 'printf x >> "$1"', "sh", str(counter)],
                 "caps": ["effect:exec"]}, "counter", True)
    if number % 200 == 50:
        target = cwd / f"touch-{index:06d}"
        return ({**base, "argv": ["touch", str(target)],
                 "caps": ["effect:mutate"]}, "touch", False)
    if number % 333 == 0:
        text = f"STDIN-{index:06d}\n"
        return ({**base, "argv": ["cat"], "stdin": text,
                 "caps": ["effect:read"]}, "stdin", False)
    if number % 400 == 75:
        return ({**base, "argv": ["sh", "-c", "true"],
                 "timeout_ms": 1000, "caps": ["effect:exec"]},
                "timeout", False)
    return ({**base, "argv": ["echo", f"OK-{index:06d}"],
             "caps": ["effect:read"]}, "echo", False)


def validate_response(result: dict, request: dict, kind: str, index: int) -> None:
    if result.get("id") != request["id"] or result.get("rc") != 0:
        raise RuntimeError(f"bad runtime response at {index}: {result}")
    if kind == "echo" and result.get("stdout") != f"OK-{index:06d}\n":
        raise RuntimeError(f"bad echo response at {index}")
    if kind == "stdin" and result.get("stdout") != f"STDIN-{index:06d}\n":
        raise RuntimeError(f"bad stdin response at {index}: {result}")
    if kind == "overflow" and (
        result.get("stdout_bytes") != 40000 or not result.get("stdout_ref")
    ):
        raise RuntimeError(f"bad overflow response at {index}")
    if kind == "touch" and result.get("files_changed", {}).get("total", 0) < 1:
        raise RuntimeError(f"touch mutation was not measured at {index}")
    if kind == "timeout" and result.get("timed_out") is not False:
        raise RuntimeError(f"fast timeout request was misclassified at {index}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime", type=Path)
    parser.add_argument("verifier", type=Path)
    parser.add_argument("--requests", type=int, default=1000)
    parser.add_argument("--expected-build", required=True)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--runtime-p95-ms", type=float, default=50.0)
    parser.add_argument("--verifier-p95-ms", type=float, default=30.0)
    parser.add_argument("--verified-p95-ms", type=float, default=80.0)
    parser.add_argument("--runtime-rss-growth-kb", type=int, default=4096)
    parser.add_argument("--verifier-rss-growth-kb", type=int, default=2048)
    args = parser.parse_args()
    if args.requests < 100:
        parser.error("--requests must be at least 100")
    required_platform = all(
        hasattr(fcntl, name)
        for name in ("F_ADD_SEALS", "F_SEAL_WRITE", "F_SEAL_GROW",
                     "F_SEAL_SHRINK", "F_SEAL_SEAL")
    ) and hasattr(os, "memfd_create")
    if (
        os.uname().sysname != "Linux"
        or not Path("/proc/self/fd").exists()
        or not required_platform
    ):
        parser.error("soak requires Linux procfs and memfd seals")
    runtime_path = args.runtime.resolve()
    verifier_path = args.verifier.resolve()
    for label, path in (("runtime", runtime_path), ("verifier", verifier_path)):
        if not path.is_file() or not os.access(path, os.X_OK):
            parser.error(f"{label} is not executable: {path}")

    runtime: subprocess.Popen[bytes] | None = None
    verifier: subprocess.Popen[bytes] | None = None
    with tempfile.TemporaryDirectory(
        prefix="jash-agent-soak-", dir=os.environ.get("TMPDIR", "/tmp")
    ) as raw:
        root = Path(raw)
        home = root / "home"
        cwd = root / "cwd"
        runtime_tmp = root / "tmp"
        for path in (home, cwd, runtime_tmp):
            path.mkdir()
        counter = cwd / "counter"
        env = {
            "HOME": str(home), "JASH_SIGIL": "0", "PATH": "/usr/bin:/bin",
            "LANG": "C", "LC_ALL": "C", "JASH_TMPDIR": str(runtime_tmp),
        }
        runtime_latencies: list[float] = []
        verifier_latencies: list[float] = []
        end_to_end_latencies: list[float] = []
        receipts: set[str] = set()
        overflow_refs: set[str] = set()
        manifest = hashlib.sha256()
        replay_count = 0
        mutation_count = 0
        verification_count = 0
        verifier_sequence = 0
        restart_request: bytes | None = None
        restart_response: bytes | None = None
        warmup = min(50, max(10, args.requests // 20))
        handshake_resources: tuple[int, int, int, int] | None = None
        warm_resources: tuple[int, int, int, int] | None = None
        peak_runtime_rss = 0
        peak_verifier_rss = 0

        def launch_runtime() -> tuple[subprocess.Popen[bytes], dict]:
            process = subprocess.Popen(
                [str(runtime_path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, bufsize=0, cwd=cwd, env=env,
            )
            if process.stdin is None or process.stdout is None:
                stop_process(process)
                raise RuntimeError("runtime pipes unavailable")
            handshake = json.loads(read_line(process.stdout, "runtime"))
            if (
                handshake.get("runtime") != "jash-agentd"
                or handshake.get("profile") != "agent-strict"
                or handshake.get("build", {}).get("git") != args.expected_build
            ):
                stop_process(process)
                raise RuntimeError(f"unexpected runtime handshake: {handshake}")
            return process, handshake

        def verify_artifacts(request_line: bytes, result_line: bytes,
                             result: dict) -> float:
            nonlocal verifier_sequence, verification_count
            if verifier is None or verifier.stdin is None or verifier.stdout is None:
                raise RuntimeError("verifier unavailable")
            receipt_ref = result["receipt_ref"]
            receipt_path = home / ".jash" / "agent_receipts" / receipt_ref
            receipt_fd = os.open(
                receipt_path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
            )
            try:
                receipt_bytes = read_fd(receipt_fd, 8192)
            finally:
                os.close(receipt_fd)
            fds = [
                sealed(request_line, f"request-{verifier_sequence}"),
                sealed(result_line, f"response-{verifier_sequence}"),
                sealed(receipt_bytes, f"receipt-{verifier_sequence}"),
            ]
            for tag in ("stdout", "stderr"):
                ref = result.get(f"{tag}_ref")
                if not ref:
                    fds.append(-1)
                    continue
                stream_path = home / ".jash" / "overflow" / ref
                stream_fd = os.open(
                    stream_path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
                )
                try:
                    stream_bytes = read_fd(stream_fd, result[f"{tag}_bytes"])
                finally:
                    os.close(stream_fd)
                fds.append(sealed(stream_bytes, f"{tag}-{verifier_sequence}"))
            started = time.perf_counter_ns()
            try:
                fields = [
                    "V", str(verifier_sequence),
                    *(str(fd) if fd >= 0 else "-" for fd in fds), receipt_ref,
                ]
                verifier.stdin.write(("\t".join(fields) + "\n").encode())
                verifier.stdin.flush()
                verdict = read_line(verifier.stdout, "verifier")
                expected = f"R\t{verifier_sequence}\tVALID\n".encode()
                if verdict != expected:
                    raise RuntimeError(
                        f"receipt verification failed: {verdict!r}"
                    )
            finally:
                for fd in fds:
                    if fd >= 0:
                        os.close(fd)
            verifier_sequence += 1
            verification_count += 1
            return (time.perf_counter_ns() - started) / 1e6

        try:
            runtime, handshake = launch_runtime()
            pubkey = handshake["receipts"]["pubkey"]
            verifier = subprocess.Popen(
                [str(verifier_path), "runtime-server", pubkey, str(os.getpid())],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, bufsize=0,
            )
            if verifier.stdin is None or verifier.stdout is None:
                raise RuntimeError("verifier pipes unavailable")
            ready = read_line(verifier.stdout, "verifier").decode().strip().split("\t")
            if ready[:3] != ["JEFV", "1", "READY"]:
                raise RuntimeError(f"unexpected verifier handshake: {ready}")
            handshake_resources = (
                rss_kb(runtime.pid), rss_kb(verifier.pid),
                fd_count(runtime.pid), fd_count(verifier.pid),
            )
            peak_runtime_rss = handshake_resources[0]
            peak_verifier_rss = handshake_resources[1]
            soak_started = time.perf_counter_ns()

            for index in range(args.requests):
                request, kind, replay = workload(index, cwd, counter)
                request_line = json.dumps(
                    request, separators=(",", ":")
                ).encode() + b"\n"
                total_started = time.perf_counter_ns()
                runtime_started = time.perf_counter_ns()
                assert runtime.stdin is not None and runtime.stdout is not None
                runtime.stdin.write(request_line)
                runtime.stdin.flush()
                result_line = read_line(runtime.stdout, "runtime")
                runtime_elapsed = (time.perf_counter_ns() - runtime_started) / 1e6
                result = json.loads(result_line)
                validate_response(result, request, kind, index)
                if result.get("receipt_pubkey") != pubkey:
                    raise RuntimeError(f"receipt key drift at {index}")
                receipt_ref = result.get("receipt_ref")
                if not isinstance(receipt_ref, str) or receipt_ref in receipts:
                    raise RuntimeError(f"bad or duplicate receipt at {index}")
                receipts.add(receipt_ref)
                if result.get("stdout_ref"):
                    overflow_refs.add(result["stdout_ref"])
                manifest.update(receipt_ref.encode() + b"\0")
                manifest.update(result["receipt_blake3"].encode() + b"\n")
                verifier_elapsed = verify_artifacts(request_line, result_line, result)
                total_elapsed = (time.perf_counter_ns() - total_started) / 1e6

                if index >= warmup:
                    runtime_latencies.append(runtime_elapsed)
                    verifier_latencies.append(verifier_elapsed)
                    end_to_end_latencies.append(total_elapsed)
                if index == warmup:
                    warm_resources = (
                        rss_kb(runtime.pid), rss_kb(verifier.pid),
                        fd_count(runtime.pid), fd_count(verifier.pid),
                    )

                if replay:
                    mutation_count += 1
                    runtime.stdin.write(request_line)
                    runtime.stdin.flush()
                    replay_line = read_line(runtime.stdout, "runtime")
                    if replay_line != result_line:
                        raise RuntimeError(f"non-identical replay at {index}")
                    replay_result = json.loads(replay_line)
                    verify_artifacts(request_line, replay_line, replay_result)
                    replay_count += 1
                    if counter.stat().st_size != mutation_count:
                        raise RuntimeError(f"effect repeated during replay at {index}")
                    restart_request = request_line
                    restart_response = result_line

                if (index + 1) % 100 == 0:
                    peak_runtime_rss = max(peak_runtime_rss, rss_kb(runtime.pid))
                    peak_verifier_rss = max(peak_verifier_rss, rss_kb(verifier.pid))
                    if children(runtime.pid):
                        raise RuntimeError("runtime retained a child process")

            soak_seconds = (time.perf_counter_ns() - soak_started) / 1e9
            if warm_resources is None or handshake_resources is None:
                raise RuntimeError("resource baseline was not captured")
            final_resources = (
                rss_kb(runtime.pid), rss_kb(verifier.pid),
                fd_count(runtime.pid), fd_count(verifier.pid),
            )
            if restart_request is None or restart_response is None:
                raise RuntimeError("no non-idempotent replay fixture was produced")

            runtime.stdin.close()
            if runtime.wait(timeout=10) != 0:
                raise RuntimeError("runtime failed during clean shutdown")
            runtime = None
            runtime, restarted_handshake = launch_runtime()
            if restarted_handshake["receipts"]["pubkey"] != pubkey:
                raise RuntimeError("runtime receipt identity changed after restart")
            assert runtime.stdin is not None and runtime.stdout is not None
            runtime.stdin.write(restart_request)
            runtime.stdin.flush()
            restarted_line = read_line(runtime.stdout, "restarted runtime")
            if restarted_line != restart_response:
                raise RuntimeError("restart replay was not byte-identical")
            verify_artifacts(restart_request, restarted_line, json.loads(restarted_line))
            if counter.stat().st_size != mutation_count:
                raise RuntimeError("restart replay repeated a committed effect")
            if children(runtime.pid):
                raise RuntimeError("restarted runtime retained a child process")
            final_resources = (
                final_resources[0], rss_kb(verifier.pid), final_resources[2],
                fd_count(verifier.pid),
            )
            peak_verifier_rss = max(peak_verifier_rss, final_resources[1])
            runtime.stdin.close()
            if runtime.wait(timeout=10) != 0:
                raise RuntimeError("restarted runtime failed during clean shutdown")
            runtime = None
            verifier.stdin.close()
            if verifier.wait(timeout=10) != 0:
                raise RuntimeError("verifier failed during clean shutdown")
            verifier = None

            request_records = list(
                (home / ".jash" / "agent_requests").glob("*.req")
            )
            receipt_records = list(
                (home / ".jash" / "agent_receipts").glob("*.dsse")
            )
            overflow_records = list((home / ".jash" / "overflow").glob("*"))
            leaked_temps = [
                str(path.relative_to(root)) for path in root.rglob("*")
                if ".tmp." in path.name
            ]
            runtime_tmp_leaks = list(runtime_tmp.iterdir())
            runtime_p95 = percentile(runtime_latencies, 0.95)
            verifier_p95 = percentile(verifier_latencies, 0.95)
            verified_p95 = percentile(end_to_end_latencies, 0.95)
            checks = {
                "runtime_p95": runtime_p95 <= args.runtime_p95_ms,
                "verifier_p95": verifier_p95 <= args.verifier_p95_ms,
                "verified_p95": verified_p95 <= args.verified_p95_ms,
                "runtime_peak_rss": peak_runtime_rss - warm_resources[0]
                <= args.runtime_rss_growth_kb,
                "verifier_peak_rss": peak_verifier_rss - warm_resources[1]
                <= args.verifier_rss_growth_kb,
                "runtime_total_rss": final_resources[0] - handshake_resources[0]
                <= args.runtime_rss_growth_kb * 2,
                "verifier_total_rss": final_resources[1] - handshake_resources[1]
                <= args.verifier_rss_growth_kb * 2,
                "runtime_fds": final_resources[2] == warm_resources[2],
                "verifier_fds": final_resources[3] == warm_resources[3],
                "request_records": len(request_records) == args.requests,
                "receipt_records": len(receipt_records) == args.requests,
                "overflow_records": len(overflow_records) == len(overflow_refs),
                "no_temp_leaks": not leaked_temps and not runtime_tmp_leaks,
                "replays_exactly_once": replay_count == mutation_count,
                "restart_replay": counter.stat().st_size == mutation_count,
                "all_unique_receipts_verified": len(receipts) == args.requests,
            }
            report = {
                "schema": "jash.agent-runtime-soak.v2",
                "build": args.expected_build,
                "runtime_sha256": file_sha256(runtime_path),
                "verifier_sha256": file_sha256(verifier_path),
                "receipt_pubkey": pubkey,
                "receipt_manifest_sha256": manifest.hexdigest(),
                "requests": args.requests,
                "receipts_verified": len(receipts),
                "verification_operations": verification_count,
                "replays": replay_count,
                "restart_replays": 1,
                "throughput_requests_s": round(args.requests / soak_seconds, 3),
                "runtime_ms": {
                    "p50": round(statistics.median(runtime_latencies), 3),
                    "p95": round(runtime_p95, 3),
                },
                "verifier_ms": {
                    "p50": round(statistics.median(verifier_latencies), 3),
                    "p95": round(verifier_p95, 3),
                },
                "end_to_end_ms": {
                    "p50": round(statistics.median(end_to_end_latencies), 3),
                    "p95": round(verified_p95, 3),
                },
                "rss_kb": {
                    "handshake": list(handshake_resources[:2]),
                    "warm": list(warm_resources[:2]),
                    "peak": [peak_runtime_rss, peak_verifier_rss],
                    "final": list(final_resources[:2]),
                },
                "fds": {
                    "warm": list(warm_resources[2:]),
                    "final": list(final_resources[2:]),
                },
                "thresholds": {
                    "runtime_p95_ms": args.runtime_p95_ms,
                    "verifier_p95_ms": args.verifier_p95_ms,
                    "end_to_end_p95_ms": args.verified_p95_ms,
                    "runtime_rss_growth_kb": args.runtime_rss_growth_kb,
                    "verifier_rss_growth_kb": args.verifier_rss_growth_kb,
                },
                "checks": checks,
            }
            rendered = json.dumps(report, sort_keys=True)
            print(rendered)
            if args.report:
                write_report(args.report, rendered)
            failed = [name for name, passed in checks.items() if not passed]
            if failed:
                raise RuntimeError(f"soak checks failed: {', '.join(failed)}")
        finally:
            stop_process(runtime)
            stop_process(verifier)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
