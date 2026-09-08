"""Preserve converter sidecar files referenced by GFXReconstruct JSONL."""
import io
import json
from pathlib import PurePosixPath
import shlex
import tarfile


def preserve_binaries(shell, remote, evidence, sha, folder='calls', manifest_name='converted-files.json'):
    command = 'cd ' + shlex.quote(remote)
    archive = shell(command + ' && tar cf - ' + shlex.quote(folder), capture_output=True, check=True, timeout=30)
    with tarfile.open(fileobj=io.BytesIO(archive.stdout)) as bundle:
        for member in bundle:
            name = PurePosixPath(member.name)
            if name.is_absolute() or '..' in name.parts or not name.parts or name.parts[0] != folder:
                raise ValueError('tool archive path escaped its directory')
            destination = evidence.joinpath(*name.parts)
            if member.isdir():
                destination.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(bundle.extractfile(member).read())
            else:
                raise ValueError('converter archive contains a link or special file')
    hashes = shell(command + ' && find ' + shlex.quote(folder) + ' -type f -exec sha256sum {} +',
                   capture_output=True, text=True, check=True, timeout=30)
    expected = dict(line.split(None, 1)[::-1] for line in hashes.stdout.splitlines())
    actual = {str(p.relative_to(evidence)): sha(p) for p in (evidence / folder).rglob('*') if p.is_file()}
    if actual != expected:
        raise ValueError('saved converter binaries differ from device files')
    manifest = evidence / manifest_name
    manifest.write_text(json.dumps(actual, indent=2) + '\n')
    return {'files': len(actual), 'manifest_sha256': sha(manifest)}


def check_references(calls, evidence):
    references = set()
    def visit(value):
        if isinstance(value, dict):
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)
        elif isinstance(value, str) and value.startswith('calls/'):
            name = PurePosixPath(value)
            if '..' in name.parts or not evidence.joinpath(*name.parts).is_file():
                raise ValueError('missing converter binary: ' + value)
            references.add(value)
    for line in calls.open():
        visit(json.loads(line))
    return len(references)
