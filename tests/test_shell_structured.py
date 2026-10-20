#!/usr/bin/env python3
"""Structured shell state is checked against subsequent shell/OS observations."""
import base64
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()


class ShellState(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.validators = {}
        for name in ['shell-state', 'cwd', 'directory-stack', 'error']:
            schema = json.loads((ROOT / 'schemas' / ('builtin-' + name + '-v1.schema.json')).read_text())
            Draft202012Validator.check_schema(schema)
            key = schema['properties']['schema']['const']
            cls.validators[key] = Draft202012Validator(schema)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="jash-shell-data-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.env = dict(PATH=os.defpath, HOME=str(self.directory), LC_ALL="C.UTF-8",
                        JASH_SIGIL="0", JASH_ADOPT="0", JASH_NUDGE="0",
                        JASH_MEMO="0", JASH_LOOPGUARD="off")

    def runj(self, command, status=0, **kwargs):
        result = subprocess.run([str(BINARY), "-c", command], env=self.env,
                                cwd=self.directory, capture_output=True, timeout=20, **kwargs)
        self.assertEqual(result.returncode, status, (result.stdout, result.stderr))
        return result

    def documents(self, command, status=0):
        documents = [json.loads(line) for line in self.runj(command, status).stdout.splitlines()]
        for document in documents:
            if document.get('schema') in self.validators:
                self.validators[document['schema']].validate(document)
        return documents

    def test_export_observed_value_and_literal_boundaries(self):
        value = 'space \" quote \\\n tab\t' + 'x' * 3000
        reports = self.documents('export --json ' + shlex.quote('AUDIT=' + value) + '; env --json')
        first = reports[0]["items"][0]
        self.assertEqual(first["name"], "AUDIT")
        self.assertEqual(first["value"], value)
        self.assertTrue(first["changed"])
        self.assertEqual(reports[1]["AUDIT"], value)

    def test_export_partial_failure_retains_each_outcome(self):
        report, = self.documents('export --json GOOD=ok 1bad=x ALSO=yes', status=1)
        self.assertEqual(report["status"], 1)
        self.assertEqual([row["error"] for row in report["items"]], [None, "invalid_identifier", None])
        self.assertEqual([row["changed"] for row in report["items"]], [True, False, True])

    def test_readonly_is_enforced_and_unset_reports_actual_state(self):
        rows = self.documents('readonly --json A=keep; export --json A=change; unset --json A', status=1)
        self.assertTrue(rows[0]["items"][0]["readonly"])
        for report in rows[1:]:
            self.assertEqual(report["items"][0]["error"], "readonly_variable")
            self.assertEqual(report["items"][0]["value"], "keep")
            self.assertFalse(report["items"][0]["changed"])

    def test_declare_and_array_query_are_native(self):
        reports = self.documents("declare --json -l 'WORD=UPPER SPACE'; array --json 'A[7]=two words'; declare --json -p A")
        self.assertEqual(reports[0]["items"][0]["value"], "upper space")
        self.assertEqual(reports[1]["items"][0]["value"], "two words")
        found = next(row for row in reports[2]["items"] if row["name"] == "A[7]")
        self.assertEqual(found["value"], "two words")

    def test_array_initializers_keep_exact_elements_and_changes(self):
        reports = self.documents("declare --json -a 'A=(\"two words\" \"Olá ∞\" [5]=last)'; "
                                 "declare --json -a 'A=(\"two words\" new)'; declare --json -p A")
        self.assertEqual([r['value'] for r in reports[0]['items']], ['two words', 'Olá ∞', 'last'])
        self.assertEqual([r['name'] for r in reports[0]['items']], ['A[0]', 'A[1]', 'A[5]'])
        self.assertEqual([r['changed'] for r in reports[1]['items']], [False, True])
        # The established flat-array model updates named slots, retaining others.
        final = {r['name']: r['value'] for r in reports[2]['items']}
        self.assertEqual(final, {'A[0]': 'two words', 'A[1]': 'new', 'A[5]': 'last'})

    def test_inherited_environment_is_not_truncated(self):
        self.env['LONG_INHERITED_VALUE'] = 'v' * 15000
        report, = self.documents('env --json')
        self.assertEqual(report['LONG_INHERITED_VALUE'], self.env['LONG_INHERITED_VALUE'])

    def test_set_and_shopt_query_do_not_change_state(self):
        reports = self.documents('set --json -o pipefail; set --json -o; shopt --json -s nullglob; shopt --json nullglob')
        for report in reports[:2]:
            self.assertTrue(next(row for row in report["items"] if row["name"] == "pipefail")["enabled"])
        for report in reports[2:]:
            self.assertTrue(report["items"][0]["enabled"])
        reports = self.documents('shopt --json -u nullglob; shopt --json nullglob')
        self.assertFalse(reports[1]["items"][0]["enabled"])

    def test_umask_matches_created_file_mode(self):
        report, = self.documents('umask --json 077; touch created')
        self.assertEqual(report["items"][0]["mask"], 0o77)
        self.assertEqual((self.directory / "created").stat().st_mode & 0o777, 0o600)

    def test_resource_limits_and_cpu_times_are_exact_units(self):
        reports = self.documents('ulimit --json -Sn 512; ulimit --json -Sn; times --json')
        for report in reports[:2]:
            self.assertEqual(report["items"][0]["soft"], 512)
            self.assertEqual(report["items"][0]["display_scale"], 1)
        row = reports[2]["items"][0]
        self.assertGreater(row["ticks_per_second"], 0)
        self.assertIsInstance(row["shell_user_ticks"], int)

    def test_invalid_options_do_not_apply_changes(self):
        reports = self.documents('umask --json 7777; umask --json')
        self.assertFalse(reports[0]["complete"])
        self.assertNotEqual(reports[1]["items"][0]["mask"], 0o777)
        reports = self.documents('set --json -e -Z; set --json -o')
        self.assertFalse(reports[0]["complete"])
        self.assertFalse(next(row for row in reports[1]["items"] if row["name"] == "errexit")["enabled"])

    def test_trap_reads_actual_handler_without_running_it(self):
        action = 'echo action-must-not-run'
        reports = self.documents('trap --json ' + shlex.quote(action) + ' USR1; trap --json -p USR1')
        for report in reports:
            self.assertEqual(report["items"][0]["action"], action)

    def test_trap_filters_resets_and_partial_errors_are_observed(self):
        reports = self.documents("trap --json 'echo one' USR1; trap --json '' USR2; trap --json -p USR2; trap --json - USR1; trap --json -p USR1")
        self.assertTrue(reports[1]['items'][0]['ignored'])
        self.assertEqual([r['name'] for r in reports[2]['items']], ['USR2'])
        self.assertTrue(reports[3]['items'][0]['changed'])
        self.assertEqual(reports[3]['items'][0]['before'], 'echo one')
        self.assertFalse(reports[4]['items'][0]['present'])
        report, = self.documents("trap --json 'echo harmless' KILL USR1 bad", status=1)
        self.assertEqual([r['error'] for r in report['items']], ['signal_not_catchable', None, 'invalid_signal'])
        listing, = self.documents('trap --json -l')
        self.assertIn('TERM', [r['name'] for r in listing['items']])
        self.assertIn('EXIT', [r['name'] for r in listing['items']])

    def test_readonly_admission_prevents_partial_attribute_failure(self):
        names = ['R' + str(i) for i in range(64)]
        commands = ['readonly --json ' + ' '.join(name + '=old' for name in names),
                    'readonly --json OVERFLOW=new', 'env --json']
        reports = self.documents('; '.join(commands))
        self.assertEqual(reports[1]['items'][0]['error'], 'readonly_limit')
        self.assertFalse(reports[1]['items'][0]['changed'])
        self.assertNotIn('OVERFLOW', reports[2])

    def test_integer_error_and_combined_limit_selection(self):
        reports = self.documents("declare --json -i 'BAD=8/0'; env --json; ulimit --json -SHn 512; typeset --json WORD=value")
        self.assertEqual(reports[0]['items'][0]['error'], 'arithmetic_error')
        self.assertNotIn('BAD', reports[1])
        self.assertEqual(reports[2]['items'][0]['soft'], 512)
        self.assertEqual(reports[2]['items'][0]['hard'], 512)
        self.assertEqual(reports[3]['command'], 'typeset')

    def test_data_composition_keeps_path_and_values(self):
        self.env["AUDIT"] = "bytes survive"
        report = self.runj('env --data | pick --from-data name,value')
        rows = [json.loads(row) for row in report.stdout.splitlines()]
        self.assertEqual(next(row for row in rows if row["name"] == "AUDIT")["value"], "bytes survive")
        result = self.runj('pwd --data | pick --from-data cwd')
        self.assertEqual(json.loads(result.stdout)["cwd"], str(self.directory))

    def test_checked_delivery_and_json_budget(self):
        for command in ['export --json', 'set --json -o', 'umask --json', 'ulimit --json -a', 'times --json']:
            with self.subTest(command=command):
                self.runj(command + ' > /dev/full', status=125)
        self.env["JASH_LLM_BUDGET"] = "1"
        report, = self.documents('export --json', status=75)
        self.assertFalse(report["complete"])
        self.assertEqual(report["error"]["code"], "output_budget_exceeded")

    def test_cd_reports_kernel_and_logical_paths_without_splitting(self):
        target = self.directory / 'space \" newline\n'
        target.mkdir()
        reports = self.documents('cd --json ' + shlex.quote(str(target)) + '; pwd --json; cd --json -; pwd --json')
        self.assertEqual(reports[0]["items"][0]["cwd"], str(target))
        self.assertEqual(reports[1]["cwd"], str(target))
        self.assertEqual(reports[2]["items"][0]["cwd"], str(self.directory))
        self.assertEqual(reports[3]["cwd"], str(self.directory))

    def test_directory_stack_preserves_spaces_and_roundtrips(self):
        names = ['one space', 'two\nline']
        for name in names:
            (self.directory / name).mkdir()
        commands = ['pushd --json ' + shlex.quote(str(self.directory / name)) for name in names]
        reports = self.documents('; '.join(commands + ['dirs --json', 'popd --json', 'popd --json', 'pwd --json']))
        paths = [row["path"] for row in reports[2]["items"]]
        self.assertEqual(paths, [str(self.directory / names[1]), str(self.directory / names[0]), str(self.directory)])
        self.assertEqual(reports[3]["items"][0]["path"], str(self.directory / names[0]))
        self.assertEqual(reports[4]["items"][0]["path"], str(self.directory))
        self.assertEqual(reports[5]["cwd"], str(self.directory))

    def test_failed_push_preserves_stack_and_cwd(self):
        reports = self.documents('dirs --json; pushd --json missing; dirs --json; pwd --json')
        self.assertTrue(reports[1]["complete"])
        self.assertEqual(reports[1]["status"], 1)
        self.assertTrue(reports[1]["items"][0]["error"])
        self.assertFalse(reports[1]["items"][0]["stack_changed"])
        self.assertEqual(reports[0]["items"], reports[2]["items"])
        self.assertEqual(reports[3]["cwd"], str(self.directory))

    def test_directory_modes_agree_with_bash(self):
        for name in ['a', 'b']:
            (self.directory / name).mkdir()
        program = 'pushd a >/dev/null; pushd ../b >/dev/null; pushd +1; dirs -l; popd +1; dirs -l; popd; pwd'
        reference = subprocess.run(['bash', '--noprofile', '--norc', '-c', program],
                                   cwd=self.directory, env=self.env, capture_output=True)
        actual = self.runj(program)
        self.assertEqual(actual.stdout, reference.stdout)


if __name__ == '__main__':
    unittest.main()
