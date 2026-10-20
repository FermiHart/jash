#!/usr/bin/env python3
"""Grep count/line/predicate modes share observed source records across encoders."""
import base64
import copy
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import time
import unittest

from jsonschema.exceptions import ValidationError
import native_data_fixture as f
from contract_schema_fixture import projection, validate_contract

FAULT_SOURCE = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int mode(const char* expected) { const char* value=getenv("GREP_TEST_FAULT"); return value&&!strcmp(value,expected); }
static int target(int fd) {
    char link[64],path[PATH_MAX+1]; const char* name=getenv("GREP_TEST_FILE");
    if(!name) return 0;
    snprintf(link,sizeof link,"/proc/self/fd/%d",fd);
    ssize_t n=readlink(link,path,PATH_MAX); if(n<0) return 0; path[n]=0; return !strcmp(path,name);
}
ssize_t read(int fd,void* buffer,size_t count) {
    static ssize_t (*next)(int,void*,size_t); if(!next) *(void**)&next=dlsym(RTLD_NEXT,"read");
    if(target(fd)&&mode("since-hold")) {
        static int waiting;
        const char* ready=getenv("GREP_TEST_READY"); const char* release=getenv("GREP_TEST_RELEASE");
        if(!waiting++&&ready&&release) {
            char byte='1'; (void)write(atoi(ready),&byte,1);
            while(next(atoi(release),&byte,1)<0&&errno==EINTR) {}
        }
    }
    if(target(fd)&&mode("read")) { errno=EIO; return -1; }
    if(target(fd)&&mode("change")) {
        static int changed; if(!changed++) { int output=open(getenv("GREP_TEST_FILE"),O_WRONLY|O_APPEND); if(output>=0) { (void)write(output,"new\n",4); close(output); } }
    }
    static int interrupted;
    if(target(fd)&&mode("short")&&!interrupted++) { errno=EINTR; return -1; }
    if(target(fd)&&mode("short")&&count>3) count=3;
    return next(fd,buffer,count);
}
int close(int fd) {
    static int (*next)(int); if(!next) *(void**)&next=dlsym(RTLD_NEXT,"close");
    int failed=target(fd)&&mode("close"); int result=next(fd);
    if(failed) { errno=EIO; return -1; } return result;
}
struct dirent* readdir(DIR* directory) {
    static struct dirent* (*next)(DIR*); if(!next) *(void**)&next=dlsym(RTLD_NEXT,"readdir");
    if(target(dirfd(directory))&&mode("directory")) { errno=EIO; return NULL; }
    return next(directory);
}
int poll(struct pollfd* fds,nfds_t count,int timeout) {
    static int (*next)(struct pollfd*,nfds_t,int); if(!next) *(void**)&next=dlsym(RTLD_NEXT,"poll");
    if(mode("stdin-hold")&&count==1&&fds[0].fd==0&&(fds[0].events&POLLIN)) {
        static int held=-1;
        if(held<0) {
            /* Hold a real pipe writer open: input is paused without EOF.
             * The producer must leave poll itself to observe the cancel FD. */
            held=open("/proc/self/fd/0",O_WRONLY|O_NONBLOCK|O_CLOEXEC);
            const char* ready=getenv("GREP_TEST_READY");
            if(held>=0&&ready) (void)write(atoi(ready),"1",1);
        }
    }
    return next(fds,count,timeout);
}
'''


def raw(value):
    if isinstance(value, str): return value.encode()
    return base64.b64decode(value['data'], validate=True)


class GrepSemantic(f.NativeQueryCase):
    @classmethod
    def setUpClass(cls):
        temporary=tempfile.TemporaryDirectory(prefix='jash-grep-semantic-faults-')
        cls.addClassCleanup(temporary.cleanup)
        source=Path(temporary.name)/'faults.c';source.write_text(FAULT_SOURCE)
        cls.hook=Path(temporary.name)/'faults.so'
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-shared','-fPIC',str(source),'-ldl','-o',str(cls.hook)],check=True,capture_output=True)

    def report(self,*arguments,mode='--data-v2',rc=0,strict=False,stdin=None,validate=True):
        result=self.run_command(['grep',mode,*arguments],strict=strict,stdin=stdin)
        self.assertEqual(result.returncode,rc,result.stderr+result.stdout[:512])
        if mode=='--json':
            document=json.loads(result.stdout);rows=document['items'];summary=document['summary'];terminal=None
            if validate:validate_contract('grep-query',document)
        else:
            terminal=f.stream(result.stdout)
            self.assertEqual(terminal.producer,rc)
            for row in terminal.items:
                self.scalar(row,'record','symbol')
                if row.value['record'].value=='summary':
                    self.scalar(row,'exit_code','i64',rc)
                    for field in ('sources_observed','source_failures','source_bytes','selected_lines_observed',
                                  'records_total','records_returned','offset','context_before','context_after','fuzzy_work'):
                        self.scalar(row,field,'u64')
                    for field in ('predicate','complete','scan_complete','inverted','any_selected_line'):
                        self.scalar(row,field,'bool')
                else:
                    self.scalar(row,'source','symbol')
                    self.scalar(row,'file','path' if row.value['source'].value=='file' else 'nothing')
                    if row.value['record'].value in ('line','span'):
                        self.scalar(row,'content','bytes');self.scalar(row,'line','u64')
                    elif row.value['record'].value=='count':self.scalar(row,'count','u64')
            decoded=[projection(row) for row in terminal.items]
            self.assertTrue(decoded);rows=decoded[:-1];summary=decoded[-1]
            if validate:
                for row in decoded:validate_contract('grep-query-row',row)
        self.assertEqual(summary['schema'],'jash.grep.query.v1')
        self.assertEqual(summary['record'],'summary')
        self.assertEqual(summary['exit_code'],rc)
        self.assertEqual(summary['records_returned'],len(rows))
        return rows,summary,terminal

    def write(self,name,data):
        path=self.work/name;path.write_bytes(data);return path

    def test_count_inversion_and_combination_use_line_counts(self):
        self.write('input',b'red red\nblue\nRed\n')
        for mode in ('--json','--data','--data-v2'):
            for strict in (False,True):
                rows,summary,terminal=self.report('-cio','red','input',mode=mode,strict=strict)
                self.assertEqual([(row['record'],row['count'],row['complete']) for row in rows],[('count',2,True)])
                self.assertEqual(summary['selected_lines_observed'],2)
                self.assertEqual(summary['mode'],'count')
                if terminal:self.assertEqual((terminal.state,terminal.errors),(0,0))
                inverse,_,_=self.report('-vci','red','input',mode=mode,strict=strict)
                self.assertEqual(inverse[0]['count'],1)
        rows,summary,terminal=self.report('-c','absent','input',rc=1)
        self.assertEqual(rows[0]['count'],0)
        self.assertFalse(summary['predicate']);self.assertEqual((terminal.state,terminal.errors),(0,0))

    def test_files_with_and_without_matches_keep_real_predicates(self):
        self.write('hit',b'red\nmore\n');self.write('miss',b'blue\n');self.write('empty',b'')
        for mode in ('--json','--data-v2'):
            rows,summary,_=self.report('-l','red','hit','miss','empty',mode=mode)
            self.assertEqual([row['file'] for row in rows],['hit'])
            self.assertTrue(rows[0]['matched']);self.assertFalse(rows[0]['scanned_to_end'])
            self.assertTrue(summary['complete']);self.assertFalse(summary['scan_complete'])
            rows,summary,_=self.report('--files-without-matches','red','hit','miss','empty',mode=mode)
            self.assertEqual([row['file'] for row in rows],['miss','empty'])
            self.assertTrue(all(not row['matched'] for row in rows))
            self.assertTrue(summary['predicate'])
        rows,_,terminal=self.report('-L','red','hit',rc=1)
        self.assertEqual(rows,[]);self.assertEqual((terminal.state,terminal.errors),(0,0))

    def test_quiet_exists_and_false_are_complete_without_scan_claim(self):
        self.write('hit',b'red\nmore\n');self.write('miss',b'blue\n')
        for flags in ('-q','-qclL','--quiet','--silent'):
            rows,summary,terminal=self.report(flags,'red','hit','missing',strict=True)
            self.assertEqual(rows,[]);self.assertTrue(summary['predicate']);self.assertFalse(summary['scan_complete'])
            self.assertEqual((terminal.state,terminal.errors),(0,0))
            self.assertEqual(summary['mode'],'quiet')
        rows,summary,terminal=self.report('-q','red','miss',rc=1)
        self.assertEqual(rows,[]);self.assertFalse(summary['predicate']);self.assertTrue(summary['scan_complete'])
        self.assertEqual((terminal.state,terminal.errors),(0,0))
        rows,summary,terminal=self.report('-q','red','missing','hit',rc=2)
        self.assertTrue(summary['predicate']);self.assertEqual(rows[0]['error'],'stat_failed')
        self.assertEqual((terminal.state,terminal.errors),(2,1))

    def test_context_deduplicates_overlaps_and_preserves_line_bytes(self):
        self.write('input',b'zero\none\nred\nthree\nred\nfive\nsix\n')
        expected=[(1,'context'),(2,'context'),(3,'selected'),(4,'context'),(5,'selected'),(6,'context'),(7,'context')]
        for args in (('-B2','-A2'),('-C','2'),('--context=2',),('--context','2')):
            rows,summary,terminal=self.report(*args,'red','input')
            self.assertEqual([(row['line'],row['role']) for row in rows],expected)
            self.assertEqual(raw(rows[2]['content']),b'red')
            self.assertEqual((summary['context_before'],summary['context_after']),(2,2))
            self.assertEqual((terminal.state,terminal.errors),(0,0))
        rows,summary,_=self.report('--context','red','input',mode='--json')
        self.assertEqual((summary['context_before'],summary['context_after']),(3,3))
        self.assertEqual(len(rows),7)

    def test_stdin_explicit_implicit_and_byte_projection(self):
        contents=b'before\nhi-\xfe\nno\n'
        for mode in ('--json','--data','--data-v2'):
            for strict in (False,True):
                for operands in ((),('-',)):
                    rows,summary,terminal=self.report('-F',b'hi-\xfe',*operands,mode=mode,strict=strict,stdin=contents)
                    self.assertEqual(len(rows),1)
                    self.assertIsNone(rows[0]['file']);self.assertEqual(rows[0]['source'],'stdin')
                    self.assertEqual((rows[0]['line'],raw(rows[0]['content'])),(2,b'hi-\xfe'))
                    self.assertEqual(summary['source_bytes'],len(contents))
                    if terminal:self.assertEqual((terminal.state,terminal.errors),(0,0))
        rows,summary,terminal=self.report('-c','red','-',rc=1,stdin=b'')
        self.assertEqual(rows[0]['count'],0);self.assertTrue(summary['scan_complete']);self.assertEqual(terminal.state,0)

    def test_fuzzy_observes_distance_windows_and_smart_actual_or_rule(self):
        self.write('fuzzy',b'usernane other\nnot close\nUSERNAME\n')
        self.write('smart',b'fixed thing\nbugs here\nnone\n')
        for mode in ('--json','--data-v2'):
            rows,summary,_=self.report('--fuzzy','username','fuzzy',mode=mode)
            self.assertEqual([row['line'] for row in rows],[1,3]);self.assertEqual(summary['fuzzy_distance'],2)
            self.assertEqual(rows[0]['span'],{'start':0,'end':8})
            spans,_,_=self.report('--fuzzy','-o','username','fuzzy',mode=mode)
            self.assertEqual([raw(row['content']) for row in spans],[b'usernane',b'USERNAME'])
            rows,summary,_=self.report('--smart','fix bug','smart',mode=mode)
            self.assertEqual([row['line'] for row in rows],[1,2])
            self.assertEqual(summary['smart_rule'],'case-insensitive-any-pattern-word-substring')
            spans,_,_=self.report('--smart','-o','fix bug','smart',mode=mode)
            self.assertEqual([raw(row['content']) for row in spans],[b'fix',b'bug'])

    def test_multiple_patterns_and_only_matching_stdin_keep_spans(self):
        rows,summary,terminal=self.report('-o','-F','-e','re','-e','red',stdin=b'red red\nblue\n')
        self.assertEqual([(row['record'],raw(row['content']),row['span']) for row in rows],
                         [('span',b'red',{'start':0,'end':3}),('span',b'red',{'start':4,'end':7})])
        self.assertEqual(summary['selected_lines_observed'],1);self.assertEqual(terminal.state,0)
        self.write('input',b'[\nred\nblue\n')
        rows,_,_=self.report('--fuzzy','-e','[','-e','red','input')
        self.assertTrue(rows)

    def test_recursive_filters_and_literal_names_use_actual_sources(self):
        (self.work/'tree/sub').mkdir(parents=True)
        self.write('tree/a.c',b'red\nred\n');self.write('tree/b.py',b'red\n');self.write('tree/sub/c.c',b'red\n')
        os.symlink('..',self.work/'tree/sub/loop')
        rows,summary,_=self.report('-rc','--include=*.c','--exclude','c.c','red','tree')
        self.assertEqual([(row['file'],row['count']) for row in rows],[('tree/a.c',2)])
        self.assertTrue(summary['complete'])
        self.write('--json',b'--data\n')
        rows,_,_=self.report('-c','-F','-e','--data','--','--json')
        self.assertEqual((rows[0]['file'],rows[0]['count']),('--json',1))

    def test_source_faults_never_publish_unverified_lines(self):
        source=self.write('input',b'red\nblue\n')
        for fault,code in (('read','read_failed'),('close','close_failed'),('change','changed_during_read')):
            with self.subTest(fault=fault):
                source.write_bytes(b'red\nblue\n')
                self.env.update(LD_PRELOAD=str(self.hook),GREP_TEST_FAULT=fault,GREP_TEST_FILE=str(source))
                rows,summary,terminal=self.report('-c','red','input',rc=2)
                self.assertEqual([row['record'] for row in rows],['error'])
                self.assertEqual(rows[0]['error'],code);self.assertFalse(summary['complete'])
                self.assertEqual((terminal.state,terminal.errors),(2,1))
        self.env.update(GREP_TEST_FAULT='short')
        source.write_bytes(b'red\nblue\n');rows,_,terminal=self.report('-c','red','input')
        self.assertEqual(rows[0]['count'],1);self.assertEqual(terminal.state,0)
        self.env.update(GREP_TEST_FAULT='directory',GREP_TEST_FILE=str(self.work))
        rows,_,terminal=self.report('-rc','red','.',rc=2)
        self.assertEqual(rows[0]['error'],'directory_read_failed');self.assertEqual(terminal.state,2)

    def test_nonregular_binary_and_long_sources_fail_explicitly(self):
        self.write('binary',b'red\x00blue\n');self.write('long',b'x'*65536)
        os.mkfifo(self.work/'fifo')
        rows,summary,terminal=self.report('-c','red','binary','long','fifo',rc=2)
        self.assertEqual({row['error'] for row in rows},{'binary_input','line_exceeds_json_limit','non_regular_input'})
        self.assertEqual((terminal.state,terminal.errors),(2,3));self.assertEqual(summary['source_failures'],3)
        rows,_,terminal=self.report('-c','red',rc=2,stdin=b'red\0')
        self.assertEqual(rows[0]['error'],'binary_input');self.assertEqual(terminal.state,2)

    def test_pages_match_limits_and_work_limits_do_not_claim_full_scan(self):
        self.write('a',b'red\nred\nred\n');self.write('b',b'red\n');self.write('c',b'blue\n')
        for mode in ('--json','--data-v2'):
            rows,summary,terminal=self.report('-c','--offset','1','--limit','1','red','a','b','c',mode=mode)
            self.assertEqual([(r['file'],r['count']) for r in rows],[('b',1)])
            self.assertFalse(summary['complete']);self.assertEqual(summary['next_offset'],2)
            if terminal:self.assertEqual((terminal.state,terminal.reason),(3,'page_limit'))
        rows,summary,terminal=self.report('-cm1','red','a')
        self.assertEqual((rows[0]['count'],rows[0]['complete']),(1,False))
        self.assertEqual((terminal.state,terminal.reason),(3,'match_limit'))
        rows,summary,terminal=self.report('-cm0','red','a',rc=1)
        self.assertEqual(rows,[]);self.assertEqual(summary['sources_observed'],0);self.assertEqual(terminal.state,3)
        rows,summary,terminal=self.report('-cm0','red','missing',rc=1)
        self.assertEqual(rows,[]);self.assertEqual(summary['source_bytes'],0);self.assertEqual(terminal.reason,'match_limit')
        rows,summary,_=self.report('--offset','1','--limit','1','red','a',mode='--json')
        self.assertEqual((rows[0]['record'],rows[0]['line']),('line',2));self.assertEqual(summary['next_offset'],2)
        rows,summary,_=self.report('-c','--offset','18446744073709551615','red','a',mode='--json')
        self.assertEqual(rows,[]);self.assertEqual(summary['offset'],18446744073709551615)
        self.write('many',b'blue\n'*4097)
        rows,summary,terminal=self.report('-v','red','many',rc=75)
        self.assertEqual(len(rows),4096);self.assertEqual((terminal.state,terminal.reason),(3,'query_collection_limit'))
        self.write('work',b'a'*65535+b'\n')
        rows,summary,terminal=self.report('--fuzzy','z'*64,'work',rc=75)
        self.assertEqual(rows,[]);self.assertLessEqual(summary['fuzzy_work'],50000000)
        self.assertEqual((terminal.state,terminal.reason),(3,'fuzzy_work_limit'))

    def test_invalid_context_and_pattern_limits_fail_before_source(self):
        self.write('input',b'red\n')
        for option in (['-A','4097'],['-B','-1'],['-C1x'],['--context=bad'],['--before-context',''],['--after-context=18446744073709551616']):
            result=self.run_command(['grep','--data-v2',*option,'red','input'])
            self.assertEqual(result.returncode,2,result.stderr)
            terminal=f.stream(result.stdout);self.assertEqual((terminal.count,terminal.state,terminal.errors),(0,2,1))
        for engine,size in (('--fuzzy',65),('--smart',256)):
            rows,summary,terminal=self.report(engine,'a'*size,'input',rc=2)
            self.assertEqual(rows[0]['error'],'pattern_byte_limit');self.assertEqual(summary['sources_observed'],0)
            self.assertEqual(rows[0]['source'],'command');self.assertEqual(summary['source_failures'],0)
            self.assertEqual(terminal.state,2)

    def test_schema_rejects_type_and_semantic_drift(self):
        self.write('input',b'before\nred\n')
        rows,summary,_=self.report('-c','red','input',mode='--json')
        invalid=copy.deepcopy(rows[0]);invalid['count']='1'
        with self.assertRaises(ValidationError):validate_contract('grep-query-row',invalid)
        invalid=copy.deepcopy(summary);invalid['complete']=False
        with self.assertRaises(ValidationError):validate_contract('grep-query-row',invalid)
        rows,_,_=self.report('-B1','red','input')
        invalid=copy.deepcopy(rows[0]);invalid['matched']=True
        with self.assertRaises(ValidationError):validate_contract('grep-query-row',invalid)
        name=b'byte-\xfe';path=os.fsencode(self.work)+b'/'+name
        with open(path,'wb') as source:source.write(b'red\n')
        for mode in ('--json','--data-v2'):
            rows,_,_=self.report('-c','red',name,mode=mode)
            self.assertEqual(raw(rows[0]['file']),name)

    def chain_source(self):
        directory=Path(self.env['HOME'])/'.jash';directory.mkdir(mode=0o700,exist_ok=True)
        chain=directory/'chain.log'
        chain.write_text('0 1699999000.000000000 '+'0'*64+' rc=0 cmd=first recorded entry\n'+
                         '1 1700000000.000000000 '+'a'*64+' rc=0 cmd=recorded fixture\n')
        chain.chmod(0o600)
        return chain

    def test_since_uses_checked_chain_source_and_explicit_filter_boundary(self):
        chain=self.chain_source();original=chain.read_bytes()
        (self.work/'tree').mkdir()
        old=self.write('tree/old',b'red\n');new=self.write('tree/new',b'red\n')
        os.utime(old,(1699999999,1699999999));os.utime(new,(1700000001,1700000001))
        for mode in ('--json','--data','--data-v2'):
            for strict in (False,True):
                rows,summary,_=self.report('-rc','--since','1','red','tree',mode=mode,strict=strict)
                self.assertEqual([(row['file'],row['count']) for row in rows],[('tree/new',1)])
                self.assertEqual((summary['since_sequence'],summary['since_seconds']),(1,1700000000))
                self.assertEqual((summary['since_source'],summary['since_source_bytes']),(str(chain),len(original)))
        self.assertEqual(chain.read_bytes(),original)
        rows,summary,_=self.report('-rc','--since','0','red','tree')
        self.assertEqual({row['file'] for row in rows},{'tree/old','tree/new'})
        self.assertEqual(summary['since_sequence'],0)
        malformed=(original+b'corrupt\n',b'1 18446744073709551615.0 '+b'a'*64+b' rc=0 cmd=overflow\n')
        for contents,reason in zip(malformed,('since_chain_invalid','since_timestamp_range')):
            chain.write_bytes(contents)
            for mode in ('--json','--data-v2'):
                result=self.run_command(['grep',mode,'-rc','--since=1','red','tree'])
                self.assertEqual(result.returncode,2,result.stderr)
                if mode=='--json':self.assertEqual(json.loads(result.stdout)['error']['code'],reason)
                else:
                    terminal=f.stream(result.stdout)
                    self.assertEqual((terminal.count,terminal.state,terminal.reason),(0,2,reason))
        chain.unlink();os.mkfifo(chain,0o600)
        for strict in (False,True):
            result=self.run_command(['grep','--data-v2','-rc','--since','1','red','tree'],strict=strict)
            self.assertEqual(result.returncode,2,result.stderr)
            terminal=f.stream(result.stdout);self.assertEqual((terminal.state,terminal.reason),(2,'since_chain_invalid'))
            unrelated=self.run_command(['pwd','--data-v2'],strict=strict)
            self.assertEqual(unrelated.returncode,0,unrelated.stderr)
            self.assertTrue(unrelated.stdout.startswith(b'JTV2'))

    def test_delivery_and_budget_are_checked(self):
        self.write('input',b'red\nblue\n')
        for mode in ('--json','--data-v2'):
            result=subprocess.run([str(f.JASH),'-c',f'grep {mode} -c red input >/dev/full'],cwd=self.work,env=self.env,capture_output=True,timeout=20)
            self.assertEqual(result.returncode,125,result.stderr)
        self.env['JASH_LLM_BUDGET']='1'
        rows,_,terminal=self.report('-c','red','input')
        self.assertEqual(rows[0]['count'],1);self.assertEqual(terminal.state,0)
        result=self.run_command(['grep','--json','-c','red','input'])
        self.assertEqual(result.returncode,75,result.stderr)
        self.assertEqual(json.loads(result.stdout)['error']['code'],'output_budget_exceeded')

    def test_stdin_poll_accepts_cancel_while_writer_is_open_and_reuses_session(self):
        self.cancel_while_paused(['grep','--data-v2','red'],'stdin-hold')

    def test_since_read_accepts_cancel_before_first_observation(self):
        chain=self.chain_source();(self.work/'tree').mkdir();self.write('tree/input',b'red\n')
        self.cancel_while_paused(['grep','--data-v2','-rc','--since','1','red','tree'],'since-hold',chain)

    def cancel_while_paused(self,argv,fault,source=None):
        control,child=socket.socketpair();ready,notify=os.pipe()
        release,send_release=os.pipe()
        env=dict(self.env,LD_PRELOAD=str(self.hook),GREP_TEST_FAULT=fault,GREP_TEST_READY=str(notify),
                 GREP_TEST_RELEASE=str(release),JASH_AGENT_CANCEL_FD=str(child.fileno()))
        if source:env['GREP_TEST_FILE']=str(source)
        proc=subprocess.Popen([str(f.AGENT)],cwd=self.work,env=env,pass_fds=(child.fileno(),notify,release),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
        child.close();os.close(notify);os.close(release);pending={}
        def line(stream):
            fd=stream.fileno();data=pending.pop(fd,b'');deadline=time.monotonic()+10
            while b'\n' not in data:
                self.assertTrue(select.select([fd],[],[],max(0,deadline-time.monotonic()))[0],data)
                part=os.read(fd,65536);self.assertTrue(part,data);data+=part
            row,_,rest=data.partition(b'\n');pending[fd]=rest;return json.loads(row)
        try:
            self.assertTrue(line(proc.stdout)['cancel']['enabled'])
            ident='grep-'+fault
            request={'id':ident,'protocol_version':2,'argv':argv,'caps':['effect:read'],'cancelable':True}
            proc.stdin.write((json.dumps(request)+'\n').encode());self.assertEqual(line(proc.stdout).get('event'),'start')
            self.assertTrue(select.select([ready],[],[],10)[0],('IO boundary not reached',argv));self.assertEqual(os.read(ready,1),b'1')
            control.sendall((json.dumps({'cancel':ident},separators=(',',':'))+'\n').encode())
            if source:os.write(send_release,b'1')
            self.assertEqual(line(control),{'cancel':ident,'accepted':True})
            chunks=[]
            while True:
                row=line(proc.stdout)
                if row.get('event')!='chunk':break
                if row['stream']=='stdout':chunks.append(base64.b64decode(row['data_b64']))
            self.assertEqual(row['rc'],130,row);self.assertTrue(row['cancelled'],row)
            terminal=f.stream(b''.join(chunks));self.assertEqual((terminal.state,terminal.producer,terminal.reason),(1,130,'request_cancelled'))
            if source:self.assertEqual(terminal.count,0)
            else:
                summary=projection(terminal.items[-1]);self.assertFalse(summary['complete']);self.assertFalse(summary['scan_complete'])
            self.write('input',b'red\n')
            request={'id':'grep-after-cancel','protocol_version':2,'argv':['grep','--data-v2','-c','red','input'],'caps':['effect:read']}
            proc.stdin.write((json.dumps(request)+'\n').encode());self.assertEqual(line(proc.stdout).get('event'),'start')
            chunks=[]
            while True:
                row=line(proc.stdout)
                if row.get('event')!='chunk':break
                if row['stream']=='stdout':chunks.append(base64.b64decode(row['data_b64']))
            self.assertEqual(row['rc'],0,row);self.assertEqual(f.stream(b''.join(chunks)).state,0)
        finally:
            control.close();os.close(ready);os.close(send_release)
            if proc.poll() is None:proc.kill()
            proc.communicate(timeout=10)


if __name__=='__main__':unittest.main()
