"""Summarize vcperf Chrome time traces without printing captured environment data.

Phase durations are aggregate compiler work, not wall-clock build time. Header
durations are inclusive and nested: do not sum them into a total parse cost.
"""
import argparse
from collections import defaultdict
import json
from pathlib import Path


def project_path(name):
    name = str(name).replace('\\', '/')
    for marker in ('WitnessCamera/', 'WitnessServer/', 'WitnessCommon/', 'Tools/'):
        index = name.find('/' + marker)
        if index >= 0:
            return name[index + 1:]
    return name.rsplit('/', 1)[-1]


def summarize(trace):
    events = trace.get('traceEvents', []) if isinstance(trace, dict) else trace
    stacks = defaultdict(list)
    last_sources = {}
    phases = defaultdict(float)
    phase_counts = defaultdict(int)
    attributed = defaultdict(float)
    units = defaultdict(lambda: defaultdict(float))
    headers = defaultdict(lambda: {'inclusiveSeconds': 0.0, 'visits': 0})
    unmatched = 0
    for event in events:
        phase = event.get('ph')
        key = (event.get('pid'), event.get('tid'))
        if phase == 'B':
            source = event.get('args', {}).get('File Input')
            name = str(event.get('name', ''))
            if name.startswith('CL Invocation'):
                last_sources.pop(key, None)
            # /MP invocations list many indexed inputs. Their per-file event
            # starts inside FrontEndPass, so attribute the enclosing phase
            # retroactively and retain it for the following BackEndPass.
            source_file = name.lower().endswith(('.cpp', '.cxx', '.c'))
            enclosing_file = any(str(frame[0].get('name', '')).lower().endswith(
                ('.cpp', '.cxx', '.c')) for frame in stacks[key])
            if source_file and not enclosing_file:
                source = event['name']
                last_sources[key] = source
                for frame in stacks[key]:
                    if frame[0].get('name') in ('FrontEndPass', 'BackEndPass'):
                        frame[1] = source
            if name == 'BackEndPass':
                source = last_sources.get(key, source)
            if not source and stacks[key]:
                source = stacks[key][-1][1]
            if not source:
                source = last_sources.get(key)
            stacks[key].append([event, source])
        elif phase == 'E':
            if not stacks[key]:
                unmatched += 1
                continue
            start, source = stacks[key].pop()
            seconds = max(0, event['ts'] - start['ts']) / 1_000_000
            record(start.get('name', ''), seconds, source, phases, units, headers, phase_counts, attributed)
        elif phase == 'X':
            source = stacks[key][-1][1] if stacks[key] else None
            record(event.get('name', ''), max(0, event.get('dur', 0)) / 1_000_000,
                   source, phases, units, headers, phase_counts, attributed)
    unmatched += sum(len(stack) for stack in stacks.values())
    ranked_units = [{'source': source, **dict(times), 'totalPhaseSeconds': sum(times.values())}
                    for source, times in units.items()]
    ranked_units.sort(key=lambda item: item['totalPhaseSeconds'], reverse=True)
    ranked_headers = [{'header': path, **values} for path, values in headers.items()]
    ranked_headers.sort(key=lambda item: item['inclusiveSeconds'], reverse=True)
    return {'eventCount': len(events), 'unmatchedBeginEndEvents': unmatched,
            'translationUnitCount': len(units), 'aggregatePhaseSeconds': dict(phases),
            'phaseCounts': dict(phase_counts),
            'unattributedPhaseSeconds': {name: max(0, seconds - attributed[name]) for name, seconds in phases.items()},
            'slowestTranslationUnits': ranked_units[:15], 'projectHeaders': ranked_headers[:20],
            'notes': ['Phase totals are aggregate work, not elapsed build time.',
                      'Header timings are inclusive and nested; they cannot be added together.',
                      'Environment variables, command lines and non-project header paths are omitted.']}


def record(name, seconds, source, phases, units, headers, phase_counts, attributed):
    if name in ('FrontEndPass', 'BackEndPass'):
        phases[name] += seconds
        phase_counts[name] += 1
        if source:
            units[project_path(source)][name] += seconds
            attributed[name] += seconds
    path = str(name).replace('\\', '/')
    if any('/' + marker in path for marker in ('WitnessCamera/', 'WitnessServer/', 'WitnessCommon/')) and path.lower().endswith(('.h', '.hpp')):
        entry = headers[project_path(path)]
        entry['inclusiveSeconds'] += seconds
        entry['visits'] += 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    args = parser.parse_args()
    with args.trace.open(encoding='utf-8') as stream:
        result = summarize(json.load(stream))
    print(json.dumps(result, indent=2))
    if result['unmatchedBeginEndEvents']:
        raise SystemExit(1)
