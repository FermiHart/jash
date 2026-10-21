#!/usr/bin/env python3
"""BASH_LINENO differential contract, using the installed Bash as the oracle.

Run: python3 tests/test_bash_lineno.py --jash /path/to/jash [--bash /path/to/bash]
JASH_BIN (then JASH, then build/jash) supplies the default binary. Both shells
receive identical source bytes and isolated temporary homes. Bundled native
modules must already be built; attestation is not disabled for this test.
--case NAME and --mode c|stdin|file restrict diagnostic runs, not CI coverage.
The full contract uses Bash 5 or newer as the oracle. Bash 3.2 numbers -c
input from zero; use --mode stdin --mode file for its additional legacy checks.
No oracle values or Jash source maps are adjusted to hide that difference.
"""
import argparse
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BEGIN = '__JASH_LINENO_BEGIN__\n'
END = '__JASH_LINENO_END__\n'


def observe(label):
    """Inline observation: a helper shell function would change the stack."""
    return (
        "printf '" + label + " scalar=<%s> braced=<%s> zero=<%s> one=<%s> far=<%s> "
        "star=<%s> count=<%s> scalarlen=<%s> set=<%s> default=<%s>\\n' "
        '"$BASH_LINENO" "${BASH_LINENO}" "${BASH_LINENO[0]}" '
        '"${BASH_LINENO[1]}" "${BASH_LINENO[99]}" "${BASH_LINENO[*]}" '
        '"${#BASH_LINENO[@]}" "${#BASH_LINENO}" "${BASH_LINENO+x}" '
        '"${BASH_LINENO:-missing}"\n'
        'set -- "${BASH_LINENO[@]}"\n'
        "printf '" + label + " at-count=<%s>\\n' \"$#\"\n"
        "printf '" + label + " at-item=<%s>\\n' \"$@\"\n"
        'set -- "${BASH_LINENO[*]}"\n'
        "printf '" + label + " star-count=<%s> star-item=<%s>\\n' \"$#\" \"$1\"\n"
    )


