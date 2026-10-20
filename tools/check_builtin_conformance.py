#!/usr/bin/env python3
"""Check the reviewed denominator and the executable conformance registrations.

This checks evidence wiring, not behavioral correctness by itself. Every
declared semantic gate must have a labelled CTest execution. An optional JUnit
result must contain those executions and rejects failed, skipped, unfinished or
truncated gates, including inner unittest and legacy shell skips. CI separately
runs the master suite; registration in that suite is not qualification evidence
here.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def registered_tests(build, ctest):
    result = subprocess.run([ctest, '--test-dir', str(build), '--show-only=json-v1'],
                            cwd=ROOT, capture_output=True, text=True, check=True)
    ctests = json.loads(result.stdout)['tests']
    commands = [(entry['name'], entry['command']) for entry in ctests if 'command' in entry]
    labelled = {entry['name'] for entry in ctests if any(
        prop['name'] == 'LABELS' and 'builtin-contracts' in prop['value']
        for prop in entry.get('properties', []))}
    return commands, labelled


def semantic_evidence(required, commands, labelled):
    """Bind declared files to the CTest gates that qualification actually runs."""
    # These drivers compile and execute the real C harness, including its fault
    # modes. A source filename mentioned only by a master registration cannot
    # establish that the harness ran.
    drivers = {'tests/test_digest_engine_unit.c': 'tests/test_digest_project.py',
               'tests/test_assert_delivery.c': 'tests/test_assert_native.py'}
    evidence, errors = {}, []
    for test in sorted(required):
        path = Path(drivers.get(test, test))
        needle = path.stem if path.suffix == '.c' else path.name
        found = {name for name, argv in commands if any(
            Path(argument).name == needle or needle in argument.split()
            for argument in argv)}
        selected = found & labelled
        if not found:
            errors.append('Declared semantic gate is not registered in CTest: ' + test)
        elif not selected:
            errors.append('Declared semantic gate has no builtin-contracts label: ' + test)
        else:
            evidence[test] = sorted(selected)
    return evidence, errors


def junit_errors(document, labelled, evidence):
    """Reject absent executions and exit-zero runners that skipped assertions."""
    errors = []
    cases = document.findall('.//testcase')
    observed = {case.attrib.get('name') for case in cases}
    for missing in sorted(labelled - observed):
        errors.append('Labelled gate missing from JUnit evidence: ' + missing)
    for test, gates in sorted(evidence.items()):
        if not observed.intersection(gates):
            errors.append('Declared semantic gate has no executed JUnit evidence: ' + test)
    names = set()
    for case in cases:
        name = case.attrib.get('name', '<unnamed>')
        if name in names:
            errors.append('Duplicate JUnit gate evidence: ' + name)
        names.add(name)
        output = '\n'.join(node.text or '' for stream in ('system-out', 'system-err')
                           for node in case.findall(stream))
        if (any(case.find(kind) is not None for kind in ('failure', 'error', 'skipped')) or
                case.attrib.get('status') in ('notrun', 'disabled', 'skipped')):
            errors.append('Failed/skipped/unfinished gate: ' + name)
        # CTest may mark a gate passed while removing the runner's final
        # unittest/legacy summary from JUnit. Its omitted bytes cannot prove
        # that no assertions were skipped. Match the CTest marker itself;
        # product fields such as "truncated": true are unrelated evidence.
        if re.search(r'\[This part of the test output was removed since it exceeds '
                     r'the threshold of [0-9]+ bytes\.\]', output):
            errors.append('Truncated CTest output is not qualification: ' + name)
        if (re.search(r'OK\s*\([^)]*\bskipped=[1-9]\d*\b|\bSkipTest\b', output) or
                re.search(r'\bRan\s+0\s+tests?\b', output)):
            errors.append('Inner unittest skip/empty suite is not qualification: ' + name)
        if (re.search(r'^\s*SKIP(?:PED)?(?:\s|:|$)', output, re.MULTILINE | re.IGNORECASE) or
                re.search(r'\b(?:absent|unavailable|missing)\s*[—–-]\s*skipped\b',
                          output, re.IGNORECASE)):
            errors.append('Inner legacy shell skip is not qualification: ' + name)
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build')
    parser.add_argument('--ctest', default=os.environ.get('CTEST', 'ctest'))
    parser.add_argument('--junit', type=Path)
    parser.add_argument('--allow-partial', action='store_true',
                        help='Development inventory only; never used for qualification in CI.')
    args = parser.parse_args()
    subprocess.run([sys.executable, str(ROOT / 'tools/gen_builtin_contracts.py'), '--check'], check=True)
    manifest = json.loads((ROOT / 'schemas/BUILTIN_CONTRACTS.json').read_text())
    errors = []
    candidates = [row for row in manifest['builtins'] if row['role'] == 'structured']
    if not args.allow_partial and not manifest['denominator_frozen']:
        errors.append('The reviewed operation denominator has not been frozen.')
    required = set()
    for row in candidates:
        modes = row['modes']
        if not args.allow_partial and (not row['operations_complete'] or
            {m['mode'] for m in modes} != {'json', 'data'} or
            not all(m['qualified'] and m['options_complete'] for m in modes)):
            errors.append('Unqualified/incomplete builtin: ' + row['name'])
        for mode in modes:
            if mode['qualified']:
                required.update(test.split('::')[0] for test in mode['semantic_tests'])
    commands, labelled = registered_tests(args.build_dir, args.ctest)
    if not labelled:
        errors.append('No builtin-contracts CTest label was configured.')
    evidence, wiring_errors = semantic_evidence(required, commands, labelled)
    errors.extend(wiring_errors)
    if args.junit:
        document = ET.parse(args.junit).getroot()
        errors.extend(junit_errors(document, labelled, evidence))
    print(json.dumps(dict(builtin_names=len(manifest['builtins']), structured_candidates=len(candidates),
                          declared_semantic_files=len(required), registered_semantic_files=len(evidence),
                          labelled_ctest_gates=len(labelled), denominator_frozen=manifest['denominator_frozen'],
                          qualification=args.junit is not None and not errors and not args.allow_partial), indent=2))
    for error in errors:
        print(error, file=sys.stderr)
    return bool(errors)


if __name__ == '__main__':
    raise SystemExit(main())
