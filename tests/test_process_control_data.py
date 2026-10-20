#!/usr/bin/env python3
"""Actual child execution, signals, persisted job records and sample identity."""
import json
import os
import signal
import subprocess
import unittest
from pathlib import Path
from jsonschema import ValidationError
import native_data_fixture as f
from contract_schema_fixture import projection, validate_contract

class ProcessControl(f.NativeQueryCase):
    def report(self, args, rc=0, strict=False):
        result = self.run_command(args, strict=strict, caps=['effect:read','effect:mutate','effect:exec'])
        self.assertEqual(result.returncode, rc, result.stderr)
        value = json.loads(result.stdout)
        validate_contract(args[0], value)
        return value, result

    def test_timeout_has_exact_argv_environment_and_clean_stdout(self):
        self.env['LARGE_VALUE'] = 'v' * 15000
        for strict in (False, True):
            args = ['timeout', '--json', '3', '/bin/sh', '-c', 'printf "%s|%s" "$1" "${#LARGE_VALUE}"; exit 7', 'fixture', 'one two']
            with self.subTest(strict=strict):
                report, result = self.report(args, 7, strict)
                self.assertEqual(report['exit_code'], 7)
                self.assertFalse(report['timed_out'])
                self.assertEqual(report['argv'], args[3:])
                self.assertEqual(report['child_output_channel'], 'stderr')
                self.assertIn(b'one two|15000', result.stderr)
                for key in ('where_available', 'where', 'where_preview'):
                    self.assertNotIn(key, report)
                result = self.run_command(['timeout','--data','3','/bin/sh','-c','exit 0'], strict=strict, caps=['effect:exec'])
                self.assertEqual(result.returncode, 0, result.stderr)
                stream = f.stream(result.stdout, credit_kind='none')
                self.assertEqual(stream.state, 0)
                self.scalar(stream.items[0], 'argv', 'list')
                self.scalar(stream.items[0], 'duration', 'duration')
                projected = projection(stream.items[0])
                validate_contract('timeout', projected)
                for key in ('where_available', 'where', 'where_preview'):
                    self.assertNotIn(key, projected)

                for selector in ('--json', '--data'):
                    result = self.run_command(['timeout', selector, '--diagnose', '3', '/bin/true'],
                                              strict=strict, caps=['effect:exec'])
                    self.assertEqual(result.returncode, 0, result.stderr)
                    diagnostic = (json.loads(result.stdout) if selector == '--json' else
                                  projection(f.stream(result.stdout, credit_kind='none').items[0]))
                    validate_contract('timeout', diagnostic)
                    self.assertFalse(diagnostic['timed_out'])
                    self.assertFalse(diagnostic['where_available'])
                    self.assertTrue(diagnostic['where_preview'])
                    self.assertTrue(diagnostic['where'].strip())
                    for key in ('where_available', 'where', 'where_preview'):
                        invalid = dict(diagnostic)
                        del invalid[key]
                        with self.assertRaises(ValidationError):
                            validate_contract('timeout', invalid)
                    for invalid_value in (None, ''):
                        with self.assertRaises(ValidationError):
                            validate_contract('timeout', dict(diagnostic, where=invalid_value))
                    with self.assertRaises(ValidationError):
                        validate_contract('timeout', dict(diagnostic, where_preview=False))

    def test_timeout_real_deadline_and_invalid_duration(self):
        report, result = self.report(['timeout','--json','-k','0.1','0.05','/bin/sleep','30'], 124)
        self.assertTrue(report['timed_out'])
        self.assertFalse(report['kill_incomplete'])
        self.assertLess(report['duration_ms'], 5000)
        self.assertEqual(report['child_exit_code'], 128 + signal.SIGTERM)
        for value in ['NaN','inf','1e999','-1','900000000000000000000']:
            with self.subTest(value=value):
                report, _ = self.report(['timeout','--json',value,'/bin/true'],125)
                self.assertFalse(report['complete'])

    def test_timeout_cannot_spawn_after_failed_admission(self):
        marker = self.work / 'executed'
        for selector in ('--json','--data','--data-v2'):
            request={'request_version':2,'argv':['timeout',selector,'3','/bin/sh','-c','touch "$1"','marker',str(marker)]}
            with open('/dev/full','wb') as sink:
                result=subprocess.run([str(f.JASH),'--exec-json',json.dumps(request)],env=self.env,cwd=self.work,stdout=sink,stderr=subprocess.PIPE,timeout=10)
            self.assertEqual(result.returncode,125,result.stderr)
            self.assertFalse(marker.exists())

    def test_timeout_setup_failure_prevents_child_execution(self):
        shim = self.work / 'timer.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
            str(f.ROOT / 'tests/timeout_timer_faults.c'), '-ldl', '-o', str(shim)], check=True)
        marker = self.work / 'executed'
        self.env['LD_PRELOAD'] = str(shim)
        for failure in ('get', 'handler', 'arm'):
            self.env['JASH_TEST_TIMER_FAILURE'] = failure
            for mode in ('--json', '--data'):
                with self.subTest(failure=failure, mode=mode):
                    result = self.run_command(['timeout', mode, '3', '/bin/sh', '-c',
                                               'touch "$1"', 'fixture', str(marker)])
                    self.assertEqual(result.returncode, 125, result.stderr)
                    self.assertFalse(marker.exists())
                    if mode == '--json':
                        report = json.loads(result.stdout)
                        self.assertFalse(report['started'])
                        self.assertIsNone(report['pid'])
                        self.assertIsNone(report['child_exit_code'])
                    else:
                        stream = f.stream(result.stdout, credit_kind='none')
                        self.assertEqual((stream.state, stream.producer), (2, 125))
                        self.scalar(stream.items[0], 'started', 'bool', False)

    def test_kill_receipts_match_actual_signal_and_preview(self):
        child=subprocess.Popen(['/bin/sleep','60'],start_new_session=True)
        self.addCleanup(lambda: child.poll() is None and (child.terminate(), child.wait(timeout=5)))
        report,_=self.report(['kill','--json','--dry-run',str(child.pid)])
        self.assertTrue(report['items'][0]['preview'])
        self.assertFalse(report['items'][0]['sent'])
        self.assertIsNone(child.poll())
        result=self.run_command(['kill','--data','-TERM',str(child.pid)],caps=['effect:exec'])
        self.assertEqual(result.returncode,0,result.stderr)
        stream=f.stream(result.stdout,credit_kind='none')
        self.scalar(stream.items[0],'sent','bool',True)
        self.assertEqual(child.wait(timeout=5),-signal.SIGTERM)

    def test_kill_failed_admission_has_no_effect(self):
        child=subprocess.Popen(['/bin/sleep','60'],start_new_session=True)
        self.addCleanup(lambda: child.poll() is None and (child.terminate(),child.wait(timeout=5)))
        for flag in ('--json','--data'):
            with open('/dev/full','wb') as sink:
                result=subprocess.run([str(f.JASH),'--exec-json',json.dumps({'request_version':2,'argv':['kill',flag,str(child.pid)]})],env=self.env,stdout=sink,stderr=subprocess.PIPE,timeout=10)
            self.assertEqual(result.returncode,125,result.stderr)
            self.assertIsNone(child.poll())

    def test_job_records_decode_exactly_and_reject_corruption(self):
        directory=Path(self.env['HOME'])/'.jash/jobs';directory.mkdir(parents=True)
        source={'id':12,'state':'done','pid':4321,'rc':7,'ms':98,'start_ts':123,'end_ts':124,'notified':False,'cmd':'space " and slash \\ and newline\n'}
        (directory/'12.json').write_text(json.dumps(source))
        report,_=self.report(['jobs','--json'])
        self.assertEqual(report[0]['cmd'],source['cmd'])
        self.assertEqual((report[0]['id'],report[0]['rc']),(12,7))
        result=self.run_command(['jobs','--data'],caps=['effect:mutate'])
        self.assertEqual(result.returncode,0,result.stderr)
        stream=f.stream(result.stdout)
        self.scalar(stream.items[0],'start','timestamp',123_000_000_000)
        (directory/'12.json').write_text('{"id":12,"state":"done","cmd":"truncated')
        report,_=self.report(['jobs','--json'],1)
        self.assertFalse(report['complete'])

    def test_breathe_sample_is_private_and_identity_bound(self):
        report,_=self.report(['breathe','--json','check','--interval','0'])
        self.assertTrue(report['baseline'])
        self.assertTrue(report['applied'])
        self.assertTrue(report['durable'])
        sample=Path(self.env['HOME'])/'.jash/breathe_sample.tsv'
        self.assertEqual(sample.stat().st_mode&0o777,0o600)
        lines=sample.read_text().splitlines()
        self.assertTrue(lines[0].startswith('# jash-breathe-v2 '))
        self.assertTrue(all(len(x.split())==3 for x in lines[1:]))
        # PID equality without matching start ticks must never imply a frozen process.
        altered=[lines[0]]+[' '.join(x.split()[:2]+['1']) for x in lines[1:]]
        sample.write_text('\n'.join(altered)+'\n')
        report,_=self.report(['breathe','--json','check','--interval','0'])
        self.assertEqual(report['stuck'],0)
        self.assertEqual(report['identity'],'pid-and-start-ticks')

    def test_breathe_failed_admission_preserves_sample(self):
        sample=Path(self.env['HOME'])/'.jash/breathe_sample.tsv'
        for args in (['breathe','--json','check'], ['breathe','--data','check'],
                     ['breathe','check','--json']):
            with open('/dev/full','wb') as sink:
                result=subprocess.run([str(f.JASH),'--exec-json',json.dumps({'request_version':2,'argv':args})],env=self.env,cwd=self.work,stdout=sink,stderr=subprocess.PIPE,timeout=10)
            self.assertEqual(result.returncode,125,result.stderr)
            self.assertFalse(sample.exists())

    def test_breathe_trailing_json_retains_legacy_shapes_and_identity(self):
        sample = Path(self.env['HOME']) / '.jash/breathe_sample.tsv'
        process_keys = {'pid','ppid','uid','user','comm','state','rss_kb','vsz_kb','cpu_ticks'}
        for strict in (False, True):
            with self.subTest(strict=strict):
                result = self.run_command(['breathe','status','--json'], strict=strict)
                self.assertEqual(result.returncode, 0, result.stderr)
                legacy = json.loads(result.stdout)
                self.assertEqual(set(legacy), {'count','procs'})
                self.assertEqual(legacy['count'], len(legacy['procs']))
                self.assertTrue(all(set(row) == process_keys for row in legacy['procs']))
                canonical, _ = self.report(['breathe','--json','status'], strict=strict)
                self.assertEqual(canonical['schema'], 'jash.breathe.v1')
                self.assertTrue(all('start_ticks' in row for row in canonical['procs']))

                sample.unlink(missing_ok=True)
                args = ['breathe','check','--json']
                result = self.run_command(args, strict=strict, caps=['effect:mutate'])
                self.assertEqual(result.returncode, 0, result.stderr)
                baseline = json.loads(result.stdout)
                self.assertEqual(set(baseline), {'baseline','tracked','stuck','procs'})
                self.assertEqual((baseline['baseline'],baseline['stuck'],baseline['procs']), (True,0,[]))
                before = sample.read_bytes()
                result = self.run_command(args + ['--interval','2147483647'], strict=strict, caps=['effect:mutate'])
                self.assertEqual(result.returncode, 0, result.stderr)
                too_soon = json.loads(result.stdout)
                self.assertEqual(set(too_soon), {'baseline','too_soon','elapsed_s','interval_s','stuck','procs'})
                self.assertEqual((too_soon['baseline'],too_soon['too_soon'],too_soon['stuck']), (False,True,0))
                self.assertEqual(sample.read_bytes(), before)

                # A historical shape must still use the new identity-bound
                # engine. PID matches with deliberately wrong start counters
                # are a negative control, never evidence of a frozen process.
                lines = before.decode().splitlines()
                altered = [lines[0]] + [' '.join(line.split()[:2] + ['1']) for line in lines[1:]]
                sample.write_text('\n'.join(altered) + '\n')
                result = self.run_command(args + ['--interval','0'], strict=strict, caps=['effect:mutate'])
                self.assertEqual(result.returncode, 0, result.stderr)
                checked = json.loads(result.stdout)
                self.assertEqual(set(checked), {'baseline','elapsed_s','interval_s','threshold_ticks','tracked','stuck','procs'})
                self.assertEqual((checked['baseline'],checked['stuck'],checked['procs']), (False,0,[]))

    def test_breathe_legacy_kill_has_checked_admission_and_exact_receipt(self):
        for strict in (False, True):
            with self.subTest(strict=strict):
                refused = self.run_command(['breathe','kill','1','--json'], strict=strict, caps=['effect:exec'])
                self.assertEqual(refused.returncode, 1, refused.stderr)
                self.assertEqual(json.loads(refused.stdout), {'pid':1,'ok':False,'error':'refusing to signal pid <= 1'})
                child = subprocess.Popen(['/bin/sleep','60'], start_new_session=True)
                self.addCleanup(lambda child=child: child.poll() is None and (child.terminate(),child.wait(timeout=5)))
                args = ['breathe','kill',str(child.pid),'--json']
                if strict:
                    denied = self.run_command(args, strict=True, caps=['effect:mutate'])
                    self.assertEqual(denied.returncode, 125, denied.stderr)
                    self.assertIn(b'capability denied', denied.stderr)
                    self.assertIsNone(child.poll())
                if not strict:
                    # Direct builtin sink admission is exercised in general.
                    # agentd uses protocol chunks: redirecting its transport
                    # stdout is not the same as refusing the builtin writer.
                    with open('/dev/full','wb') as sink:
                        failed = subprocess.run([str(f.JASH),'--exec-json',json.dumps({'request_version':2,'argv':args})],
                            env=self.env,cwd=self.work,stdout=sink,stderr=subprocess.PIPE,timeout=10)
                    self.assertEqual(failed.returncode, 125, failed.stderr)
                    self.assertIsNone(child.poll())
                result = self.run_command(args, strict=strict, caps=['effect:exec'])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, ('{"pid":%d,"signal":"TERM","ok":true}\n' % child.pid).encode())
                self.assertEqual(child.wait(timeout=5), -signal.SIGTERM)

    def test_detached_job_fifo_is_rejected_without_waiting_for_a_writer(self):
        directory = Path(self.env['HOME']) / '.jash/jobs'
        directory.mkdir(parents=True, exist_ok=True)
        os.mkfifo(directory / '123.json')
        self.report(['jobs', '--json'], 1)

if __name__=='__main__':
    unittest.main()
