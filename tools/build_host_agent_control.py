#!/usr/bin/env python3
"""GCC/Linux host control from the explicit CMake agent source selection.

This does not run CMake and is not a Beyond/Tina/native qualification. The
normal CMake product build stays authoritative. All commands, source hashes
and the CMake digest are recorded for reproducing this limited host gate.
"""
import argparse
import hashlib
import json
import re
import shlex
import subprocess
from pathlib import Path

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[1])
    parser.add_argument('--out',required=True,type=Path)
    parser.add_argument('--cc',default='gcc')
    parser.add_argument('--optimization',choices=('O2','Og'),default='O2',
                        help='O2 release control or Og semantic/debug control; failures stay explicit')
    args=parser.parse_args();root=args.source_root.resolve();out=args.out.resolve()
    if out==root or out.is_relative_to(root/'c'):
        parser.error('output must not replace sources')
    out.mkdir(parents=True,exist_ok=True)
    cmake=(root/'CMakeLists.txt').read_text()
    def block(pattern):
        found=re.findall(pattern,cmake,re.S)
        if len(found)!=1: raise ValueError('ambiguous CMake selection: '+pattern)
        return shlex.split(re.sub(r'#[^\n]*','',found[0]))
    source=block(r'set\(JASH_SOURCES\s+(.*?)\n\)')
    remove=block(r'list\(REMOVE_ITEM JASH_AGENT_SOURCES\s+(.*?)\n\)')
    selected=[]
    variables={'${OSIF_SRC}':'c/osif/linux/syscall.S','${OSIF_OSINFO_SRC}':'c/osif/linux/osinfo.c'}
    excluded=['$<$<BOOL:${JASH_BETHOS}>:c/osif/bethos/posix_compat.c>',
              '$<$<BOOL:${JASH_TINALINUX}>:${JASH_TINA_PROCNU_DIR}/tina_procnu.c>',
              '$<$<BOOL:${JASH_TINALINUX}>:c/osif/tina/surface.c>']
    for item in source:
        if item in excluded or item in remove: continue
        item=variables.get(item,item)
        if '$' in item or not (root/item).is_file(): raise ValueError('unresolved source '+item)
        selected.append(item)
    if 'list(APPEND JASH_AGENT_SOURCES c/agent/jash_agentd_main.c)' not in cmake:
        raise ValueError('unknown agent entry')
    selected.append('c/agent/jash_agentd_main.c')
    if len(set(selected))!=len(selected): raise ValueError('duplicate source')
    digest=hashlib.sha256(''.join(item+':'+sha(root/item)+'\n' for item in selected).encode()).hexdigest()
    header=out/'jash_hostcontrol_version.h'
    header.write_text('#define JASH_GITHASH "host-control-sha256-'+digest+'"\n')
    flags=['-'+args.optimization,'-g','-std=c11','-D_POSIX_C_SOURCE=200809L','-D_DEFAULT_SOURCE','-DJASH_TARGET_POSIX',
           '-DJASH_AGENT_ONLY=1','-DJASH_AGENT_RUNTIME_NAME="jash-agentd"','-D_FORTIFY_SOURCE=2',
           '-Wall','-Wextra','-Wpedantic','-Wno-unused-parameter','-Wformat=2','-Wvla','-Wshadow',
           '-Wpointer-arith','-Wstrict-prototypes','-Wmissing-prototypes','-Wno-missing-field-initializers',
           '-Werror','-fstack-protector-strong','-fPIE','-ffunction-sections','-fdata-sections',
           '-include',str(header)]
    for directory in ('c','c/sys','c/osif','c/liquid','libjef'): flags+=['-I'+str(root/directory)]
    manifest={'schema':'jash.gcc-host-control/1','scope':'linux-host-cmake-source-selection-without-cmake-execution',
              'optimization':args.optimization,
              'native_beyond_qualified':False,'cmake_sha256':sha(root/'CMakeLists.txt'),
              'selected_source_digest':digest,'compiler':subprocess.check_output([args.cc,'--version'],text=True).splitlines()[0],
              'sources':{s:sha(root/s) for s in selected},'commands':[],'passed':False}
    def run(command):
        manifest['commands'].append(command)
        with (out/'build.log').open('ab') as log:
            log.write((shlex.join(command)+'\n').encode())
            result=subprocess.run(command,cwd=root,stdout=log,stderr=log)
        if result.returncode: raise RuntimeError('command failed; see '+str(out/'build.log'))
    try:
        objects=[];failed_sources=[]
        for s in selected:
            obj=out/'objects'/(s+'.o');obj.parent.mkdir(parents=True,exist_ok=True)
            try:
                run([args.cc,*flags,'-c',str(root/s),'-o',str(obj)]);objects.append(str(obj))
            except RuntimeError:
                failed_sources.append(s)
        if failed_sources:
            manifest['failed_sources']=failed_sources
            raise RuntimeError('source compile gates failed: '+', '.join(failed_sources))
        run([args.cc,'-pie','-Wl,--gc-sections','-Wl,-z,relro,-z,now,-z,noexecstack',*objects,'-lm','-o',str(out/'jash-agentd')])
        verify=['libjef/jef_verify_main.c','libjef/jef_verify.c','libjef/ed25519.c','c/module/jash_crypto.c','c/module/blake3.c']
        run([args.cc,*flags,*[str(root/s) for s in verify],'-o',str(out/'jef_verify')])
        manifest['verifier_sources']={s:sha(root/s) for s in verify}
        manifest['passed']=True
        manifest['binaries']={s:sha(out/s) for s in ('jash-agentd','jef_verify')}
    finally:
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps({'passed':True,'sources':len(selected),'scope':manifest['scope']}))
if __name__=='__main__':main()
