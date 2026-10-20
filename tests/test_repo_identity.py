#!/usr/bin/env python3
"""The module writer and core memory reader must use the same evidence scope."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import json

ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get('JASH', ROOT / 'build/jash')).resolve()

class RepositoryIdentity(unittest.TestCase):
    def check_scope(self, marker):
        with tempfile.TemporaryDirectory(prefix='jash-repo-identity-') as tmp:
            home, parent = Path(tmp) / 'home', Path(tmp) / 'parent'
            home.mkdir(); parent.mkdir()
            work = parent / 'child'; work.mkdir()
            if marker == 'file': (parent / '.git').write_bytes(b'not a repository')
            elif marker == 'empty-directory': (parent / '.git').mkdir()
            elif marker == 'git':
                subprocess.run(['git','init','-q',str(parent)], check=True, capture_output=True)
            elif marker == 'worktree':
                control = Path(tmp) / 'control'; control.mkdir()
                (control / 'HEAD').write_text('ref: refs/heads/main\n')
                (parent / '.git').write_text('gitdir: ../control\n')
            env = dict(PATH=os.defpath, HOME=str(home), LC_ALL='C.UTF-8', JASH_SIGIL='1', JASH_ADOPT='0', JASH_NUDGE='0')
            subprocess.run(['bash',str(ROOT / 'tests/setup_sigil_modules.sh'),str(home)],env=env,check=True,capture_output=True)
            first = subprocess.run([str(JASH),'-c','echo observation'],cwd=work,env=env,capture_output=True)
            self.assertEqual(first.returncode,0,first.stderr)
            key = '-'
            if marker in ('git','worktree'):
                hashed = 1469598103934665603
                for byte in os.fsencode(parent): hashed = ((hashed ^ byte) * 1099511628211) & ((1 << 64)-1)
                key = f'{hashed:016x}'
            path = home / '.jash' / ('chain.log' if key == '-' else f'chain-{key}.log')
            self.assertTrue(path.exists(), list((home / '.jash').glob('chain*.log')))
            self.assertIn('cmd=echo observation',path.read_text())
            result = subprocess.run([str(JASH),'-c','mem distill --json'],cwd=work,env=env,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(json.loads(result.stdout)['chain_lines'],1)

    def test_stray_file_and_empty_directory_do_not_merge_scope(self):
        for marker in ('file','empty-directory'):
            with self.subTest(marker=marker): self.check_scope(marker)

    def test_repository_and_relative_worktree_keep_shared_scope(self):
        for marker in ('git','worktree'):
            with self.subTest(marker=marker): self.check_scope(marker)

if __name__ == '__main__': unittest.main()
