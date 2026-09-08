#!/usr/bin/env python3
"""Keep compiler outputs separate from the inputs used to validate their cache."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import time

from build_inputs import tree_identity


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(args):
    root, out = args.root.resolve(), args.out.resolve()
    for source_root in (root / 'hybris', root / 'compat', root / 'tools'):
        if out.is_relative_to(source_root) or source_root.is_relative_to(out):
            raise ValueError('output directory overlaps repository build inputs')
    snapshot, work = out / 'inputs', out / 'src'
    state_path = out / 'build-cache.json'
    previous = json.loads(state_path.read_text()) if state_path.exists() else None
    # A failed/interrupted build cannot publish reusable cache provenance.
    state_path.unlink(missing_ok=True)
    (out / 'manifest.json').unlink(missing_ok=True)
    (out / 'build-report.json').unlink(missing_ok=True)
    if snapshot.exists():
        shutil.rmtree(snapshot)
    snapshot.mkdir()
    for name in ('hybris', 'compat'):
        shutil.copytree(root / name, snapshot / name, symlinks=True)
    shutil.copytree(args.protocols.resolve(), snapshot / 'protocols', symlinks=True)
    (snapshot / 'tools').mkdir()
    for name in ('stage-runtime.py', 'prepare-build.py'):
        shutil.copy2(root / 'tools' / name, snapshot / 'tools' / name)
    headers = out / 'headers'
    # Allow --headers to reuse this output's existing snapshot without deleting
    # the copy source before it is read.
    headers_next = out / 'headers-next'
    if out.is_relative_to(args.headers.resolve()) or args.headers.resolve().is_relative_to(headers_next):
        raise ValueError('header source overlaps the output staging directory')
    if headers_next.exists():
        shutil.rmtree(headers_next)
    shutil.copytree(args.headers, headers_next, symlinks=True,
                    ignore=shutil.ignore_patterns('.git'))
    if headers.exists():
        shutil.rmtree(headers)
    headers_next.replace(headers)
    source = tree_identity(snapshot)
    header_identity = tree_identity(headers)
    key = {'version': 1, 'builder': args.builder, 'debug': args.debug,
           'headers': header_identity['sha256'],
           'build_script': digest(root / 'tools/build-aarch64.sh'),
           'prepare_script': digest(Path(__file__)),
           'input_script': digest(root / 'tools/build_inputs.py')}
    files = {item['path']: item for item in source['files']}
    old_files = previous.get('files', {}) if previous else {}
    changed = sorted(name for name in files.keys() | old_files.keys()
                     if files.get(name) != old_files.get(name))
    reason = None
    if not args.incremental:
        reason = 'clean build requested'
    elif not previous or not work.is_dir():
        reason = 'no completed build cache'
    elif previous.get('key') != key:
        reason = 'builder, headers, configuration or build scripts changed'
    elif files.keys() != old_files.keys():
        reason = 'input files added or removed'
    elif any(Path(name).suffix not in {'.c', '.cc', '.cpp', '.cxx', '.S', '.s'}
             or 'link' in files[name] for name in changed):
        reason = 'headers, build rules, generators or other non-compiler inputs changed'

    if reason:
        if work.exists():
            shutil.rmtree(work)
        shutil.copytree(snapshot, work, symlinks=True)
    else:
        # Compare actual cached inputs too: edits inside the cache must not be
        # silently attributed to the source snapshot. Do not preserve an old
        # input mtime after checkout/revert or a content change with old mtime.
        for name, item in files.items():
            source_path, target = snapshot / name, work / name
            if 'link' in item:
                matches = target.is_symlink() and str(target.readlink()) == item['link']
            else:
                matches = (not target.is_symlink() and target.is_file()
                           and digest(target) == item['sha256']
                           and bool(target.stat().st_mode & 0o111) == item['executable'])
            if matches:
                continue
            if target.is_dir() and not target.is_symlink():
                shutil.rmtree(target)
            else:
                target.unlink(missing_ok=True)
            target.parent.mkdir(parents=True, exist_ok=True)
            if 'link' in item:
                target.symlink_to(item['link'])
            else:
                shutil.copy2(source_path, target)
                os.utime(target, None)
            if name not in changed:
                changed.append(name)

    for name in ('install', 'runtime'):
        path = out / name
        if path.exists():
            shutil.rmtree(path)
    pending = {'key': key, 'files': files}
    (out / 'build-cache-pending.json').write_text(json.dumps(pending, indent=2) + '\n')
    report = {'mode': 'clean' if reason else 'incremental', 'reason': reason,
              'changed_inputs': sorted(changed), 'started': args.started,
              'status': 'building'}
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Build cache:', reason or f'reuse; {len(changed)} changed input(s)')


def finish(args):
    out = args.out.resolve()
    report_path = out / 'build-report.json'
    report = json.loads(report_path.read_text())
    report.update(status='complete', elapsed_seconds=round(time.monotonic() - report.pop('started'), 3))
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    (out / 'build-cache-pending.json').replace(out / 'build-cache.json')
    print(f"Build complete: {report['mode']}, {report['elapsed_seconds']} seconds")


parser = argparse.ArgumentParser(description=__doc__)
sub = parser.add_subparsers(dest='action', required=True)
prepare_parser = sub.add_parser('prepare')
prepare_parser.add_argument('--root', type=Path, required=True)
prepare_parser.add_argument('--out', type=Path, required=True)
prepare_parser.add_argument('--protocols', type=Path, required=True)
prepare_parser.add_argument('--headers', type=Path, required=True)
prepare_parser.add_argument('--builder', required=True)
prepare_parser.add_argument('--debug', action='store_true')
prepare_parser.add_argument('--incremental', action='store_true')
prepare_parser.add_argument('--started', type=float, required=True)
finish_parser = sub.add_parser('finish')
finish_parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
prepare(args) if args.action == 'prepare' else finish(args)
