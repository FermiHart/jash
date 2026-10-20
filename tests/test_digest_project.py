#!/usr/bin/env python3
"""Digest waves 1/3 regressions; run with DIGEST_BINARY=/absolute/build/jash.

All fixtures, HOME and TMPDIR are isolated in one private temporary directory,
respecting the caller's TMPDIR. No shell from PATH is used as the binary under
test. No repository fixtures are modified.

Direct API harness (Linux; built by the default testing build):
  cmake --build build --target jash test_digest_engine_unit
  DIGEST_BINARY="$PWD/build/jash" DIGEST_UNIT_BINARY="$PWD/build/test_digest_engine_unit" \
    python3 tests/test_digest_project.py
The harness links the real native adapters and codecs too. It wraps reads,
fsync and metadata observations for deterministic faults; no codec stubs.
"""
import concurrent.futures
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


class ProjectDigest(unittest.TestCase):
    def setUp(self):
        self.binary = os.environ["DIGEST_BINARY"]
        self.temp = tempfile.TemporaryDirectory(prefix="digest-test-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.home = self.base / "home"
        self.tmp = self.base / "tmp"
        self.root = self.base / "Orchard"
        for p in (self.home, self.tmp, self.root):
            p.mkdir(mode=0o700)
        self.env = {"HOME": str(self.home), "TMPDIR": str(self.tmp),
                    "PATH": "/usr/bin:/bin", "LC_ALL": "C", "JASH_SIGIL": "0",
                    "JASH_AGENT": "0", "JASH_BRIEF": "0"}

    def put(self, name, content="x\n", root=None):
        p = (root or self.root) / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
        return p

    def run_digest(self, *args, root=None, ok=True, command=None):
        command = command or shlex.join(["digest", *map(str, args), str(root or self.root)])
        p = subprocess.run([self.binary, "-c", command], env=self.env, cwd=self.base,
                           capture_output=True, text=True, timeout=15)
        if ok:
            self.assertEqual(p.returncode, 0, p.stderr + p.stdout)
        else:
            self.assertNotEqual(p.returncode, 0, p.stdout)
            self.assertIn("digest:", p.stderr)
            self.assertNotIn("# Project Digest", p.stdout)
        return p.stdout

    def fingerprint(self, **kw):
        text = self.run_digest("--fingerprint", **kw)
        self.assertRegex(text, r"\A[0-9a-f]{64}\n\Z")
        return text.strip()

    def counts(self, output, expected):
        labels = ("source files", "headers", "tests", "docs", "build files", "configuration files")
        for label, count in zip(labels, expected):
            self.assertRegex(output, rf"\*\*{label}:\*\* {count}(?:\D|$)")

    def test_generic_python_c_js_and_mixed_counts(self):
        fixtures = [
            (["app.py", "tests/test_app.py", "README.md", "pyproject.toml"], (1, 0, 1, 1, 1, 0)),
            (["src/main.c", "inc/main.h", "tests/check.c", "README.md", "CMakeLists.txt"],
             (1, 1, 1, 1, 1, 0)),
            (["app.js", "view.tsx", "types.d.ts", "app.test.ts", "README.md", "package.json", "tsconfig.json"],
             (2, 1, 1, 1, 2, 0)),
            (["app.py", "native.c", "native.h", "web.js", "web.ts", "tests/check.sh", "tools/build.sh",
              "Docs/guide.rst", "Makefile", "settings.toml"], (5, 1, 1, 1, 1, 1)),
        ]
        for i, (files, expected) in enumerate(fixtures):
            with self.subTest(fixture=i):
                root = self.base / f"Garden{i}"
                root.mkdir()
                for name in files:
                    self.put(name, "# Orchard documentation\n" if name == "README.md" else "x\n", root)
                out = self.run_digest("--brief", root=root)
                self.counts(out, expected)
                self.assertIn(f"**project:** Garden{i}", out)
                for invented in ("BearJosh", "c/parser/eval.c", "Risk Map", "Recent Work", "--target jash"):
                    self.assertNotIn(invented, out)

    def test_identity_readme_and_no_readme(self):
        out = self.run_digest("--brief")
        self.assertIn("**project:** Orchard", out)
        self.assertIn(str(self.root), out)
        self.assertNotIn("POSIX-ish", out)
        self.put("README.md", "# Orchard\nGrows fruit offline.\n")
        self.assertIn("Grows fruit offline.", self.run_digest("--brief"))

    def test_spaced_paths_output_and_option_order(self):
        root = self.base / "fruit orchard"
        root.mkdir()
        self.put("fruit.py", root=root)
        output = self.base / "project brief.md"
        command = shlex.join(["digest", str(root), "--output", str(output), "--brief"])
        self.assertEqual(self.run_digest(command=command), "")
        self.assertIn("**project:** fruit orchard", output.read_text())
        self.assertIn("**source files:** 1", output.read_text())

    def test_strict_flags_and_session_dispatch(self):
        for args in (("--helpful",), ("--wat",), ("-x",), ("--last", "garbage"),
                     ("--last", "0"), ("--no-git",), ("--session", "--no-cache"),
                     ("--brief", "--deep")):
            with self.subTest(args=args):
                self.run_digest(*args, ok=False)
        self.run_digest(command="digest --output", ok=False)
        out = self.run_digest(command="digest --session --brief --last 8 --no-git")
        self.assertIn("# Session Digest", out)
        self.assertIn("**level:** brief", out)
        self.assertNotIn("Git Status", out)
        for value in ("", "-1", "+1", "1x", "10001", "9999999999999999999999999"):
            self.run_digest(command=shlex.join(["digest", "--session", "--last", value]), ok=False)
        for flag in ("--refresh", "--no-cache", "--fingerprint"):
            self.run_digest(command="digest --session " + flag, ok=False)
        self.run_digest(command="digest --last 1", ok=False)

    def test_quoted_paths_across_dispatch_contexts(self):
        root = self.base / "fruit 'orchard'"
        root.mkdir()
        self.put("app.py", root=root)
        qroot = shlex.quote(str(root))
        expected = self.run_digest("--brief", "--no-cache", root=root)
        for command in (f"builtin digest --no-cache {qroot} --brief",
                        f"command digest --no-cache --brief {qroot}",
                        f"digest --no-cache --brief {qroot} | /bin/cat",
                        f"p={qroot}; digest --brief --no-cache \"$p\""):
            self.assertEqual(expected, self.run_digest(command=command))
        output = self.base / "redirected brief.md"
        self.run_digest(command=f"digest --brief --no-cache {qroot} > {shlex.quote(str(output))}")
        self.assertEqual(expected, output.read_text())

    def test_output_flags_and_special_targets(self):
        output = self.base / "explicit brief.md"
        command = shlex.join(["digest", "--fingerprint", "--output", str(output), str(self.root)])
        self.assertEqual(self.run_digest(command=command), "")
        self.assertEqual(output.read_text().strip(), self.fingerprint())
        self.assertFalse((self.home / ".jash/digest").exists())
        self.run_digest(command="digest --output a --output b", ok=False)
        self.run_digest(command="digest --output ''", ok=False)
        output.unlink()
        os.mkfifo(output)
        self.run_digest("--no-cache", "--output", output, ok=False)

    def test_direct_engine_writer_errors_short_writes_and_readonly(self):
        binary = os.environ.get("DIGEST_UNIT_BINARY")
        if not binary:
            self.skipTest("set DIGEST_UNIT_BINARY to the direct engine harness")
        target = self.put("app.py")
        absent_home = self.base / "absent home"
        cached_failure_modes = {"read-error", "mutate"}
        original = {str(p): p.read_bytes() for p in self.base.rglob("*") if p.is_file()}
        for mode in ("negative", "zero", "short", "readonly", "builtin-readonly", "read-error", "mutate"):
            with self.subTest(mode=mode):
                home = self.home if mode in cached_failure_modes else absent_home
                env = dict(self.env, DIGEST_UNIT_TARGET=str(target),
                           HOME=str(home), TMPDIR=str(self.base / "absent tmp"))
                p = subprocess.run([binary, mode, str(self.root)], env=env,
                                   capture_output=True, text=True, timeout=15)
                self.assertEqual(p.returncode, 0, p.stderr)
                if mode in ("negative", "zero", "read-error", "mutate"):
                    self.assertIn("digest:", p.stderr)
                self.assertEqual(p.stdout, "")
                if mode in cached_failure_modes:
                    cache_dir = self.home / ".jash/digest"
                    self.assertFalse(list(cache_dir.glob("*.md")))
                    self.assertFalse([p for p in (cache_dir / "cas").glob("*/*")
                                      if p.is_file()])
                else:
                    self.assertFalse(Path(env["HOME"]).exists())
                self.assertFalse(Path(env["TMPDIR"]).exists())
                if mode != "mutate":
                    self.assertEqual(original, {str(p): p.read_bytes() for p in self.base.rglob("*") if p.is_file()})

    def test_atomic_publication_failure_preserves_old_cache(self):
        binary = os.environ.get("DIGEST_UNIT_BINARY")
        if not binary:
            self.skipTest("set DIGEST_UNIT_BINARY to the direct engine harness")
        self.put("app.py")
        expected = self.run_digest("--brief")
        cache_dir = self.home / ".jash/digest"
        original = {p.name: p.read_bytes() for p in cache_dir.iterdir() if p.is_file()}
        p = subprocess.run([binary, "publish-error", str(self.root)], env=self.env,
                           capture_output=True, text=True, timeout=15)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("atomic cache publication failed", p.stderr)
        self.assertEqual(original, {p.name: p.read_bytes() for p in cache_dir.iterdir() if p.is_file()})
        self.assertEqual(expected, self.run_digest("--brief"))

    def test_double_dash_and_multiple_roots(self):
        root = self.base / "--brief"
        root.mkdir()
        self.assertIn("**project:** --brief", self.run_digest(command="digest -- --brief"))
        self.run_digest(str(self.root), ok=False)

    def test_crossroot_cache_identity_even_with_identical_bytes(self):
        other = self.base / "Other"
        other.mkdir()
        self.put("README.md", "# Identical\n", self.root)
        self.put("README.md", "# Identical\n", other)
        a = self.run_digest("--brief")
        b = self.run_digest("--brief", root=other)
        self.assertIn("**project:** Orchard", a)
        self.assertIn("**project:** Other", b)
        self.assertNotEqual(self.fingerprint(), self.fingerprint(root=other))

    def test_invalidation_content_rename_delete_and_configuration(self):
        p = self.put("app.py", "one\n")
        a = self.fingerprint()
        before = p.stat()
        p.write_text("two\n")
        os.utime(p, ns=(before.st_atime_ns, before.st_mtime_ns))
        b = self.fingerprint()
        self.assertNotEqual(a, b)
        p.rename(self.root / "renamed.py")
        c = self.fingerprint()
        self.assertNotEqual(b, c)
        (self.root / "renamed.py").unlink()
        d = self.fingerprint()
        self.assertNotEqual(c, d)
        self.put("settings.toml", "opaque")
        self.assertNotEqual(d, self.fingerprint())

    def test_opaque_artifacts_are_outside_inventory(self):
        self.put("app.py")
        before = self.fingerprint()
        with (self.root / "model.bin").open("wb") as f:
            f.truncate(64 * 1024 * 1024)
        self.put("jash-dev", "executable artifact")
        self.assertEqual(before, self.fingerprint())
        self.counts(self.run_digest("--brief"), (1, 0, 0, 0, 0, 0))
        # Vendor SOURCE is included; do not exclude an entire vendor tree.
        self.put("vendor/adapter.py")
        self.assertNotEqual(before, self.fingerprint())
        self.assertIn("**source files:** 2", self.run_digest("--brief"))

    def test_readme_invalid_utf8_and_control_bytes_are_escaped(self):
        (self.root / "README.md").write_bytes("# Café 果樹\n".encode() + b"bad:\xff\xc0\xaf\x1b\x00\n")
        out = self.run_digest("--brief")
        self.assertIn("Café 果樹", out)
        self.assertIn(r"bad:\xff\xc0\xaf\x1b\x00", out)
        self.assertNotIn("\x1b", out)

    def test_ignored_dirs_special_files_and_empty_dirs(self):
        self.put("app.py")
        before = self.fingerprint()
        for name in (".git", "build", "node_modules", ".venv", "__pycache__", ".jash"):
            self.put(name + "/large.py", "ignored")
        (self.root / "loop").symlink_to(self.root, target_is_directory=True)
        (self.root / "outside.py").symlink_to(self.put("outside.py", root=self.base))
        os.mkfifo(self.root / "pipe.py")
        self.assertEqual(before, self.fingerprint())
        self.counts(self.run_digest("--brief"), (1, 0, 0, 0, 0, 0))
        (self.root / "empty").mkdir()
        self.assertNotEqual(before, self.fingerprint())
        # Unsupported files are excluded regardless of their name.
        (self.root / "build/large.py").unlink()
        (self.root / "build").rmdir()
        self.put("build")
        self.assertIn("**configuration files:** 0", self.run_digest("--brief"))

    def test_cmake_generated_tree_is_not_project_source(self):
        self.put("app.py")
        before = self.fingerprint()
        generated = self.root / "build-custom"
        self.put("build-custom/CMakeCache.txt", "# Generated by CMake\n")
        self.put("build-custom/CMakeFiles/compiler_depend.py", "generated\n")
        self.put("build-custom/Testing/report.md", "test output\n")
        self.assertEqual(before, self.fingerprint())
        self.counts(self.run_digest("--brief", "--no-cache"), (1, 0, 0, 0, 0, 0))
        self.assertTrue(generated.is_dir())
        self.put("build-tools/generator.py", "genuine source\n")
        self.assertNotEqual(before, self.fingerprint())

    def test_total_byte_bound_is_global_and_no_partial_cache_is_published(self):
        for n in range(17):
            with (self.root / f"part{n}.py").open("wb") as f:
                f.truncate(8 * 1024 * 1024)
        self.run_digest("--brief", ok=False)
        self.assertFalse(list((self.home / ".jash/digest").rglob("*.md")))

    def test_corrupt_cache_fails_and_refresh_repairs_it(self):
        self.put("app.py")
        expected = self.run_digest("--brief")
        cache, = (self.home / ".jash/digest").glob("*.md")
        for corrupted in (b"# plausible but fake brief\n", cache.read_bytes()[:-7]):
            cache.write_bytes(corrupted)
            self.run_digest("--brief", ok=False)
            self.assertEqual(expected, self.run_digest("--brief", "--no-cache"))
            self.assertEqual(expected, self.run_digest("--brief", "--refresh"))

    def test_levels_refresh_and_canonical_alias_are_deterministic(self):
        for name in ("z.py", "a.ts", "README.md", "tests/test_b.py"):
            self.put(name)
        alias = self.base / "alias"
        alias.symlink_to(self.root, target_is_directory=True)
        for level in ("--brief", "--standard", "--deep"):
            first = self.run_digest(level)
            self.assertEqual(first, self.run_digest(level))
            self.assertEqual(first, self.run_digest(level, "--refresh"))
            self.assertEqual(first, self.run_digest(level, root=alias))
        deep = self.run_digest("--deep")
        self.assertLess(deep.index("`a.ts`"), deep.index("`z.py`"))
        self.assertEqual(self.fingerprint(), self.fingerprint(root=alias))
        self.assertEqual(self.fingerprint(), self.run_digest("--deep", "--fingerprint").strip())

    def test_file_order_and_line_counts_are_content_based(self):
        files = {"z.py": "z" * 5000, "b.py": "b\nc\n", "a.py": "", "README.md": "# Orchard\n"}
        for name, content in files.items():
            self.put(name, content)
        first = self.run_digest("--deep", "--no-cache")
        self.assertIn("**total lines:** 4", first)
        self.assertIn("**total bytes:** 5014", first)
        for name in files:
            (self.root / name).unlink()
        for name, content in reversed(list(files.items())):
            self.put(name, content)
        self.assertEqual(first, self.run_digest("--deep", "--no-cache"))

    def test_no_cache_and_fingerprint_create_no_cache_state(self):
        self.put("app.py")
        out = self.run_digest("--brief", "--no-cache")
        self.assertFalse((self.home / ".jash/digest").exists())
        fingerprint = self.fingerprint()
        self.assertIn(fingerprint, out)
        self.assertFalse((self.home / ".jash/digest").exists())
        cached = self.run_digest("--brief")
        self.assertEqual(cached, out)
        cache = self.home / ".jash/digest"
        snapshot = {str(p): (p.read_bytes(), p.stat().st_mtime_ns) for p in cache.rglob("*") if p.is_file()}
        self.assertEqual(out, self.run_digest("--brief", "--no-cache", "--refresh"))
        self.assertEqual(snapshot, {str(p): (p.read_bytes(), p.stat().st_mtime_ns)
                                   for p in cache.rglob("*") if p.is_file()})

    def test_depth_and_file_byte_bounds_fail_visibly(self):
        p = self.root
        for _ in range(33):
            p /= "d"
            p.mkdir()
        self.run_digest("--brief", ok=False)
        other = self.base / "large"
        other.mkdir()
        with (other / "large.py").open("wb") as f:
            f.truncate(8 * 1024 * 1024 + 1)
        self.run_digest("--brief", root=other, ok=False)
        self.assertFalse(list((self.home / ".jash/digest").rglob("*.md")))

    def test_total_entry_bound_is_global(self):
        for group in range(33):
            folder = self.root / str(group)
            folder.mkdir()
            for n in range(1000):
                (folder / str(n)).touch()
        self.run_digest("--brief", ok=False)

    def test_cache_symlinks_and_fifo_are_rejected(self):
        self.put("app.py")
        self.run_digest("--brief")
        files = list((self.home / ".jash/digest").rglob("*.md"))
        self.assertEqual(len(files), 1)
        victim = self.put("victim", "untouched", self.base)
        files[0].unlink()
        files[0].symlink_to(victim)
        self.run_digest("--brief", ok=False)
        self.assertEqual(victim.read_text(), "untouched")
        self.run_digest("--brief", "--refresh", ok=False)
        files[0].unlink()
        os.mkfifo(files[0])
        self.run_digest("--brief", ok=False)
        self.assertIn("**project:** Orchard", self.run_digest("--brief", "--no-cache"))

    def test_cache_hardlink_is_rejected(self):
        self.put("app.py")
        self.run_digest("--brief")
        cache, = (self.home / ".jash/digest").glob("*.md")
        outside = self.base / "cache-hardlink"
        os.link(cache, outside)
        self.assertEqual(cache.stat().st_nlink, 2)
        self.run_digest("--brief", ok=False)
        self.assertTrue(outside.is_file())
        self.assertEqual(cache.stat().st_nlink, 2)

    def test_cache_target_transient_unlinked_inode_is_retried(self):
        binary = os.environ.get("DIGEST_UNIT_BINARY")
        if not binary:
            self.skipTest("set DIGEST_UNIT_BINARY to the direct engine harness")
        target = self.put("app.py")
        env = dict(self.env, DIGEST_UNIT_TARGET=str(target), HOME=str(self.home))
        p = subprocess.run([binary, "cache-link-zero", str(self.root)], env=env,
                           capture_output=True, text=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, "")
        self.assertIn("**source files:** 1", self.run_digest("--brief"))

    def test_cache_parent_symlink_is_rejected(self):
        (self.home / ".jash").mkdir(exist_ok=True)
        outside = self.base / "outside-cache"
        outside.mkdir()
        (self.home / ".jash/digest").symlink_to(outside, target_is_directory=True)
        self.run_digest("--brief", ok=False)
        self.assertEqual(list(outside.iterdir()), [])
        self.run_digest("--brief", "--no-cache")

    def test_concurrent_cache_publication(self):
        self.put("README.md", "# Orchard\n" + "Fruit.\n" * 1000)
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            outputs = list(pool.map(lambda _: self.run_digest("--standard", "--refresh"), range(12)))
        self.assertEqual(len(set(outputs)), 1)
        self.assertEqual(outputs[0], self.run_digest("--standard"))
        self.assertFalse(list((self.home / ".jash/digest").rglob("*.tmp")))

    def test_jobs_budget_is_validated(self):
        self.put("app.py")
        for value in ("", "-1", "0", "1x", "33", "9999999999999999999999999"):
            self.run_digest("--jobs", value, ok=False)
        self.run_digest(command="digest --jobs", ok=False)
        self.run_digest(command="digest --jobs 2 --jobs 3", ok=False)
        self.run_digest("--session", "--jobs", "4", ok=False)
        # Every valid budget produces the same fingerprint as the default.
        base = self.fingerprint()
        for value in ("1", "2", "8", "32"):
            self.assertEqual(self.fingerprint(), base)

    def test_output_is_byte_identical_across_worker_budgets(self):
        # Several README candidates race for the capture buffer under parallel
        # reads; a multi-chunk file exercises per-entry accounting; shebang,
        # UTF-8 and empty directories exercise the walk-thread-only paths.
        self.put("README.md", "# Root readme\n" + "line\n" * 200)
        self.put("README.rst", "rst fallback\n")
        self.put("readme.md", "case variant\n")
        self.put("app.py", "print('hi')\n" * 300)
        self.put("big.c", "int x;\n" * 25000)
        self.put("docs/guide.md", "guia com acentos e café\n")
        self.put("run", "#!/bin/sh\necho orchard\n")
        (self.root / "empty").mkdir()
        for i in range(24):
            self.put(f"a/mod{i:02}.c", f"int v{i};\n" * (i + 1))
            self.put(f"b/lib{i:02}.py", "x = 1\n" * (i % 7 + 1))
        self.put("tests/check.c", "#include <stdio.h>\n")
        for mode in (("--fingerprint",), ("--manifest",), ("--brief", "--no-cache"),
                     ("--standard", "--no-cache"), ("--deep", "--no-cache")):
            with self.subTest(mode=mode):
                outputs = [self.run_digest(*mode, "--jobs", str(jobs)) for jobs in (1, 2, 8)]
                outputs.append(self.run_digest(*mode))
                self.assertEqual(len(set(outputs)), 1)

    def test_total_byte_bound_holds_under_worker_pool(self):
        for n in range(17):
            with (self.root / f"part{n}.py").open("wb") as f:
                f.truncate(8 * 1024 * 1024)
        self.run_digest("--brief", "--jobs", "4", ok=False)
        self.assertFalse(list((self.home / ".jash/digest").rglob("*.md")))

    def save_manifest(self, root=None):
        import json
        output = self.base / "base.json"
        command = __import__("shlex").join(
            ["digest", "--manifest", "--output", str(output), str(root or self.root)])
        self.assertEqual(self.run_digest(command=command), "")
        manifest = json.loads(output.read_text())
        self.assertEqual(manifest["schema"], "jash.project-observation.v1")
        return output, manifest

    def run_diff(self, base, *args, root=None, ok=True):
        import json
        text = self.run_digest("--diff-base", str(base), *args, root=root, ok=ok)
        return json.loads(text) if ok else text

    def test_diff_identical_tree_is_empty(self):
        self.put("app.py", "one\n")
        self.put("docs/guide.md", "guide\n")
        base, manifest = self.save_manifest()
        diff = self.run_diff(base)
        self.assertEqual(diff["schema"], "jash.project-diff.v1")
        self.assertEqual(diff["generator"], "project-v5")
        self.assertEqual(diff["base_fingerprint"], manifest["fingerprint"])
        self.assertEqual(diff["fingerprint"], manifest["fingerprint"])
        self.assertEqual(diff["fingerprint"], self.fingerprint())
        self.assertEqual((diff["added"], diff["modified"], diff["removed"]), ([], [], []))

    def test_diff_reports_added_modified_removed(self):
        self.put("keep.py", "keep\n")
        self.put("change.py", "before\n")
        self.put("gone.py", "gone\n")
        base, manifest = self.save_manifest()
        old_hash = next(e["blake3"] for e in manifest["entries"] if e["path"] == "change.py")
        self.put("change.py", "after\n")
        self.put("new.py", "new\n")
        (self.root / "gone.py").unlink()
        diff = self.run_diff(base)
        self.assertEqual([e["path"] for e in diff["added"]], ["new.py"])
        self.assertEqual(len(diff["modified"]), 1)
        self.assertEqual(diff["modified"][0]["path"], "change.py")
        self.assertEqual(diff["modified"][0]["old_blake3"], old_hash)
        self.assertNotEqual(diff["modified"][0]["blake3"], old_hash)
        self.assertEqual([e["path"] for e in diff["removed"]], ["gone.py"])
        self.assertNotEqual(diff["fingerprint"], diff["base_fingerprint"])
        # A same-path kind change surfaces as a removed+added pair.
        (self.root / "keep.py").unlink()
        (self.root / "keep.py").mkdir()
        self.put("keep.py/inner.py", "inner\n")
        diff = self.run_diff(base)
        kinds = {e["path"]: e["kind"] for e in diff["added"]}
        self.assertEqual(kinds.get("keep.py"), "directory")
        self.assertEqual([e["path"] for e in diff["removed"] if e["path"] == "keep.py"],
                         ["keep.py"])
        self.assertFalse([e for e in diff["modified"] if e["path"] == "keep.py"])

    def test_diff_rejects_bad_base(self):
        self.put("app.py")
        _, manifest = self.save_manifest()
        import json
        cases = {
            "not json": "{oops",
            "wrong schema": json.dumps({**manifest, "schema": "jash.other.v9"}),
            "generator mismatch": json.dumps({**manifest, "generator": "project-v1"}),
            "missing entries": json.dumps({k: v for k, v in manifest.items() if k != "entries"}),
            "entries not array": json.dumps({**manifest, "entries": {}}),
            "bad hash": json.dumps({**manifest, "entries": [
                {**manifest["entries"][0], "blake3": "0" * 63 + "G"}]}),
            "dir with hash": json.dumps({**manifest, "entries": [
                {"path": "d", "kind": "directory", "language": None,
                 "blake3": "0" * 64, "bytes": 1, "lines": 1}]}),
            "duplicate path": json.dumps({**manifest, "entries":
                manifest["entries"] + manifest["entries"][:1]}),
            "unknown kind": json.dumps({**manifest, "entries": [
                {**manifest["entries"][0], "kind": "mystery"}]}),
        }
        for name, content in cases.items():
            with self.subTest(name=name):
                victim = self.base / "bad.json"
                victim.write_text(content)
                self.run_digest("--diff-base", str(victim), ok=False)
        missing = self.base / "absent.json"
        self.run_digest("--diff-base", str(missing), ok=False)
        link = self.base / "link.json"
        (self.base / "base.json").write_text(json.dumps(manifest))
        link.symlink_to(self.base / "base.json")
        self.run_digest("--diff-base", str(link), ok=False)

    def test_diff_flag_conflicts(self):
        self.put("app.py")
        base, _ = self.save_manifest()
        for args in (("--session",), ("--manifest",), ("--fingerprint",),
                     ("--brief",), ("--deep",), ("--refresh",)):
            with self.subTest(args=args):
                self.run_digest("--diff-base", str(base), *args, ok=False)
        self.run_digest(command="digest --diff-base", ok=False)
        self.run_digest(command="digest --diff-base ''", ok=False)
        # --no-cache is accepted (diff never touches the briefing cache),
        # --jobs applies to the shared inventory walk.
        diff = self.run_diff(base, "--no-cache")
        self.assertEqual((diff["added"], diff["modified"], diff["removed"]), ([], [], []))

    def ledger_lines(self):
        import json
        path = self.home / ".jash/digest/ledger.jsonl"
        if not path.is_file():
            return []
        return [json.loads(line) for line in path.read_text().splitlines()]

    def test_ledger_records_hit_and_miss(self):
        self.put("app.py", "one\n")
        first = self.run_digest("--brief")
        second = self.run_digest("--brief")
        self.assertEqual(first, second)
        lines = self.ledger_lines()
        self.assertEqual(len(lines), 2)
        for line in lines:
            self.assertEqual(line["schema"], "jash.digest-receipt.v1")
            self.assertEqual(line["generator"], "project-v5")
            self.assertEqual(line["root"], str(self.root))
            self.assertEqual(line["level"], "brief")
            self.assertEqual(line["fingerprint"], self.fingerprint())
            self.assertEqual(line["files"], 1)
        self.assertFalse(lines[0]["served_from_cache"])
        self.assertTrue(lines[1]["served_from_cache"])

    def test_ledger_absent_for_pure_modes(self):
        self.put("app.py")
        self.fingerprint()
        self.run_digest("--manifest", "--no-cache")
        self.run_digest("--brief", "--no-cache")
        self.run_digest("--brief", "--no-cache", "--refresh")
        base, _ = self.save_manifest()
        self.run_diff(base)
        self.assertEqual(self.ledger_lines(), [])
        self.assertFalse((self.home / ".jash/digest").exists())

    def test_ledger_query(self):
        import json
        self.put("app.py")
        self.run_digest("--brief")
        self.run_digest("--standard")
        lines = self.ledger_lines()
        self.assertEqual(len(lines), 2)
        out = self.run_digest(command="digest --ledger")
        self.assertEqual([json.loads(line) for line in out.splitlines()], lines)
        out = self.run_digest(command="digest --ledger 1")
        self.assertEqual([json.loads(line) for line in out.splitlines()], lines[1:])
        for args in (("--ledger", "0"), ("--ledger", "1001"),
                     ("--ledger", "x"), ("--ledger", "--ledger"),
                     ("--cache-status", "--cache-status")):
            with self.subTest(args=args):
                self.run_digest(*args, ok=False)
        self.run_digest(command="digest --ledger --cache-status", ok=False)
        for args in (("--session", "--brief"), ("--manifest",),
                     ("--fingerprint",), ("--brief",), ("--refresh",)):
            with self.subTest(args=args):
                self.run_digest("--ledger", *args, ok=False)
                self.run_digest("--cache-status", *args, ok=False)
        self.run_digest(command="digest --ledger 1 extra-root-arg", ok=False)

    def test_ledger_absent_initially(self):
        # An absent ledger is empty output, not an error, and creates nothing.
        self.assertEqual(self.run_digest(command="digest --ledger"), "")
        self.assertFalse((self.home / ".jash/digest").exists())

    def test_cache_status(self):
        import json
        empty = json.loads(self.run_digest(command="digest --cache-status"))
        self.assertEqual(empty["schema"], "jash.digest-cache-status.v1")
        self.assertEqual((empty["entries"], empty["bytes"],
                          empty["oldest"], empty["newest"]), (0, 0, 0, 0))
        self.assertFalse((self.home / ".jash/digest").exists())
        self.put("app.py")
        self.run_digest("--brief")
        status = json.loads(self.run_digest(command="digest --cache-status"))
        self.assertEqual(status["entries"], 1)
        self.assertGreater(status["bytes"], 0)
        self.assertGreater(status["newest"], 0)
        self.assertLessEqual(status["oldest"], status["newest"])

    def test_ledger_rotation(self):
        import json
        self.put("app.py")
        (self.home / ".jash").mkdir(mode=0o700)
        cache = self.home / ".jash/digest"
        cache.mkdir(mode=0o700)
        filler = {"schema": "jash.digest-receipt.v1"}
        with (cache / "ledger.jsonl").open("w") as handle:
            handle.write((json.dumps(filler) + "\n") * 30000)
        (cache / "ledger.jsonl").chmod(0o600)
        self.assertGreater((cache / "ledger.jsonl").stat().st_size, 1024 * 1024)
        self.run_digest("--brief")
        rotated = cache / "ledger.jsonl.1"
        self.assertTrue(rotated.is_file())
        lines = self.ledger_lines()
        self.assertEqual(len(lines), 1)
        self.assertEqual(lines[0]["schema"], "jash.digest-receipt.v1")
        self.assertIn("fingerprint", lines[0])

    def cas_objects(self):
        found = []
        cas = self.home / ".jash/digest/cas"
        if cas.is_dir():
            for shard in sorted(cas.iterdir()):
                if shard.is_dir():
                    found.extend(sorted(p for p in shard.iterdir() if p.is_file()))
        return found

    def fetch(self, content_hash, **kw):
        return self.run_digest(command=f"digest --fetch {content_hash}", **kw)

    def test_fetch_round_trip(self):
        self.put("app.py", "fetch me\n")
        self.run_digest("--brief")
        manifest = self.run_digest("--manifest", "--no-cache")
        import json
        entry = next(e for e in json.loads(manifest)["entries"] if e["path"] == "app.py")
        self.assertEqual(self.fetch(entry["blake3"]), "fetch me\n")
        other = self.base / "Other"
        other.mkdir()
        self.put("big.c", "int x;\n" * 20000, root=other)
        self.run_digest("--brief", "--jobs", "8", root=other)
        manifest = self.run_digest("--manifest", "--no-cache", root=other)
        entry = next(e for e in json.loads(manifest)["entries"] if e["path"] == "big.c")
        self.assertEqual(self.fetch(entry["blake3"]), "int x;\n" * 20000)

    def test_fetch_dedup_across_roots(self):
        other = self.base / "Other"
        other.mkdir()
        self.put("common.py", "shared\n")
        self.put("only_a.py", "aaa\n")
        self.put("common.py", "shared\n", root=other)
        self.put("only_b.py", "bbb\n", root=other)
        self.run_digest("--brief")
        self.run_digest("--brief", root=other)
        objects = self.cas_objects()
        self.assertEqual(len(objects), 3)
        import json
        manifest = self.run_digest("--manifest", "--no-cache")
        entry = next(e for e in json.loads(manifest)["entries"] if e["path"] == "common.py")
        self.assertEqual(self.fetch(entry["blake3"]), "shared\n")
        manifest = self.run_digest("--manifest", "--no-cache", root=other)
        other_entry = next(e for e in json.loads(manifest)["entries"] if e["path"] == "common.py")
        self.assertEqual(other_entry["blake3"], entry["blake3"])

    def test_fetch_unknown_and_bad_hash(self):
        self.put("app.py")
        self.run_digest("--brief")
        self.fetch("0" * 64, ok=False)
        for bad in ("xyz", "0" * 63, "0" * 65, "A" * 64, ""):
            with self.subTest(bad=bad):
                self.run_digest(command=f"digest --fetch {bad}", ok=False)

    def test_fetch_flag_conflicts(self):
        self.put("app.py")
        self.run_digest("--brief")
        for args in (("--session",), ("--brief",), ("--manifest",),
                     ("--fingerprint",), ("--refresh",), ("--no-cache",),
                     ("--jobs", "2"), ("--ledger",), ("--cache-status",)):
            with self.subTest(args=args):
                self.run_digest("--fetch", "0" * 64, *args, ok=False)
        self.run_digest(command="digest --fetch", ok=False)
        self.run_digest(command="digest --fetch a b", ok=False)

    def test_pure_modes_do_not_populate_cas(self):
        self.put("app.py")
        self.fingerprint()
        self.run_digest("--manifest", "--no-cache")
        self.run_digest("--brief", "--no-cache")
        base, _ = self.save_manifest()
        self.run_diff(base)
        self.assertFalse((self.home / ".jash/digest").exists())

    def test_cas_quota_disables_population(self):
        self.put("app.py", "fetch me\n")
        (self.home / ".jash").mkdir(mode=0o700)
        (self.home / ".jash/digest").mkdir(mode=0o700)
        cas = self.home / ".jash/digest/cas"
        cas.mkdir(mode=0o700)
        for shard in ("aa", "bb", "cc", "dd", "ee", "ff"):
            folder = cas / shard
            folder.mkdir(mode=0o700)
            with (folder / "bulk").open("wb") as handle:
                handle.truncate(100 * 1024 * 1024)
        self.run_digest("--brief")
        import json
        manifest = self.run_digest("--manifest", "--no-cache")
        entry = next(e for e in json.loads(manifest)["entries"] if e["path"] == "app.py")
        self.fetch(entry["blake3"], ok=False)
        self.assertEqual(len(self.cas_objects()), 6)

    def test_diff_is_deterministic_across_budgets(self):
        self.put("README.md", "# Orchard\n" + "line\n" * 100)
        self.put("big.c", "int x;\n" * 20000)
        for i in range(16):
            self.put(f"pkg/mod{i:02}.py", "x = 1\n" * (i + 1))
        base, _ = self.save_manifest()
        self.put("pkg/mod00.py", "x = 2\n")
        outputs = [self.run_digest("--diff-base", str(base), "--jobs", str(jobs))
                   for jobs in (1, 2, 8)]
        outputs.append(self.run_digest("--diff-base", str(base)))
        self.assertEqual(len(set(outputs)), 1)
        import json
        diff = json.loads(outputs[0])
        self.assertEqual([e["path"] for e in diff["modified"]], ["pkg/mod00.py"])


if __name__ == "__main__":
    unittest.main()
