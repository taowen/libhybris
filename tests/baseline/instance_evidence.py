"""Validate the bounded ICD instance trace from the concurrent live workload."""
from collections import Counter


def instance_evidence(output: str) -> dict:
    live = {}
    created = set()
    handles = Counter()
    peak = 0
    destroyed = 0
    for line in output.splitlines():
        if not line.startswith('HYBRIS_ICD_INSTANCE '):
            continue
        parts = line.split()
        if len(parts) != 4 or parts[1] not in ('create', 'destroy'):
            raise ValueError('incomplete or truncated instance trace: ' + line)
        fields = dict(part.split('=', 1) for part in parts[2:])
        generation = int(fields['generation'])
        handle = fields['handle']
        if generation <= 0:
            raise ValueError('invalid generation')
        if parts[1] == 'create':
            if generation in created or handle in live.values():
                raise ValueError('generation or live instance handle reused')
            created.add(generation)
            live[generation] = handle
            handles[handle] += 1
            peak = max(peak, len(live))
        else:
            if live.pop(generation, None) != handle:
                raise ValueError('destroy without matching live instance')
            destroyed += 1
    if live or len(created) != 16 or destroyed != 16:
        raise ValueError(f'expected 16 complete lifetimes, got {len(created)}/{destroyed}, live={live}')
    if peak != 4:
        raise ValueError(f'expected four simultaneously live instances, got {peak}')
    return dict(created=len(created), destroyed=destroyed, remaining=len(live),
                peak_live=peak, reused_handles=sum(count > 1 for count in handles.values()),
                scope='ICD instance records only; not device/resource ownership')
