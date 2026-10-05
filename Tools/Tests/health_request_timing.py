"""Separate authenticated health HTTP latency from parsing/collection work."""
import json
import math
import time

SUMMARY_SCOPE = 'authenticatedHealthWithoutRawWorkSamplesRequestAndResponseBodyOnly'
REQUEST_SCOPE = 'authenticatedHealthRequestAndResponseBodyOnly'


def sample_health(request, raw_processing=False, clock=time.monotonic):
    def timed(path):
        start = clock()
        status, body = request(path)
        milliseconds = (clock() - start) * 1000
        if not math.isfinite(milliseconds) or milliseconds < 0:
            raise ValueError('Invalid health HTTP clock interval')
        if status != 200:
            raise ValueError('Authenticated health sampling failed')
        return body, milliseconds

    body, http_ms = timed('/debug/health?workSamples=' + ('1' if raw_processing else '0'))
    if raw_processing:
        ordinary_body, summary_ms = timed('/debug/health?workSamples=0')
    else:
        ordinary_body, summary_ms = body, http_ms
    # Decode only after the two HTTP clocks have stopped. Collection belongs to
    # the caller and cannot accidentally enter either latency measurement.
    health = json.loads(body)
    ordinary = json.loads(ordinary_body) if raw_processing else health
    for name in ('nativeBuildHash', 'webBuildHash'):
        if not health['server'].get(name) or health['server'][name] != ordinary['server'].get(name):
            raise ValueError('Health build identity changed between requests')
    if sorted(c['cameraId'] for c in health['cameras']) != sorted(c['cameraId'] for c in ordinary['cameras']):
        raise ValueError('Health camera membership changed between requests')
    if any('rawSamplesNS' in c['processing']['workLatency'] for c in ordinary['cameras']):
        raise ValueError('Summary request unexpectedly exported raw processing samples')
    return health, {'httpMs': http_ms, 'healthHttpScope': REQUEST_SCOPE,
                    'healthHttpIncludesRawSamples': bool(raw_processing),
                    'summaryHttpMs': summary_ms, 'summaryHttpScope': SUMMARY_SCOPE}