def cases():
    """No expected trace is generated from Jash implementation details."""
    o = observe
    return {
        'top_level': (o('top') + '\n# blank/comment must not create a frame\n' + o('after'), {}),
        'function': ('f() {\n' + o('f') + '}\n\n# call on its physical line\nf\n' + o('after'), {}),
        'inline_semicolons': ('f() { printf "inline=<%s>\\n" "${BASH_LINENO[*]}"; }; f; f\n' + o('after'), {}),
        'nested_functions': ('inner() {\n' + o('inner') + '}\nouter() {\n' + o('outer') + 'inner\n' + o('outer_after') + '}\n\nouter\n' + o('after'), {}),
        'recursion': ('recur() {\nif [ "$1" -gt 0 ]; then\nrecur "$(($1 - 1))"\nfi\n' + o('recur') + '}\nrecur 2\n' + o('after'), {}),
        'loop_callsite': ('f() {\n' + o('loop') + '}\nfor n in 1 2 3; do\nf\ndone\n' + o('after'), {}),
        'pipeline_compound_for': ('f(){ printf "frame=<%s>\\n" "${BASH_LINENO[*]}"; };\nfor x in a; do\nf\ndone | cat\n', {}),
        'pipeline_compound_if': ('f(){ printf "frame=<%s>\\n" "${BASH_LINENO[*]}"; };\nif true; then\nf\nfi | cat\n', {}),
        'background_function': ('f(){ printf "frame=<%s>\\n" "${BASH_LINENO[*]}"; };\n\nf & wait\n', {}),
        'arithmetic_index': ('f(){ printf "index=<%s>\\n" "${BASH_LINENO[1+0]}"; };\ng(){\nf\n};\ng\n', {}),
        'active_function_unset': ('f(){ unset -f f; printf "frame=<%s>\\n" "${BASH_LINENO[*]}"; };\nf\n', {}),
        'pipeline_function': ('f() {\n' + o('pipe') + '}\nf | cat\n' + o('after'), {}),
        'source': ('source ./one.sh\n' + o('after'), {'one.sh': '#!/bin/sh\n\n# source origin\n' + o('source')}),
        'nested_source': ('outer() {\nsource ./one.sh\n' + o('outer_after') + '}\nouter\n' + o('after'), {
            'one.sh': '\n# sourced inside function\n' + o('one') + '. ./two.sh\n' + o('one_after'),
            'two.sh': '#!/bin/sh\n\n' + o('two'),
        }),
        'retained_source_function': ('source ./definitions.sh\n\n# definition origin must survive source return\nfrom_source\n' + o('after'), {
            'definitions.sh': '#!/bin/sh\n\nleaf() {\n' + o('leaf') + '}\n\nfrom_source() {\n' + o('from_source') + 'leaf\n}\n',
        }),
        'subshell': ('f() {\n(\n' + o('subshell') + ')\n' + o('parent') + '}\nf\n' + o('after'), {}),
        'command_substitution': ('f() {\ncaptured=$(\n' + o('cmdsub') + ')\nprintf "%s\\n" "$captured"\n' + o('parent') + '}\nf\n' + o('after'), {}),
        'source_in_command_substitution': ('f() {\ncaptured=$(source ./one.sh)\nprintf "%s\\n" "$captured"\n' + o('parent') + '}\nf\n' + o('after'), {
            'one.sh': '\n' + o('source_cmdsub'),
        }),
        'function_return_unwind': ('f() {\n' + o('before_return') + 'return 17\nprintf "UNREACHABLE\\n"\n}\nf || printf "return=<%s>\\n" "$?"\n' + o('after') + 'f || true\n' + o('again'), {}),
        'source_return_unwind': ('source ./one.sh || printf "return=<%s>\\n" "$?"\n' + o('after'), {
            'one.sh': o('source_return') + 'return 19\nprintf "UNREACHABLE\\n"\n',
        }),
        'source_return_in_function': ('f() {\nsource ./one.sh || printf "source-return=<%s>\\n" "$?"\n' + o('function_after_source') + '}\nf\n' + o('after'), {
            'one.sh': o('source_return_nested') + 'return 19\nprintf "UNREACHABLE\\n"\n',
        }),
        'empty_sources': ('source ./empty.sh\nsource ./comments.sh\n' + o('after_empty') + 'f() {\nsource ./empty.sh\n' + o('function_after_empty') + '}\nf\n', {
            'empty.sh': '', 'comments.sh': '#!/bin/sh\n\n# no command\n',
        }),
        'command_error_unwind': ('f() {\ncommand_that_does_not_exist_for_lineno_test\nprintf "missing=<%s>\\n" "$?"\n' + o('after_error') + 'return 23\n}\nf || printf "return=<%s>\\n" "$?"\n' + o('after'), {}),
        'return_trap': ('f() {\ntrap \'printf "return-trap=<%s>\\n" "${BASH_LINENO[*]}"\' RETURN\n' + o('f') + 'return 0\n}\nf\n' + o('after'), {}),
        'heredoc_provenance': ('f() {\n' + o('after_heredoc') + '}\ncat <<\'PAYLOAD\'\nfirst\nsecond\nPAYLOAD\n\nf\n' + o('after'), {}),
        'continuation_provenance': ('f() {\n' + o('continued') + '}\nprintf "%s\\n" \\\n"continuation"\ntrue &&\nf\n' + o('after'), {}),
        'quoted_star_ifs': ('inner() {\nIFS=:\n' + o('colon') + 'unset IFS\n}\nouter() {\ninner\n}\nouter\n' + o('after'), {}),
    }


