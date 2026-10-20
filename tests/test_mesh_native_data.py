#!/usr/bin/env python3
"""Mesh observations and receipts describe the actual bounded store state."""

import base64
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import native_data_fixture as fixture


FAULT_SOURCE = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int mode(const char* name) { const char* selected=getenv("MESH_TEST_IO"); return selected&&!strcmp(selected,name); }
static int suffix(int fd,const char* end) {
    char link[64],path[PATH_MAX+64]; snprintf(link,sizeof link,"/proc/self/fd/%d",fd);
    ssize_t n=readlink(link,path,sizeof path-1); if(n<0) return 0; path[n]=0;
    size_t length=strlen(end); return (size_t)n>=length&&!memcmp(path+n-length,end,length);
}
ssize_t write(int fd,const void* data,size_t length) {
    static ssize_t (*real_write)(int,const void*,size_t); if(!real_write) *(void**)&real_write=dlsym(RTLD_NEXT,"write");
    if(mode("write_worker")) {
        char link[64],path[PATH_MAX+64]; snprintf(link,sizeof link,"/proc/self/fd/%d",fd);
        ssize_t n=readlink(link,path,sizeof path-1);
        if(n>0) { path[n]=0; if(strstr(path,"/mesh/workers/.mesh.tmp.")) { errno=ENOSPC; return -1; } }
    }
    if(fd==STDOUT_FILENO&&mode("sink_after_claim")) {
        const char* target=getenv("MESH_TEST_TASK");
        if(target) {
            int source=open(target,O_RDONLY|O_CLOEXEC); char bytes[256]={0}; ssize_t n=source<0?-1:read(source,bytes,sizeof bytes-1);
            if(source>=0) close(source);
            if(n>0&&strstr(bytes,"\tclaimed\t")) { errno=ENOSPC; return -1; }
        }
    }
    return real_write(fd,data,length);
}
int renameat(int olddir,const char* oldname,int newdir,const char* newname) {
    static int (*real_renameat)(int,const char*,int,const char*); if(!real_renameat) *(void**)&real_renameat=dlsym(RTLD_NEXT,"renameat");
    if(mode("rename_worker")&&suffix(newdir,"/mesh/workers")) { errno=EIO; return -1; }
    if(mode("rename_task")&&suffix(newdir,"/mesh/tasks")) { errno=EIO; return -1; }
    return real_renameat(olddir,oldname,newdir,newname);
}
int fsync(int fd) {
    static int (*real_fsync)(int); if(!real_fsync) *(void**)&real_fsync=dlsym(RTLD_NEXT,"fsync");
    if(mode("fsync_worker_directory")&&suffix(fd,"/mesh/workers")) { errno=EIO; return -1; }
    if(mode("fsync_mesh_directory")&&suffix(fd,"/mesh")) { errno=EIO; return -1; }
    return real_fsync(fd);
}
ssize_t read(int fd,void* data,size_t length) {
    static ssize_t (*real_read)(int,void*,size_t); if(!real_read) *(void**)&real_read=dlsym(RTLD_NEXT,"read");
    if(mode("read_worker")&&suffix(fd,"/mesh/workers/worker.tsv")) { errno=EIO; return -1; }
    return real_read(fd,data,length);
}
int close(int fd) {
    static int (*real_close)(int); if(!real_close) *(void**)&real_close=dlsym(RTLD_NEXT,"close");
    int fail=mode("close_worker")&&suffix(fd,"/mesh/workers/worker.tsv");
    int result=real_close(fd); if(fail) { errno=EIO; return -1; } return result;
}
struct dirent* readdir(DIR* stream) {
    static struct dirent* (*real_readdir)(DIR*); if(!real_readdir) *(void**)&real_readdir=dlsym(RTLD_NEXT,"readdir");
    if(mode("readdir_workers")&&suffix(dirfd(stream),"/mesh/workers")) { errno=EIO; return NULL; }
    return real_readdir(stream);
}
'''


class MeshNative(fixture.NativeQueryCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix="jash-mesh-io-faults-")
        cls.addClassCleanup(temporary.cleanup)
        source = Path(temporary.name) / "faults.c"
        source.write_text(FAULT_SOURCE)
        cls.fault_library = Path(temporary.name) / "faults.so"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
                        str(source), "-o", str(cls.fault_library), "-ldl"], check=True, capture_output=True)

    def mesh(self, arguments, *, mode="--data-v2", rc=0, strict=False, caps=None):
        result = self.run_command(["mesh", mode, *arguments], strict=strict,
                                  caps=caps or ["effect:read", "effect:mutate"])
        self.assertEqual(result.returncode, rc, result.stderr)
        decoded = fixture.stream(result.stdout, credit_kind="none")
        self.assertEqual(decoded.producer, rc)
        return decoded

    def result(self, decoded):
        return next(row for row in reversed(decoded.items)
                    if row.value.get("row_kind") and row.value["row_kind"].value == "result")

    def receipts(self, decoded):
        return [row for row in decoded.items
                if row.value.get("row_kind") and row.value["row_kind"].value == "receipt"]

    def store(self):
        return Path(self.env["HOME"]) / ".jash" / "mesh"

    def test_worker_round_trip_and_argv_boundaries(self):
        model = b"model \tquoted\"\nraw-\xff"
        for strict in (False, True):
            worker = "strict" if strict else "general"
            made = self.mesh(["register", "--id", worker, "--provider", "provider space",
                              "--model", model], strict=strict)
            self.assertEqual(made.state, 0)
            self.assertTrue(any(row.value["applied"].value for row in self.receipts(made)))
            self.scalar(self.result(made), "agent_id", "string", worker)
            self.mesh(["heartbeat", "--id", worker, "--status", 'busy"\\',
                       "--headroom", '10"20', "--task", "task one"], strict=strict)
            observed = self.mesh(["workers"], strict=strict, caps=["effect:read"])
            row = next(row for row in observed.items if row.value["agent_id"].value == worker)
            self.scalar(row, "model", "bytes", model)
            self.scalar(row, "headroom", "string", '10"20')
            self.scalar(row, "alive", "nothing")
            self.scalar(row, "pid", "i64", 0)
        projected = self.run_command(["mesh", "workers", "--json"])
        self.assertEqual(projected.returncode, 0, projected.stderr)
        rows = json.loads(projected.stdout)
        self.assertEqual(rows[0]["provider"], "provider space")
        encoded = rows[0]["model"]
        self.assertEqual(base64.b64decode(encoded.get("data", encoded.get("b64"))), model)

    def test_task_dependencies_owner_proof_and_exact_descriptions(self):
        self.mesh(["register", "--id", "a", "--model", "test"])
        self.mesh(["init", "--", "goal \t newline\n"])
        text = b"description with spaces\t\nraw-\xff"
        first = self.mesh(["add", "--tag", "build", "--", text])
        self.scalar(self.result(first), "id", "u64", 1)
        self.mesh(["add", "dependent", "--needs", "1"])
        selected = self.mesh(["next", "--id", "a"])
        self.scalar(self.result(selected), "id", "u64", 1)
        self.scalar(self.result(selected), "desc", "bytes", text)
        empty = self.mesh(["next", "--id", "b"], rc=1)
        self.assertEqual((empty.state, empty.errors), (0, 0))
        self.scalar(self.result(empty), "available", "bool", False)
        refused = self.mesh(["done", "1", "--id", "b"], rc=1)
        self.assertEqual(refused.state, 2)
        self.assertFalse(any(row.value["applied"].value for row in self.receipts(refused)))
        self.mesh(["done", "1", "--id", "a", "--prove", "file missing"], rc=1,
                  caps=["effect:exec"])
        self.assertEqual(self.mesh(["board"]).items[0].value["state"].value, "claimed")
        (self.work / "artifact file").write_bytes(b"verified")
        completed = self.mesh(["done", "1", "--id", "a", "--prove", "file 'artifact file'"],
                              caps=["effect:exec"])
        for action, scope in (("execute_proof", "expect-callback"),
                              ("request_attestation", "sigil-attestation-callback")):
            callback = next(row for row in self.receipts(completed) if row.value["action"].value == action)
            self.scalar(callback, "scope", "symbol", scope)
            self.scalar(callback, "transaction", "symbol", "callback-invocation")
            self.scalar(callback, "callback_invoked", "bool", True)
            self.scalar(callback, "persistence_verified", "nothing")
            self.scalar(callback, "target_base", "nothing")
        self.scalar(self.result(self.mesh(["next", "--id", "a"])), "id", "u64", 2)

    def test_claims_raw_paths_and_read_queries_do_not_prune(self):
        raw = b"claim \t\nraw-\xff"
        absolute = os.fsencode(self.work) + b"/" + raw
        with open(absolute, "wb") as file:
            file.write(b"x")
        self.mesh(["claim", "--id", "owner", "--ttl", "900", "--note", "note\t\n", "--", raw])
        path = self.store() / "claims.tsv"
        before = (path.read_bytes(), path.stat().st_ino, path.stat().st_mtime_ns)
        claims = self.mesh(["claims"], strict=True, caps=["effect:read"])
        self.scalar(claims.items[0], "path", "path", absolute)
        self.scalar(claims.items[0], "note", "string", "note\t\n")
        self.assertEqual(before, (path.read_bytes(), path.stat().st_ino, path.stat().st_mtime_ns))
        held = self.mesh(["claim", raw, "--id", "other"], rc=1)
        self.assertEqual(held.state, 2)
        self.mesh(["unclaim", raw, "--id", "owner"])
        self.assertEqual(self.mesh(["claims"]).count, 0)

    def test_empty_queries_leave_store_absent(self):
        for sub in ("workers", "who", "board", "rep", "claims"):
            for strict in (False, True):
                with self.subTest(sub=sub, strict=strict):
                    result = self.mesh([sub], strict=strict, caps=["effect:read"])
                    self.assertEqual((result.count, result.state), (0, 0))
                    self.assertFalse(self.store().exists())

    def test_invalid_options_limits_and_literals_before_effect(self):
        invalid = [
            ["register", "--id", "../bad"], ["register", "--wat"],
            ["register", "--provider"], ["heartbeat", "--id", "a", "--id", "b"],
            ["add", "x" * 512], ["add", "task", "--needs", "1junk"],
            ["claim", "file", "--ttl", "-1"], ["claim", "file", "--ttl", "99999999999999999999999"],
            ["done", "1junk"], ["done", "0"], ["workers", "--limit", "0"],
            ["workers", "--json"], ["unknown"],
        ]
        for args in invalid:
            with self.subTest(args=args):
                failed = self.mesh(args, rc=2)
                self.assertEqual(failed.state, 2)
                self.assertFalse(self.store().exists())
        self.mesh(["init"])
        self.mesh(["add", "--", "--data"])
        self.scalar(self.mesh(["board"]).items[0], "desc", "string", "--data")

    def test_query_pages_corruption_and_nonregular_sources(self):
        self.mesh(["init"])
        for number in range(3):
            self.mesh(["add", f"task {number}"])
        page = self.mesh(["board", "--offset", "1", "--limit", "1"])
        self.assertEqual((page.state, page.count, page.reason), (3, 1, "page_window"))
        self.scalar(page.items[0], "id", "u64", 2)
        task = self.store() / "tasks" / "2.tsv"
        task.write_bytes(b"broken\n")
        corrupted = self.mesh(["board"], rc=1)
        self.assertEqual(corrupted.state, 2)
        self.assertNotEqual(corrupted.reason, "")
        task.unlink()
        os.mkfifo(task)
        failed = self.mesh(["board"], rc=1)
        self.assertEqual(failed.state, 2)

    def test_sink_admission_precedes_every_mutator(self):
        for sub in (["register", "--id", "a"], ["heartbeat", "--id", "a"],
                    ["init", "goal"], ["add", "task"], ["post", "message"],
                    ["claim", "file", "--id", "a"]):
            for mode in ("--json", "--data", "--data-v2"):
                with self.subTest(sub=sub, mode=mode):
                    request = {"request_version": 2, "argv": ["mesh", mode, *sub], "cwd": str(self.work)}
                    with open("/dev/full", "wb") as sink:
                        result = subprocess.run([str(fixture.JASH), "--exec-json", json.dumps(request)],
                                                cwd=self.work, env=self.env, stdout=sink,
                                                stderr=subprocess.PIPE, timeout=10)
                    self.assertEqual(result.returncode, 125, result.stderr)
                    self.assertFalse(self.store().exists())

    def test_published_and_unpublished_io_failures_have_distinct_receipts(self):
        self.mesh(["register", "--id", "worker", "--model", "before"])
        target = self.store() / "workers" / "worker.tsv"
        original = target.read_bytes()
        self.env["LD_PRELOAD"] = str(self.fault_library)
        for fault in ("write_worker", "rename_worker"):
            with self.subTest(fault=fault):
                self.env["MESH_TEST_IO"] = fault
                failed = self.mesh(["register", "--id", "worker", "--model", "after"], rc=1)
                receipt = next(row for row in self.receipts(failed)
                               if row.value["action"].value == "register_worker")
                self.scalar(receipt, "applied", "bool", False)
                self.scalar(receipt, "durable", "bool", False)
                self.assertEqual(target.read_bytes(), original)
                self.assertFalse(list(target.parent.glob(".mesh.tmp.*")))
        self.env["MESH_TEST_IO"] = "fsync_worker_directory"
        partial = self.mesh(["register", "--id", "worker", "--model", "published"], rc=1)
        receipt = next(row for row in self.receipts(partial)
                       if row.value["action"].value == "register_worker")
        self.scalar(receipt, "applied", "bool", True)
        self.scalar(receipt, "verified", "bool", True)
        self.scalar(receipt, "durable", "bool", False)
        self.scalar(receipt, "target_base", "path", os.fsencode(self.store()))
        self.assertIn(b"\tpublished\t", target.read_bytes())
        self.assertEqual(partial.state, 2)

    def test_partial_publication_summary_tracks_applied_state(self):
        self.mesh(["register", "--id", "worker"])
        self.mesh(["add", "task"])
        self.env.update(LD_PRELOAD=str(self.fault_library), MESH_TEST_IO="fsync_worker_directory")
        selected = self.mesh(["next", "--id", "worker"], rc=1)
        self.scalar(self.result(selected), "claimed", "bool", True)
        self.scalar(self.result(selected), "registry_updated", "bool", True)
        self.assertIn(b"\tworking\t", (self.store() / "workers/worker.tsv").read_bytes())
        self.env["MESH_TEST_IO"] = "rename_task"
        refused = self.mesh(["done", "1", "--id", "worker"], rc=1)
        self.scalar(self.result(refused), "state", "string", "claimed")
        self.scalar(self.result(refused), "requested_state", "symbol", "done")
        self.scalar(self.result(refused), "applied", "bool", False)
        self.env["MESH_TEST_IO"] = "fsync_mesh_directory"
        partial = self.mesh(["claim", "path", "--id", "worker"], rc=1)
        self.scalar(self.result(partial), "changed", "bool", True)
        self.assertIn(b"/path\tworker\t", (self.store() / "claims.tsv").read_bytes())
        self.env.pop("MESH_TEST_IO")
        self.env["JASH_AGENT_PID"] = "2147483647"
        self.mesh(["register", "--id", "dead"])
        self.env["MESH_TEST_IO"] = "fsync_worker_directory"
        swept = self.mesh(["sweep"], rc=1)
        self.scalar(self.result(swept), "dead", "u64", 1)
        self.assertIn(b"\tdead\t", (self.store() / "workers/dead.tsv").read_bytes())

    def test_reputation_counts_only_the_actual_command_field(self):
        self.mesh(["init"])
        chain = self.store().parent / "chain.log"
        prefix = "1 1.000000000 " + "0" * 64 + " tag=" + "0" * 64 + " rc=0 cmd="
        chain.write_text(prefix + "echo cmd=mesh-done: task=1 tag=build model=forged\n" +
                         prefix + "mesh-done: task=1 tag=build model=real mesh_format=2\n" +
                         prefix + "mesh-fail: task=2 tag=build model=real mesh_format=2\n")
        rows = self.mesh(["rep"])
        self.assertEqual(rows.count, 1)
        self.scalar(rows.items[0], "model", "string", "real")
        self.scalar(rows.items[0], "done", "u64", 1)
        self.scalar(rows.items[0], "fail", "u64", 1)
        self.scalar(rows.items[0], "verification", "symbol", "not-verified")

    def test_path_and_artifact_preflight_leave_store_unchanged(self):
        rejected = self.mesh(["claim", "x" * 4096, "--id", "worker"], rc=2)
        self.assertEqual(rejected.state, 2)
        self.assertFalse(self.store().exists())
        self.mesh(["add", "x" * 500])
        # A legacy task-only store must not acquire worker/lock files when an
        # argument-derived description limit rejects the request.
        (self.store() / ".lock").unlink()
        (self.store() / "workers").rmdir()
        before = {str(path.relative_to(self.store())): path.read_bytes()
                  for path in self.store().rglob("*") if path.is_file()}
        self.mesh(["done", "1", "--artifact", "too-long", "--id", "worker"], rc=2)
        after = {str(path.relative_to(self.store())): path.read_bytes()
                 for path in self.store().rglob("*") if path.is_file()}
        self.assertEqual(before, after)
        self.assertFalse((self.store() / "workers").exists())
        self.assertFalse((self.store() / ".lock").exists())

    def test_read_and_directory_faults_never_become_empty_success(self):
        self.mesh(["register", "--id", "worker"])
        self.env["LD_PRELOAD"] = str(self.fault_library)
        for fault in ("read_worker", "close_worker", "readdir_workers"):
            for mode in ("--json", "--data-v2"):
                with self.subTest(fault=fault, mode=mode):
                    self.env["MESH_TEST_IO"] = fault
                    result = self.run_command(["mesh", mode, "workers"])
                    self.assertEqual(result.returncode, 1, result.stderr)
                    if mode == "--json":
                        self.assertNotIsInstance(json.loads(result.stdout), list)
                    else:
                        parsed = fixture.stream(result.stdout, credit_kind="none")
                        self.assertEqual(parsed.state, 2)
                        self.assertGreater(parsed.errors, 0)

    def test_late_delivery_failure_stops_before_worker_assignment(self):
        self.mesh(["register", "--id", "worker"])
        self.mesh(["add", "task"])
        worker = self.store() / "workers" / "worker.tsv"
        before = worker.read_bytes()
        self.env.update(LD_PRELOAD=str(self.fault_library), MESH_TEST_IO="sink_after_claim",
                        MESH_TEST_TASK=str(self.store() / "tasks" / "1.tsv"))
        result = self.run_command(["mesh", "--data-v2", "next", "--id", "worker"])
        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertIn(b"\tclaimed\tworker\t", (self.store() / "tasks" / "1.tsv").read_bytes())
        self.assertEqual(worker.read_bytes(), before)
        self.assertTrue(result.stdout.startswith(b"JTV2"))
        with self.assertRaises(AssertionError):
            fixture.stream(result.stdout, credit_kind="none")

    def test_query_budget_and_expired_claims_are_explicit_without_disk_changes(self):
        self.mesh(["register", "--id", "worker"])
        self.env["JASH_LLM_BUDGET"] = "1"
        result = self.run_command(["mesh", "--json", "workers"])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)["error"]["code"], "output_budget_exceeded")
        self.env.pop("JASH_LLM_BUDGET")
        path = self.store() / "claims.tsv"
        path.write_text(f"{self.work}/old\tworker\t1\t1\tnote\n")
        before = (path.read_bytes(), path.stat().st_ino, path.stat().st_mtime_ns)
        self.assertEqual(self.mesh(["claims"]).count, 0)
        self.assertEqual(before, (path.read_bytes(), path.stat().st_ino, path.stat().st_mtime_ns))

    def test_messages_versioned_bytes_and_literal_backslashes(self):
        self.mesh(["init", "goal"])
        message = b"literal\\t actual\t newline\n raw-\xff"
        self.mesh(["post", "--id", "sender", "--", message])
        rows = self.mesh(["board", "--messages"])
        self.assertEqual(rows.count, 2)
        self.scalar(rows.items[0], "message", "string", "goal")
        self.scalar(rows.items[1], "message", "bytes", message)
        self.scalar(rows.items[1], "owner", "string", "sender")
        self.assertEqual(self.mesh(["board"]).count, 0)


if __name__ == "__main__":
    unittest.main()
