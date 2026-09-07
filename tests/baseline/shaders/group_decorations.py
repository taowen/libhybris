"""Encode equivalent grouped decorations for the independent shader fixtures."""
import struct


def group_module(binary):
    words = list(struct.unpack('<' + 'I' * (len(binary) // 4), binary))
    groups, retained = {}, []
    at, insertion = 5, None
    while at < len(words):
        count, opcode = words[at] >> 16, words[at] & 0xffff
        instruction = words[at:at + count]
        if not count or len(instruction) != count:
            raise ValueError('malformed shader instruction')
        # Keep BuiltIn declarations explicit; group locations and layout data.
        start = 2 if opcode == 71 else 3
        if opcode in (71, 72) and instruction[start] != 11:
            if insertion is None:
                insertion = len(retained)
            start = 2 if opcode == 71 else 3
            key = opcode, tuple(instruction[start:])
            groups.setdefault(key, []).extend(instruction[1:start])
        else:
            retained.extend(instruction)
        at += count
    if insertion is None:
        raise ValueError('fixture has no decorations to group')
    annotations = []
    next_id = words[3]
    for (opcode, decoration), targets in groups.items():
        annotations.extend([((2 + len(decoration)) << 16) | 71, next_id, *decoration])
        annotations.extend([(2 << 16) | 73, next_id])
        annotations.extend([((2 + len(targets)) << 16) | (74 if opcode == 71 else 75), next_id, *targets])
        next_id += 1
    words[3] = next_id
    result = words[:5] + retained[:insertion] + annotations + retained[insertion:]
    return struct.pack('<' + 'I' * len(result), *result)
