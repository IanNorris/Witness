"""Summarize replay measurements without converting them into a stability claim."""
import argparse
import json
import math
from pathlib import Path


def quantile(values, percentile):
    values = sorted(value for value in values
                    if type(value) in (int, float) and math.isfinite(value))
    return values[max(0, math.ceil(len(values) * percentile) - 1)] if values else None


def summarize(replay, history):
    samples = replay.get('samples', [])
    if not samples:
        raise ValueError('No replay samples')
    window = replay.get('measurementWindow')
    if window is not None:
        if (not isinstance(window, dict) or type(window.get('startedUnixMs')) is not int or
                type(window.get('completedUnixMs')) is not int or
                window['startedUnixMs'] >= window['completedUnixMs']):
            raise ValueError('Invalid measurement window')
    build = samples[0]['server']['webBuildHash']
    native = replay['nativeBuildId']
    if any(sample['server'].get('nativeBuildHash') != native or
           sample['server'].get('webBuildHash') != build for sample in samples):
        raise ValueError('Mixed build identities')
    cpu = [sample['server']['host'].get('processCpuPercent') for sample in samples]
    host = [sample['server']['host'].get('cpuPercent') for sample in samples]
    requests = [sample.get('httpMs') for sample in samples]
    camera_ids = sorted(camera['cameraId'] for camera in samples[0]['cameras'])
    if any(sorted(camera['cameraId'] for camera in sample['cameras']) != camera_ids
           for sample in samples):
        raise ValueError('Camera set changed during measurement')
    processing = []
    for camera_id in camera_ids:
        values = [next(camera for camera in sample['cameras'] if camera['cameraId'] == camera_id)
                  for sample in samples]
        rolling = [camera['processing']['workLatency'].get('p95Ms') for camera in values]
        processing.append({'cameraId': camera_id,
            'completedFrameDelta': values[-1]['processing']['completedFrames'] -
                                   values[0]['processing']['completedFrames'],
            'medianRollingFilterP95Ms': quantile(rolling, .5),
            'maximumRollingFilterP95Ms': quantile(rolling, 1),
            'finalRollingFilterP95Ms': rolling[-1],
            'filterScope': values[-1]['processing']['workLatency'].get('scope')})
    # Exclude synthetic fixture setup and other tabs/profiles/builds. Keep
    # sessions separate: counter resets/replacements must not look like recovery.
    sessions = {}
    for row in sorted(history.get('history', []), key=lambda row: row.get('receivedUnixMs', 0)):
        received = row.get('receivedUnixMs')
        if type(received) is not int:
            continue
        if window is not None and not window['startedUnixMs'] <= received <= window['completedUnixMs']:
            continue
        snapshot = row.get('snapshot', {})
        players = snapshot.get('players', [])
        if (not isinstance(players, list) or len(players) != len(camera_ids) or
                any(not isinstance(player, dict) for player in players)):
            continue
        if (row.get('userUid') != 1 or row.get('username') != 'fixture-admin' or
                snapshot.get('buildHash') != build or snapshot.get('path') != '/' or
                snapshot.get('visibilityState') != 'visible' or snapshot.get('playersTruncated') or
                {player.get('id') for player in players} != {f'{uid}_mse' for uid in camera_ids}):
            continue
        key = (row.get('browserId'), row.get('sessionId'))
        sessions.setdefault(key, []).append(row)
    browser = []
    for rows in sessions.values():
        first = {player['id']: player for player in rows[0]['snapshot']['players']}
        last = {player['id']: player for player in rows[-1]['snapshot']['players']}
        counters = ('restartCount', 'stallCount', 'errorCount', 'totalVideoFrames', 'droppedVideoFrames')
        player_reports = []
        for player_id in sorted(first):
            deltas = {name: (last[player_id][name] - first[player_id][name])
                      if type(first[player_id].get(name)) is int and
                         type(last[player_id].get(name)) is int else None for name in counters}
            timeline = [next(player for player in row['snapshot']['players']
                             if player['id'] == player_id) for row in rows]
            reset = any(type(previous.get(name)) is int and type(current.get(name)) is int and
                        current[name] < previous[name]
                        for previous, current in zip(timeline, timeline[1:]) for name in counters)
            player_reports.append({'playerId': player_id, 'counterDeltas': deltas,
                'counterResetObserved': reset,
                'finalReadyState': last[player_id].get('readyState'),
                'finalLatencyMs': last[player_id].get('latencyMs'),
                'finalLastEventType': last[player_id].get('lastEventType')})
        browser.append({'retainedRows': len(rows),
            'retainedSpanSeconds': (rows[-1]['receivedUnixMs'] - rows[0]['receivedUnixMs']) / 1000,
            'players': player_reports})
    return {'nativeBuildId': native, 'webBuildId': build,
        'cameraCount': len(camera_ids), 'sampleCount': len(samples),
        'coverage': replay.get('coverage'), 'workload': replay.get('workload'),
        'measurementWindow': window,
        'browserHistoryScope': 'serverReceiptWithinMeasurementWindow' if window else 'legacyUnanchoredHistory',
        'processCpuHostCapacityPercent': {'p50': quantile(cpu, .5), 'p95': quantile(cpu, .95)},
        'hostCpuPercent': {'p50': quantile(host, .5), 'p95': quantile(host, .95)},
        'summaryHttpWallMs': {'p50': quantile(requests, .5), 'p95': quantile(requests, .95),
                              'p99': quantile(requests, .99)},
        'processing': processing, 'browserSessions': browser,
        'processingTaskDistributions': replay.get('processingTaskDistributions', []),
        'limits': ['Rolling filter P95 summaries are not the full-phase processing P95.',
                   'Browser retained spans are not exact measurement-window coverage.',
                   'Counter deltas exclude startup and cannot prove absence of a hidden reset.',
                   'Replay sources may be duplicates, not independent physical cameras.',
                   'Disabled selective observers do not disable functional integrity/cadence evidence.',
                   'These descriptive statistics are not a passed production or overhead gate.']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('replay', type=Path)
    parser.add_argument('history', type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(json.loads(args.replay.read_text(encoding='utf8')),
                               json.loads(args.history.read_text(encoding='utf8'))), indent=2))


if __name__ == '__main__':
    main()
