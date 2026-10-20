"""IM02: native digest coverage used by the impact index. Isolated fixtures only."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ImpactInventory(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='digest-impact-', dir='/tmp/opencode')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / 'project'
        self.root.mkdir()
        self.env = {'HOME': str(self.base / 'home'), 'JASH_SIGIL': '0', 'JASH_AGENT': '0', 'JASH_BRIEF': '0'}
        (self.base / 'home').mkdir()
        self.binary = os.environ['DIGEST_BINARY']

    def put(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def run_digest(self, mode):
        result = subprocess.run([self.binary, '-c', 'digest ' + mode], cwd=self.root,
                                env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_rust_ui_and_entrypoint_changes_invalidate(self):
        files = {'src/lib.rs': 'pub fn answer() -> u32 { 1 }\n',
                 'index.html': '<button>one</button>\n', 'style.css': 'body { color: red; }\n',
                 'view.svelte': '<button>one</button>\n',
                 'cli': '#!/usr/bin/env python3\nprint(1)\n'}
        paths = [self.put(name, text) for name, text in files.items()]
        first = self.run_digest('--fingerprint')
        for path in paths:
            before = path.stat()
            path.write_text(path.read_text() + '\n')
            os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
            after = self.run_digest('--fingerprint')
            self.assertNotEqual(first, after, path.name)
            first = after
            moved = path.with_name('renamed-' + path.name)
            path.rename(moved)
            after = self.run_digest('--fingerprint')
            self.assertNotEqual(first, after, 'rename ' + path.name)
            first = after
            moved.unlink()
            after = self.run_digest('--fingerprint')
            self.assertNotEqual(first, after, 'delete ' + path.name)
            first = after

    def test_manifest_declares_identity_paths_hashes_and_exclusions(self):
        self.put('Cargo.toml', '[package]\nname = "demo"\n')
        self.put('Cargo.lock', 'version = 3\n')
        self.put('src/lib.rs', 'pub fn answer() -> u32 { 42 }\n')
        self.put('index.html', '<main>Demo</main>\n')
        self.put('style.css', 'body { color: red; }\n')
        self.put('image.bin', 'opaque')
        result = json.loads(self.run_digest('--manifest'))
        self.assertEqual(result['schema'], 'jash.project-observation.v1')
        self.assertEqual(result['root'], str(self.root))
        self.assertEqual(result['fingerprint'] + '\n', self.run_digest('--fingerprint'))
        entries = {e['path']: e for e in result['entries']}
        self.assertEqual(entries['Cargo.toml']['kind'], 'build')
        self.assertEqual(entries['Cargo.lock']['kind'], 'build')
        self.assertEqual(entries['src/lib.rs']['language'], 'Rust')
        self.assertEqual(entries['style.css']['language'], 'CSS')
        self.assertNotIn('image.bin', entries)
        self.assertRegex(entries['src/lib.rs']['blake3'], '^[0-9a-f]{64}$')
        self.assertEqual(result['consistency'], 'best-effort')
        self.assertIn('target', result['coverage']['ignored_directories'])
        self.assertFalse((self.base / 'home/.jash/digest').exists())

    def test_manifest_quotes_and_unicode_paths(self):
        self.put('strange-"\\-\n-café.rs', 'fn hello() {}\n')
        result = json.loads(self.run_digest('--manifest'))
        self.assertEqual(result['entries'][0]['path'], 'strange-"\\-\n-café.rs')


if __name__ == '__main__':
    unittest.main()
