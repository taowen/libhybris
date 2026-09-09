"""Check ICD device lifetimes and their instance parents in the life workload."""
from collections import Counter


def device_evidence(output: str, prefix='HYBRIS_ICD') -> dict:
    instances = {}
    devices = {}
    generations = set()
    handles = Counter()
    peak = destroyed = 0
    for line in output.splitlines():
        if not line.startswith((prefix + '_INSTANCE ', prefix + '_DEVICE ')):
            continue
        parts = line.split()
        is_device = parts[0] == prefix + '_DEVICE'
        if len(parts) != (5 if is_device else 4) or parts[1] not in ('create', 'destroy'):
            raise ValueError('incomplete or truncated lifecycle trace: ' + line)
        fields = dict(part.split('=', 1) for part in parts[2:])
        generation = int(fields['generation'])
        handle = fields['handle']
        if generation <= 0:
            raise ValueError('invalid generation')
        if not is_device:
            if parts[1] == 'create':
                if generation in instances:
                    raise ValueError('duplicate live instance generation')
                instances[generation] = handle
            else:
                if any(parent == generation for _, parent in devices.values()):
                    raise ValueError('instance destroyed with live device records')
                if instances.pop(generation, None) != handle:
                    raise ValueError('unpaired instance destroy')
            continue
        parent = int(fields['instance'])
        if parent not in instances:
            raise ValueError('device event has no live instance parent')
        if parts[1] == 'create':
            if generation in generations or any(h == handle for h, _ in devices.values()):
                raise ValueError('device generation or live handle reused')
            generations.add(generation)
            devices[generation] = (handle, parent)
            handles[handle] += 1
            peak = max(peak, len(devices))
        else:
            if devices.pop(generation, None) != (handle, parent):
                raise ValueError('unpaired device destroy')
            destroyed += 1
    if instances or devices or len(generations) != 19 or destroyed != 19 or peak < 2:
        raise ValueError(f'incomplete life workload: created={len(generations)} destroyed={destroyed} peak={peak}')
    return dict(created=len(generations), destroyed=destroyed, remaining=len(devices),
                peak_live=peak, reused_handles=sum(n > 1 for n in handles.values()),
                scope=prefix + ' device records and instance parents; no resource generations')