class BashLinenoDifferential(unittest.TestCase):
    def run_shell(self, executable, mode, script, root, home, is_bash):
        env = {key: value for key, value in os.environ.items()
               if key not in ('BASH_ENV', 'ENV', 'SHELLOPTS', 'BASHOPTS', 'CDPATH')
               and not key.startswith('BASH_FUNC_')}
        env.update(HOME=str(home), LC_ALL='C', TERM='dumb',
                   JASH_BRIEF='0', JASH_NUDGE='0', JASH_MEMO='0', JASH_ADOPT='0')
        # Do not inherit an unrelated user's opt-out from the tested shell's
        # required attestation. Native test modules are copied into this HOME.
        env.pop('JASH_SIGIL', None)
        env.pop('JASH_SIGIL_ALLOW_DEGRADED', None)
        command = [str(executable)] + (['--noprofile', '--norc'] if is_bash else [])
        stdin = None
        if mode == 'c':
            command += ['-c', script]
        elif mode == 'file':
            command += [str(root / 'main.sh')]
        else:
            stdin = script
        return subprocess.run(command, input=stdin, text=True, capture_output=True,
                              cwd=root, env=env, timeout=OPTIONS.timeout)

    def observed(self, result, label):
        self.assertEqual(result.returncode, 0, label + '\n' + result.stderr + '\n' + result.stdout)
        self.assertEqual(result.stdout.count(BEGIN), 1, label + ': missing/duplicate begin marker\n' + result.stdout)
        self.assertEqual(result.stdout.count(END), 1, label + ': missing/duplicate end marker\n' + result.stdout)
        prefix, remainder = result.stdout.split(BEGIN, 1)
        observed, suffix = remainder.split(END, 1)
        # Startup messages outside markers are not caller-stack observations.
        return observed

    def test_bash_lineno_matches_reference(self):
        self.assertTrue(OPTIONS.bash.is_file(), 'Bash oracle binary is missing')
        self.assertTrue(OPTIONS.jash.is_file(), 'Jash binary is missing; build jash and modules first')
        native_modules = sorted((ROOT / 'modules').glob('*/*.modjash'))
        self.assertTrue(any(path.name == 'sigil.modjash' for path in native_modules),
                        'Build native modules first; this test does not disable required attestation')
        selected = cases()
        for name in OPTIONS.case or selected:
            body, files = selected[name]
            # Identical marker/shebang bytes in both shells preserve a genuine
            # physical-line oracle, including the file-mode main frame.
            script = '#!/bin/sh\n' + "printf '__JASH_LINENO_BEGIN__\\n'\n" + body + "printf '__JASH_LINENO_END__\\n'\n"
            for mode in OPTIONS.mode or ('c', 'stdin', 'file'):
                with self.subTest(case=name, mode=mode), tempfile.TemporaryDirectory(prefix='jash-lineno-') as directory:
                    root = Path(directory)
                    for filename, content in {'main.sh': script, **files}.items():
                        (root / filename).write_text(content)
                    homes = [root / 'bash-home', root / 'jash-home']
                    for home in homes:
                        module_dir = home / '.jash/modules'
                        module_dir.mkdir(parents=True)
                        for path in native_modules:
                            shutil.copy2(path, module_dir / path.name)
                    reference = self.run_shell(OPTIONS.bash, mode, script, root, homes[0], True)
                    actual = self.run_shell(OPTIONS.jash, mode, script, root, homes[1], False)
                    expected = self.observed(reference, 'Bash ' + name + '/' + mode)
                    got = self.observed(actual, 'Jash ' + name + '/' + mode)
                    self.assertEqual(got, expected, name + '/' + mode + '\nJash stderr:\n' + actual.stderr)

    def test_multiline_cmdsub_physical_provenance(self):
        """Jash contract, not differential: Bash biases these to closing line.

        The invocation of f is physically on line 4, and that of g on line 7.
        """
        script = ('f(){ printf "frame=<%s>\\n" "${BASH_LINENO[*]}"; };\n'
                  'g(){\necho "$(\nf\n)"\n};\ng\n')
        modules = sorted((ROOT / 'modules').glob('*/*.modjash'))
        for mode in OPTIONS.mode or ('c', 'stdin', 'file'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix='jash-physical-') as directory:
                root = Path(directory)
                (root / 'main.sh').write_text(script)
                module_dir = root / 'home/.jash/modules'
                module_dir.mkdir(parents=True)
                for path in modules:
                    shutil.copy2(path, module_dir / path.name)
                actual = self.run_shell(OPTIONS.jash, mode, script, root, root / 'home', False)
                self.assertEqual(actual.returncode, 0, actual.stderr)
                expected = 'frame=<4 7' + (' 0' if mode == 'file' else '') + '>\n'
                self.assertEqual(actual.stdout, expected)


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jash', type=Path, default=Path(os.environ.get('JASH_BIN', os.environ.get('JASH', ROOT / 'build/jash'))))
    parser.add_argument('--bash', type=Path, default=Path(shutil.which('bash') or '/bin/bash'))
    parser.add_argument('--case', action='append', choices=tuple(cases()))
    parser.add_argument('--mode', action='append', choices=('c', 'stdin', 'file'))
    parser.add_argument('--timeout', type=float, default=10)
    options = parser.parse_args()
    if options.timeout <= 0:
        parser.error('--timeout must be positive')
    options.jash = options.jash.resolve()
    options.bash = options.bash.resolve()
    try:
        version_result = subprocess.run(
            [str(options.bash), '--version'], text=True, capture_output=True,
            env={**os.environ, 'LC_ALL': 'C'}, timeout=options.timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        parser.error('Cannot identify Bash reference: ' + str(error))
    version = version_result.stdout.splitlines()
    match = re.search(r'version (\d+)\.(\d+)', version[0] if version else '')
    if version_result.returncode or not match:
        parser.error('Reference must identify itself as GNU Bash with --version')
    print('Bash reference: ' + str(options.bash) + '\n' + version[0], flush=True)
    if int(match.group(1)) < 5 and ('c' in (options.mode or ('c', 'stdin', 'file'))):
        parser.error('Bash 5 or newer is required for the -c physical-line oracle; '
                     'legacy Bash 3.2 uses zero-based -c lines. For legacy coverage '
                     'select --mode stdin --mode file explicitly.')
    return options


if __name__ == '__main__':
    OPTIONS = arguments()
    unittest.main(argv=[__file__], verbosity=2)
