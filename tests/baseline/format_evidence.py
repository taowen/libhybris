"""Audit the raw/effective format policy against actual probe queries."""
import re

# Vulkan 1.0 format enums, independent of the adapter's lookup table.
PAIRS = [(base + sign, base + sign + 2) for base in (11, 18, 39, 72, 79, 93) for sign in (0, 1)]
FLAGS = r'(0x[0-9a-f]+,0x[0-9a-f]+,0x[0-9a-f]+)'
VERTEX = 0x40


def flags(text):
    return tuple(int(value, 16) for value in text.split(','))


def queries(log):
    rows = re.findall(r'^SCALED FORMAT index=(\d+) scaled=(\d+) integer=(\d+) legacy=' + FLAGS +
                      ' properties2=' + FLAGS + ' fetch=' + FLAGS + '$', log, re.M)
    if len(rows) != 12:
        raise ValueError('expected all 12 scaled format queries')
    result = []
    for i, (index, scaled, integer, legacy, chained, fetch) in enumerate(rows):
        if int(index) != i or (int(scaled), int(integer)) != PAIRS[i] or flags(legacy) != flags(chained):
            raise ValueError('format query identity or legacy/properties2 mismatch')
        result.append((flags(legacy), flags(fetch)))
    return result


def format_evidence(log, native=None):
    if 'SCALED DIVISOR UNSUPPORTED' in log and 'HYBRIS_SCALED_FORMAT' not in log:
        return {'snapshot': None, 'reason': 'divisor capability rejected before device creation'}
    observed = queries(log)
    reference = queries(native) if native is not None else None
    summaries = re.findall(r'^HYBRIS_SCALED_VERTEX experimental=1 force=([01]) fallback_mask=0x([0-9a-f]+)$', log, re.M)
    if len(summaries) != 1:
        raise ValueError('expected one device format mask')
    force, mask = int(summaries[0][0]), int(summaries[0][1], 16)
    rows = re.findall(r'^HYBRIS_SCALED_FORMAT device=(\S+) physical=(\S+) index=(\d+) scaled=(\d+) integer=(\d+) raw=' +
                      FLAGS + ' fetch=' + FLAGS + ' effective=' + FLAGS + r' fallback=([01]) reason=(\S+)$', log, re.M)
    if len(rows) != 12 or 'HYBRIS_SCALED_FORMAT truncated' in log:
        raise ValueError('missing or truncated format decision snapshot')
    evidence, expected_mask, owner = [], 0, rows[0][:2]
    for i, row in enumerate(rows):
        device, physical, index, scaled, integer, raw, fetch, effective, fallback, reason = row
        raw, fetch, effective = flags(raw), flags(fetch), flags(effective)
        if (device, physical) != owner or int(index) != i or (int(scaled), int(integer)) != PAIRS[i]:
            raise ValueError('format decision owner or format identity mismatch')
        native_vertex, integer_vertex = bool(raw[2] & VERTEX), bool(fetch[2] & VERTEX)
        expected_fallback = (force or not native_vertex) and integer_vertex
        expected_reason = ('native-scaled' if not force and native_vertex else 'integer-fetch-unavailable'
                           if not integer_vertex else 'forced-integer-fetch' if force else 'missing-scaled-fetch')
        expected = raw[:2] + (raw[2] | (VERTEX if expected_fallback else 0),)
        if bool(int(fallback)) != bool(expected_fallback) or reason != expected_reason or effective != expected:
            raise ValueError('format decision differs from raw capability policy')
        if observed[i] != (effective, fetch):
            raise ValueError('effective decision differs from application queries')
        if reference is not None and reference[i] != (raw, fetch):
            raise ValueError('backend raw decision differs from independent native queries')
        if expected_fallback:
            expected_mask |= 1 << i
        evidence.append({'scaled': int(scaled), 'integer': int(integer), 'raw': raw, 'fetch': fetch,
                         'effective': effective, 'fallback': bool(expected_fallback), 'reason': reason})
    if mask != expected_mask:
        raise ValueError('device conversion mask differs from format decisions')
    return {'device': owner[0], 'physical': owner[1], 'force': bool(force), 'mask': mask,
            'native_query_comparison': 'PASS' if reference is not None else 'not available', 'decisions': evidence}
