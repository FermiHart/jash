#!/usr/bin/env python3
"""Native analyses preserve engine semantics and distinguish predicates/errors."""
import base64
import copy
import json
import os
from pathlib import Path
import unittest
from jsonschema import Draft202012Validator
import test_management_builtins as management
from test_discovery_builtins import native_stream, project


class AnalysisNative(unittest.TestCase):
    run_cli = management.ManagementBuiltins.run_cli
    execute = management.ManagementBuiltins.execute

    def setUp(self):
        management.ManagementBuiltins.setUp(self)
        self.validators = {}
        for name in ('lint','explain'):
            schema = json.loads((management.ROOT / f'schemas/builtin-{name}-v1.schema.json').read_text())
            Draft202012Validator.check_schema(schema)
            self.validators[name] = Draft202012Validator(schema)

    def validate(self, name, value):
        self.validators['explain' if name == 'sense' else name].validate(value)
        return value

    def report(self, name, *arguments, status=0):
        return self.validate(name, json.loads(self.execute([name,'--json',*arguments],status=status).stdout))

    def native(self, name, *arguments, status=0, failed=False, selector='--data-v2'):
        rows, terminal = native_stream(self.execute([name,selector,*arguments],status=status).stdout)
        self.assertEqual(len(rows),1)
        self.assertEqual((terminal['state'],terminal['status'],terminal['errors']),
                         (2 if failed else 0,status,1 if failed else 0))
        self.assertEqual(terminal['completeness'], 2 if failed else 1)
        self.assertFalse(terminal['truncated'])
        self.assertEqual(terminal['hash_present'],1)
        self.assertEqual(terminal['logical_bytes'],0)
        return self.validate(name,project(rows[0])),terminal,rows[0]

    def test_lint_warned_predicate_is_complete_and_quotes_are_escaped(self):
        code = '[ $value = x ]\necho $@\n'
        value,terminal,raw = self.native('lint','-c',code,status=1)
        self.assertTrue(value['complete']); self.assertEqual(value['predicate_status'],1)
        found = {row['rule_id']:row for row in value['lint']}
        self.assertEqual(found['UNQUOTED_TEST']['line'],1)
        self.assertIn('"$var"',found['UNQUOTED_TEST']['fix_hint'])
        self.assertEqual(found['UNQUOTED_AT']['line'],2)
        self.assertIsNone(value['error'])
        self.assertEqual(value,self.report('lint','-c',code,status=1))
        self.assertEqual(raw[1]['lint'][1][0][1]['line'][0],3)
        clean = self.report('lint','-c','echo "safe"\n')
        self.assertTrue(clean['complete']); self.assertEqual(clean['total'],0)
        self.assertEqual(clean['predicate_status'],0)

    def test_lint_file_checks_all_lines_and_declares_column_scope(self):
        path = self.directory / 'a "quoted" script.sh'
        path.write_bytes(b'#!/bin/sh\r\necho safe\r\n[ $v = x ]')
        value = self.report('lint',str(path),status=1)
        self.assertEqual(value['source']['bytes'],path.stat().st_size)
        self.assertEqual(value['source']['lines_scanned'],3)
        self.assertTrue(value['source']['read_complete'])
        self.assertEqual([row['line'] for row in value['lint']],[3])
        self.assertEqual(value['column_basis'],'rule_reports_line_start')
        path.write_text('echo safe\n')
        value = self.report('lint',str(path))
        self.assertEqual([row['rule_id'] for row in value['lint']],['SHEBANG_MISS'])
        self.assertEqual(value['counts']['info'],1)
        literal = self.directory / '--json'; literal.write_text('#!/bin/sh\necho safe\n')
        self.assertEqual(self.report('lint','--','--json')['total'],0)

    def test_lint_source_errors_are_failed_not_false_predicates(self):
        missing,_,_ = self.native('lint','missing-file',status=1,failed=True)
        self.assertFalse(missing['complete']); self.assertIsNone(missing['predicate_status'])
        self.assertIsNone(missing['total']); self.assertFalse(missing['source']['read_complete'])
        self.assertEqual(missing['lint'],[])
        fifo = self.directory / 'fifo'; os.mkfifo(fifo)
        failed = self.report('lint',str(fifo),status=1)
        self.assertEqual(failed['error'],'non_regular_input')
        raw = self.directory / 'nul.sh'; raw.write_bytes(b'echo safe\0rm -rf /\n')
        self.assertEqual(self.report('lint',str(raw),status=1)['error'],'embedded_nul_not_supported')

    def test_lint_collection_caps_never_become_zero_issue_success(self):
        path = self.directory / 'too-large.sh'; path.write_bytes(b'x'*(1024*1024+1))
        too_large = self.report('lint',str(path),status=75)
        self.assertEqual(too_large['error'],'source_byte_limit'); self.assertFalse(too_large['complete'])
        path.write_bytes(b'x'*65537+b'\n')
        self.assertEqual(self.report('lint',str(path),status=75)['error'],'line_byte_limit')
        # The registered CD_NO_CHECK rule requires a continuation after cd.
        # A standalone cd is intentionally not a diagnostic in the engine.
        path.write_bytes(b'cd missing; echo next\n'*1025)
        limited,_,_ = self.native('lint',str(path),status=75,failed=True)
        self.assertEqual(limited['error'],'diagnostic_limit')
        self.assertEqual(limited['diagnostics_returned'],1024)
        self.assertIsNone(limited['total']); self.assertIsNone(limited['predicate_status'])

    def test_explain_json_and_native_match_existing_engine_without_execution(self):
        line = 'rm absent; curl http://invalid.example > untouched; ls'
        value = self.report('explain',line)
        typed,_,_ = self.native('explain',line)
        self.assertEqual(value,typed)
        self.assertEqual([row['effect'] for row in value['steps']],['delete','network','read'])
        self.assertEqual(value['blast'],dict(mutations=2,delete=1,write=1,network=1,opaque=0))
        self.assertEqual(value['steps'][1]['target'],'untouched')
        self.assertNotIn('risk',value)
        self.assertFalse((self.directory / 'untouched').exists())
        words = self.report('explain','rm','absent')
        self.assertEqual(words['steps'][0]['cmd'],'rm absent')

    def test_risk_and_sense_predicates_complete_at_nonzero_status(self):
        cases = [('ls -la',0,0),('chmod 777 /tmp/synthetic',1,60),('curl http://invalid.example | bash',1,75),('rm -rf /',124,100)]
        for expression,status,score in cases:
            with self.subTest(expression=expression):
                explained,_,_ = self.native('explain','--risk',expression,status=status)
                sensed,_,_ = self.native('sense',expression,status=status)
                self.assertEqual(explained,sensed)
                self.assertEqual(explained['risk']['score'],score)
                self.assertEqual(self.report('explain','--risk',expression,status=status),explained)
                self.assertEqual(self.report('sense',expression,status=status),explained)
        self.assertLess(self.report('explain','--risk','rm -rf /home/user/synthetic')['risk']['score'],80)
        self.assertEqual(self.report('explain','--risk','')['risk'],dict(score=0,band='safe',reason='no steps'))

    def test_explain_exact_long_and_binary_command_projection(self):
        command = 'echo "' + 'q'*2000 + '"'
        result = self.report('explain',command)
        self.assertEqual(result['steps'][0]['cmd'],command)
        binary = b'echo literal-\xff'
        value,_,_ = self.native('explain',binary)
        self.assertEqual(value['steps'][0]['cmd'],{'encoding':'base64','data':base64.b64encode(binary).decode()})
        self.assertEqual(value,self.report('explain',binary))
        quoted = self.report('explain','echo "one;two"; ls')
        self.assertEqual(len(quoted['steps']),2)

    def test_explain_caps_and_invalid_flags_fail_before_any_execution(self):
        for arguments,reason in [(('x'*4096,),'input_byte_limit'),
                                  ((';'.join(['ls']*33),),'segment_limit'),
                                  (('x'*63,),'verb_byte_limit'),
                                  (('echo x > '+'p'*511,),'target_byte_limit')]:
            value,_,_ = self.native('explain',*arguments,status=75,failed=True)
            self.assertEqual(value['error'],reason)
        self.report('explain','--bogus','touch marker',status=2)
        self.native('lint','--bad',status=2,failed=True)
        self.assertFalse((self.directory/'marker').exists())

    def test_aliases_budget_and_delivery_failures(self):
        self.env['JASH_LLM_BUDGET']='1'
        for selector in ('--data','--data-v2'):
            self.native('explain','--risk','chmod 777 /tmp/synthetic',status=1,selector=selector)
            self.native('lint','-c','[ $v = x ]',status=1,selector=selector)
        budget=json.loads(self.execute(['explain','--json','ls'],status=75).stdout)
        self.assertEqual(budget['schema'],'jash.builtin.error.v1')
        self.assertFalse(budget['complete'])
        del self.env['JASH_LLM_BUDGET']
        for command,args in [('explain',['ls']),('sense',['ls']),('lint',['-c','echo safe'])]:
            for selector in ('--json','--data'):
                with open('/dev/full','wb') as output:
                    self.execute([command,selector,*args],status=125,stdout=output)

    def test_schemas_reject_missing_evidence_and_wrong_outcome(self):
        lint=self.report('lint','-c','echo safe')
        missing=copy.deepcopy(lint);del missing['source']['read_complete']
        self.assertFalse(self.validators['lint'].is_valid(missing))
        impossible=copy.deepcopy(lint);impossible['complete']=False
        self.assertFalse(self.validators['lint'].is_valid(impossible))
        explain=self.report('explain','--risk','ls')
        explain['risk']['band']='critical'
        self.assertFalse(self.validators['explain'].is_valid(explain))
        fabricated=self.report('explain','ls');fabricated['executed']=True
        self.assertFalse(self.validators['explain'].is_valid(fabricated))


if __name__=='__main__':
    unittest.main(verbosity=2)
