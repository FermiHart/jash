#!/usr/bin/env python3
"""Graph native observations versus real C sources, stores and effects."""
import base64
import copy
import fcntl
import json
import os
from pathlib import Path
import subprocess
import time
from jsonschema import ValidationError
import native_data_fixture as f
from contract_schema_fixture import projection, validate_contract


class GraphNative(f.NativeQueryCase):
    def report(self, *arguments, mode='--json', status=0, healthy=None, strict=False):
        result = self.run_command(['graph', mode, *arguments], strict=strict,
                                  caps=['effect:read', 'effect:mutate'])
        self.assertEqual(result.returncode, status, result.stderr)
        if mode == '--json': value = json.loads(result.stdout)
        else:
            stream = f.stream(result.stdout, credit_kind='none')
            expected = 0 if (status == 0 if healthy is None else healthy) else 2
            self.assertEqual((stream.producer, stream.state), (status, expected))
            self.assertEqual(len(stream.items), 1)
            value = projection(stream.items[0])
        validate_contract('graph', value)
        return value

    def seed(self):
        source = {
            'a.c': '#include "util.h"\nint main(void) { return foo(); }\nstatic int dup(void) { return 1; }\n',
            'util.c': '#include "util.h"\n#include "b.h"\nint foo(void) { return bar() + dup(); }\nstatic int dup(void) { return 2; }\n',
            'b.c': '#include "b.h"\nint bar(void) { return dup(); }\nstatic int dup(void) { return 3; }\n',
            'util.h': 'struct point { int x; int y; };\ntypedef unsigned long my_size;\nenum color { RED, GREEN };\nint foo(void);\n',
            'b.h': 'int bar(void);\n'}
        for path, content in source.items(): (self.work / path).write_text(content)

    @property
    def store(self): return Path(self.env['HOME']) / '.jash/graph.tsv'

    def test_build_counts_come_from_parser_and_survive_process_boundary(self):
        self.seed(); result = self.report('build', mode='--data')
        self.assertTrue(result['publication']['applied']); self.assertTrue(result['publication']['durable'])
        self.assertEqual((result['result']['counts']['files'], result['result']['counts']['nodes'], result['result']['counts']['edges']), (5,14,17))
        self.assertEqual(self.store.stat().st_mode & 0o777, 0o600)
        self.assertTrue(self.store.read_bytes().endswith(b'\n'))
        counts = self.report('stats', strict=True)['result']
        self.assertEqual(counts, result['result']['counts'])
        self.assertEqual(counts['extracted'] + counts['inferred'], counts['edges'])
        for mode in ('--data', '--data-v2'):
            self.assertEqual(self.report('stats', mode=mode, strict=True)['result'], counts)

    def test_explain_path_ranking_and_communities_use_actual_adjacency(self):
        self.seed(); self.report('build')
        explanation = self.report('explain', 'foo', mode='--data')['result']
        edges = {(edge['to']['name'], edge['relation'], edge['direction'], edge['confidence']) for edge in explanation['edges']}
        self.assertIn(('bar','calls','forward','EXTRACTED'), edges)
        self.assertIn(('main','calls','reverse','EXTRACTED'), edges)
        path = self.report('path','main','bar',mode='--data-v2')['result']
        self.assertTrue(path['found']); self.assertEqual([hop['to']['name'] for hop in path['hops']], ['foo','bar'])
        ranked = self.report('god-nodes','--top','3')['result']
        self.assertEqual(ranked['nodes'][0]['name'], 'util.h'); self.assertTrue(ranked['has_more'])
        communities = self.report('communities','--top','20',mode='--data')['result']
        self.assertEqual(sum(c['size'] for c in communities['communities']),14)

    def test_complete_false_queries_have_healthy_nonzero_terminal(self):
        self.seed(); self.report('build')
        missing = self.report('explain','no-such-node',mode='--data',status=1,healthy=True)
        self.assertTrue(missing['complete']); self.assertFalse(missing['result']['found'])
        missing = self.report('path','no-such-node','foo',mode='--data-v2',status=1,healthy=True)
        self.assertTrue(missing['complete']); self.assertEqual(missing['result']['hops'],[])

    def test_incremental_and_deletion_report_actual_reuse_without_republishing(self):
        self.seed(); self.report('build'); before = self.store.stat().st_mtime_ns
        same = self.report('build',mode='--data')['result']
        self.assertEqual((same['reused'],same['reparsed'],same['gone']),(5,0,0))
        self.assertEqual(self.store.stat().st_mtime_ns,before)
        with (self.work/'util.c').open('a') as target: target.write('int baz(void) { return bar(); }\n')
        changed = self.report('build')['result']; self.assertEqual((changed['reused'],changed['reparsed']),(4,1))
        (self.work/'b.c').unlink()
        self.assertEqual(self.report('build')['result']['gone'],1)
        self.report('explain','bar',mode='--data',status=1,healthy=True)

    def test_literal_long_and_non_utf8_paths_are_preserved(self):
        name = b'file with " dollar$ ' + b'x'*140 + b'\xff.c'
        fd = os.open(os.fsencode(self.work)+b'/'+name,os.O_WRONLY|os.O_CREAT,0o600)
        os.write(fd,b'int exact(void) { return 1; }\n'); os.close(fd)
        self.report('build')
        for mode in ('--json','--data'):
            node = self.report('explain',name,mode=mode)['result']['node']
            self.assertEqual(base64.b64decode(node['file']['data']),name)
            self.assertEqual(base64.b64decode(node['name']['data']),name)

    def test_source_limits_and_unrepresentable_paths_do_not_publish(self):
        self.seed(); (self.work/'loop').symlink_to('.',target_is_directory=True)
        result = self.report('build'); self.assertGreaterEqual(result['result']['excluded'],1)
        original = self.store.read_bytes()
        with (self.work/'huge.c').open('wb') as target: target.truncate(1048577)
        self.report('build',mode='--data',status=1); self.assertEqual(self.store.read_bytes(),original)
        (self.work/'huge.c').unlink(); (self.work/'newline\n.c').write_text('int z(void) { return 1; }\n')
        self.report('build',status=1); self.assertEqual(self.store.read_bytes(),original)

    def test_missing_corrupt_and_fifo_sources_are_not_empty_success(self):
        self.report('stats',mode='--data',status=1); self.assertFalse(self.store.exists())
        self.seed(); self.report('build'); original = self.store.read_bytes()
        for corrupt in (b'N\tx\tF\tmissing.c\t1\t0\n',b'F\ta.c\t0\tbad\n',original+b'E\t999999\t0\tC\tE\n',original[:-1]):
            self.store.write_bytes(corrupt); self.report('stats',mode='--data',status=1)
        self.store.unlink(); os.mkfifo(self.store); self.report('stats',status=1)

    def test_push_and_sync_publish_real_union_and_adopt_missing_local(self):
        self.seed(); self.report('build')
        mesh = self.store.parent/'mesh'; mesh.mkdir(mode=0o700)
        pushed = self.report('push',mode='--data'); self.assertTrue(pushed['publication']['applied'])
        self.assertEqual((mesh/'graph.tsv').read_bytes(),self.store.read_bytes())
        self.store.unlink()
        synced = self.report('sync',mode='--data-v2'); self.assertTrue(synced['publication']['applied'])
        self.assertEqual(synced['result']['nodes_added_from_fleet'],14)
        self.assertEqual(self.report('stats')['result']['nodes'],14)
        noop = self.report('sync'); self.assertFalse(noop['publication']['attempted'])

    def test_hot_uses_complete_recorded_commands_and_checked_gap_source(self):
        self.seed(); self.report('build')
        commands = [(0,'echo a.c'),(1,'cat '+('padding '*500)+'util.c'),(1,'cat util.c a.c')]
        chain = ''.join(f'{i+1} 1700000000.000000001 '+ '0'*64 + f' tag=- rc={rc} cmd={command}\n' for i,(rc,command) in enumerate(commands))
        (self.store.parent/'chain.log').write_text(chain)
        (self.store.parent/'gaps.tsv').write_text('a'*16+'\topen\t2\t1700000000\t1700000001\tread util.c\tshould work\ttrue\n')
        hot = self.report('hot',mode='--data')['result']
        self.assertEqual(hot['chain_records'],3); self.assertFalse(hot['cryptographically_verified'])
        self.assertEqual(hot['files'][0]['file'],'util.c'); self.assertEqual(hot['files'][0]['pain'],8)
        self.assertTrue(hot['files'][0]['gap_match'])
        (self.store.parent/'gaps.tsv').write_text('invalid\n'); self.report('hot',status=1)

    def test_sink_and_presentation_budget_admission_precede_store_creation(self):
        self.seed()
        for mode in ('--json','--data','--data-v2'):
            with open('/dev/full','wb') as output:
                result = subprocess.run([str(f.JASH),'--exec-json',json.dumps({'request_version':2,'argv':['graph',mode,'build']})],
                    cwd=self.work,env=self.env,stdout=output,stderr=subprocess.PIPE,timeout=20)
            self.assertEqual(result.returncode,125,result.stderr); self.assertFalse(self.store.exists())
        self.env['JASH_LLM_BUDGET']='1'
        result=self.run_command(['graph','--json','build']); self.assertEqual(result.returncode,75); self.assertFalse(self.store.exists())
        self.report('build',mode='--data'); self.assertTrue(self.store.exists())
        self.env.pop('JASH_LLM_BUDGET'); self.report('stats','--budget','1',status=75)

    def test_publication_faults_distinguish_rename_from_durability(self):
        self.seed(); self.report('build'); original=self.store.read_bytes()
        with (self.work/'a.c').open('a') as target: target.write('int new_node(void) { return 3; }\n')
        shim=self.work/'fault.so'
        subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror',str(f.ROOT/'tests/graph_io_faults.c'),'-ldl','-o',str(shim)],check=True)
        self.env['LD_PRELOAD']=str(shim)
        for fault in ('rename','directory-fsync'):
            self.env['JASH_TEST_GRAPH_FAULT']=fault
            result=self.report('build',mode='--data',status=1)
            self.assertEqual(result['publication']['applied'],fault=='directory-fsync')
            self.assertFalse(result['publication']['durable'])
            if fault=='rename': self.assertEqual(self.store.read_bytes(),original)
            else: self.assertNotEqual(self.store.read_bytes(),original)
        self.assertEqual(list(self.store.parent.glob('.graph-*.tmp')),[])

    def test_writer_lock_has_deadline_and_does_not_publish(self):
        self.seed(); self.report('build'); original=self.store.read_bytes()
        with (self.work/'a.c').open('a') as target: target.write('int extra(void) { return 5; }\n')
        with (self.store.parent/'graph.lock').open('r+b') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB); started=time.monotonic()
            result=self.report('build',mode='--data',status=1)
            self.assertGreaterEqual(time.monotonic()-started,4.8)
        self.assertEqual(result['error'],'graph_lock_failed'); self.assertFalse(result['publication']['applied'])
        self.assertEqual(self.store.read_bytes(),original)

    def test_schema_negatives_and_invalid_arguments(self):
        result=self.report('help',mode='--data')
        missing=copy.deepcopy(result); del missing['result']['node_limit']
        with self.assertRaises(ValidationError): validate_contract('graph',missing)
        extra=copy.deepcopy(result); extra['invented']=1
        with self.assertRaises(ValidationError): validate_contract('graph',extra)
        for arguments in [('build','a','b'),('path','one'),('stats','--top','0'),('invented',)]:
            self.report(*arguments,mode='--data',status=2)


if __name__=='__main__': f.unittest.main()
