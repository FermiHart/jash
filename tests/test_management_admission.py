#!/usr/bin/env python3
"""Failed response admission must precede management effects, in both formats."""
import json
import os
import shlex
import subprocess
import unittest
import test_management_builtins as management


class ManagementAdmission(unittest.TestCase):
    setUp = management.ManagementBuiltins.setUp
    run_cli = management.ManagementBuiltins.run_cli
    execute = management.ManagementBuiltins.execute
    validate = management.ManagementBuiltins.validate
    report = management.ManagementBuiltins.report
    compile_library = management.ManagementBuiltins.compile_library
    plugin_fixture = management.ManagementBuiltins.plugin_fixture
    module_fixture = management.ManagementBuiltins.module_fixture

    def test_real_handler_stops_batch_and_closes_collection_failure(self):
        # Link the actual argv handler and codec. Discard unused legacy entry
        # points so this gate does not replace the shell parser with a stub.
        sources = ['tests/test_discovery_effects.c', 'c/builtins/discovery_output.c',
                   'c/builtins/alias.c', 'c/core/alias.c', 'c/core/structured_output.c',
                   'c/core/json_writer.c', 'c/core/data_json.c', 'c/core/data_value.c',
                   'c/core/data_stream.c', 'c/core/data_wire.c', 'c/core/data_wire_decode.c',
                   'c/core/data_wire_import.c', 'c/core/data_emit.c', 'c/module/blake3.c',
                   'c/theme/io.c']
        binary = self.directory / 'effects-fixture'
        command = [os.environ.get('CC', 'cc'), '-std=gnu11', '-D_GNU_SOURCE', '-DJASH_TARGET_POSIX',
                   '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                   '-ffunction-sections', '-Wl,--gc-sections', '-I' + str(management.ROOT / 'c'),
                   *[str(management.ROOT / source) for source in sources], '-lm', '-o', str(binary)]
        if os.environ.get('SANITIZE') == '1':
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        built = subprocess.run(command, capture_output=True, timeout=30)
        self.assertEqual(built.returncode, 0, built.stderr)
        result = subprocess.run([str(binary)], capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = self.validate(json.loads(result.stdout))
        self.assertFalse(report['complete'])
        self.assertEqual(report['status'], 75)
        self.assertTrue(report['error']['effects_may_have_applied'])
        self.assertEqual(report['items'][0]['value'], 'value1')

    def test_alias_and_unalias_do_not_apply_after_failed_admission(self):
        for selector in ('--json', '--data'):
            with self.subTest(selector=selector):
                result = self.run_cli('-c', f'alias {selector} admitted=value >/dev/full; alias --json admitted', status=1)
                self.assertEqual(self.validate(json.loads(result.stdout))['items'][0]['error'], 'not_found')
                result = self.run_cli('-c', f'alias admitted=value; unalias {selector} admitted >/dev/full; alias --json admitted')
                self.assertEqual(self.validate(json.loads(result.stdout))['items'][0]['value'], 'value')

    def test_enable_and_hash_do_not_apply_after_failed_admission(self):
        for selector in ('--json', '--data'):
            with self.subTest(selector=selector):
                result = self.run_cli('-c', f'enable {selector} -n pwd >/dev/full; enable --json -n')
                self.assertEqual(self.validate(json.loads(result.stdout))['items'], [])
                result = self.run_cli('-c', f'hash {selector} sh >/dev/full; hash --json')
                self.assertEqual(self.validate(json.loads(result.stdout))['items'], [])

    def test_native_code_is_not_initialized_after_failed_admission(self):
        for name, fixture in [('plugin', self.plugin_fixture()), ('module', self.module_fixture())]:
            for selector in ('--json', '--data'):
                with self.subTest(name=name, selector=selector):
                    result = self.run_cli('-c', f'{name} {selector} load {shlex.quote(str(fixture))} >/dev/full; {name} --json list')
                    self.assertEqual(self.validate(json.loads(result.stdout))['items'], [])
                    self.assertNotIn(b'fixture-' + name.encode() + b'-init', result.stderr)

    def test_persistent_files_are_not_changed_after_failed_admission(self):
        for selector in ('--json', '--data'):
            with self.subTest(selector=selector):
                state = self.directory / '.jash_state'
                state.unlink(missing_ok=True)
                with open('/dev/full', 'wb') as sink:
                    self.execute(['jash_state', selector, 'save'], status=125, stdout=sink)
                self.assertFalse(state.exists())
                state.write_bytes(b'synthetic existing file')
                with open('/dev/full', 'wb') as sink:
                    self.execute(['jash_state', selector, 'clear'], status=125, stdout=sink)
                self.assertEqual(state.read_bytes(), b'synthetic existing file')
                target = self.directory / '.config/jash/groq.key'
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text('synthetic-old-value\n')
                with open('/dev/full', 'wb') as sink:
                    self.execute(['config', selector, 'set-key', 'synthetic-new-value'], status=125, stdout=sink)
                self.assertEqual(target.read_text(), 'synthetic-old-value\n')

    def test_mutation_receipts_survive_presentation_budget(self):
        self.env['JASH_LLM_BUDGET'] = '1'
        for selector in ('--json', '--data'):
            with self.subTest(selector=selector):
                result = self.execute(['alias', selector, 'budget_alias=value'])
                if selector == '--json':
                    row = self.validate(json.loads(result.stdout))['items'][0]
                else:
                    from test_discovery_builtins import native_stream, project
                    rows, terminal = native_stream(result.stdout)
                    self.assertEqual(terminal['state'], 0)
                    row = project(rows[0])
                self.assertTrue(row['changed'])
                self.assertEqual(row['value'], 'value')
        # Read-only reports still honor the presentation budget.
        result = self.execute(['alias', '--json'], status=75)
        self.assertEqual(json.loads(result.stdout)['error']['code'], 'output_budget_exceeded')


if __name__ == '__main__':
    unittest.main(verbosity=2)
