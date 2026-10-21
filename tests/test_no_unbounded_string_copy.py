#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-SPL-1.0
"""Production guard includes implementation fragments and header-only code.

Test fixtures are intentionally excluded. Comments and literals containing a
function name are not executable calls; line numbers remain source-accurate.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
NON_CODE = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
CALL = re.compile(r'\bstrcpy\s*\(')


def calls(text):
    code = NON_CODE.sub(lambda m: ''.join('\n' if c == '\n' else ' ' for c in m.group()), text)
    return [code.count('\n', 0, match.start()) + 1 for match in CALL.finditer(code)]


class NoUnboundedCopy(unittest.TestCase):
    def test_scanner(self):
        self.assertEqual(calls('/* strcpy(x,y); */\nconst char *s="strcpy(";\nstrcpy /* gap */\n(dst, src);'), [3])
        self.assertEqual(calls('// strcpy(a,b)\nstrcpy(a,b); strcpy(c,d);'), [2, 2])

    def test_production_sources(self):
        found = []
        for path in sorted((ROOT / 'c').rglob('*')):
            if path.suffix in ('.c', '.h', '.inc'):
                found.extend(f'{path.relative_to(ROOT)}:{line}' for line in calls(path.read_text()))
        self.assertEqual(found, [], 'Unbounded production string copy: ' + ', '.join(found))


if __name__ == '__main__':
    unittest.main()
