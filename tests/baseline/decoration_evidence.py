"""Use SPIRV-Tools to independently normalize decoration semantics for audits."""
import hashlib
import subprocess


def normalize_decorations(path, disassembly, original):
    if original:
        if 'OpGroupDecorate ' not in disassembly or 'OpGroupMemberDecorate ' not in disassembly:
            raise ValueError('grouped fixture lost its variable/member applications')
    elif 'OpGroupDecorate ' in disassembly or 'OpGroupMemberDecorate ' in disassembly:
        raise ValueError('converted fixture retains group applications')
    # Strip debug only in this derived audit copy. The installed flatten pass
    # does not advance past OpMemberName; raw submissions retain all debug data.
    normalized = path.with_suffix('.flat.spv')
    subprocess.run(['spirv-opt', '--strip-debug', '--flatten-decorations', str(path), '-o', str(normalized)],
                   check=True, capture_output=True, timeout=30)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(normalized)],
                   check=True, capture_output=True, timeout=30)
    text = subprocess.check_output(['spirv-dis', '--raw-id', str(normalized)], text=True)
    normalized.with_suffix('.spvasm').write_text(text)
    return text, {'name': normalized.name, 'sha256': hashlib.sha256(normalized.read_bytes()).hexdigest()}
