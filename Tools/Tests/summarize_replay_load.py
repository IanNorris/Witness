"""Summarize replay measurements without converting them into a stability claim."""
import argparse
import json
import math
from pathlib import Path


def quantile(values, percentile):
    values = sorted(value for value in values
                    if type(value) in (int, float) and math.isfinite(value))
    return values[max(0, math.ceil(len(values) * percentile) - 1)] if values else None


def sampled_playback_health(timeline):
    """Receipt-weighted observations, not time-weighted or continuous playback proof."""
    latencies = [p['latencyMs'] for p in timeline if
                 type(p.get('latencyMs')) in (int, float) and
                 math.isfinite(p['latencyMs']) and p['latencyMs'] >= 0]
    readiness = [p['readyState'] for p in timeline if
                 type(p.get('readyState')) is int and 0 <= p['readyState'] <= 4]
    return {'scope': 'eligibleVisibleSessionReceiptSamplesNotContinuousOrTimeWeightedProof',
            'retainedSamples': len(timeline),
            'latencyMs': dict(
                {name: quantile(latencies, percentile) for name, percentile in
                 (('p50', .5), ('p95', .95), ('p99', .99), ('maximum', 1))},
                availableSamples=len(latencies), unavailableSamples=len(timeline) - len(latencies)),
            'readyStateCounts': {str(state): readiness.count(state) for state in range(5)},
            'unknownReadyStateSamples': len(timeline) - len(readiness)}


def buffer_position_observations(timeline):
    """Deduplicated retained snapshots only; never infer a gap from final latency."""
    seen = {}
    def finite(value):
        return type(value) in (int, float) and math.isfinite(value)
    for player in timeline:
        samples = player.get('driftSamples', [])
        if not isinstance(samples, list):
            continue
        for sample in samples:
            if not isinstance(sample, dict) or not isinstance(sample.get('t'), str):
                continue
            generation = sample.get('generation')
            if generation is not None and not finite(generation):
                continue
            key = generation, sample['t']
            position, gap = 'unknown', None
            if sample.get('playheadBuffered') is True:
                position = 'buffered'
            elif sample.get('playheadBuffered') is False:
                value = sample.get('playheadGapMs')
                if finite(value) and value > 0 and finite(sample.get('nextBufferedStartSeconds')):
                    position, gap = 'gap', value
                elif (finite(sample.get('bufferedRangeCount')) and sample['bufferedRangeCount'] >= 0 and
                      sample.get('headroomInCurrentRangeMs') == 0 and
                      'nextBufferedStartSeconds' not in sample and 'playheadGapMs' not in sample):
                    position = 'tailOrEmpty'
            current = position, gap
            if key in seen and seen[key] != current:
                seen[key] = ('unknown', None)  # Conflicting repeats cannot qualify a known state.
            else:
                seen[key] = current
    gaps = [gap for position, gap in seen.values() if position == 'gap']
    return {'observedUniqueSamples': len(seen),
        'bufferedSamples': sum(position == 'buffered' for position, _ in seen.values()),
        'gapSamples': len(gaps),
        'tailOrEmptySamples': sum(position == 'tailOrEmpty' for position, _ in seen.values()),
        'unknownPositionSamples': sum(position == 'unknown' for position, _ in seen.values()),
        'maximumObservedGapMs': max(gaps) if gaps else None,
        'scope': 'retainedMediaElementPositionSnapshotsNotContinuousPlaybackOrSeparateTrackReadiness'}


def scheduling_observations(timeline):
    """Visibility is not proof that a browser schedules its media watchdog."""
    seen = {}
    for player in timeline:
        samples = player.get('driftSamples', [])
        if not isinstance(samples, list):
            continue
        for sample in samples:
            if not isinstance(sample, dict) or not isinstance(sample.get('t'), str):
                continue
            generation = sample.get('generation')
            if type(generation) is not int or generation < 0:
                continue
            key = generation, sample['t']
            value = sample.get('schedulingDelayMs')
            value = value if type(value) in (int, float) and math.isfinite(value) and value >= 0 else None
            if key in seen and seen[key] != value:
                seen[key] = None
            else:
                seen[key] = value
    measured = [value for value in seen.values() if value is not None]
    return {'observedUniqueSamples': len(seen),
        'unknownSamples': len(seen) - len(measured),
        'maximumObservedCallbackIntervalMs': max(measured) if measured else None,
        'callbackIntervalsOver5Seconds': sum(value > 5000 for value in measured),
        'scope': 'deduplicatedRetainedWatchdogIntervalsNotContinuousSchedulingProof'}


def presentation_observations(timeline):
    """Report retained frame callbacks separately from the advancing media clock."""
    seen = {}
    def finite(value):
        return type(value) in (int, float) and math.isfinite(value) and value >= 0
    for player in timeline:
        samples = player.get('driftSamples', [])
        if not isinstance(samples, list):
            continue
        for sample in samples:
            if not isinstance(sample, dict) or not isinstance(sample.get('t'), str):
                continue
            generation = sample.get('generation')
            if type(generation) is not int or generation < 0:
                continue  # Unknown generations cannot establish callback continuity.
            key = generation, sample['t']
            visibility = sample.get('presentationVisible')
            if type(visibility) is not bool:
                visibility = None
            frames, presented, clock = (sample.get(field) for field in
                ('presentedFrames', 'presentedMediaTime', 'currentTime'))
            measured = (type(frames) is int and frames > 0 and
                        finite(presented) and finite(clock))
            stall = sample.get('presentationStallMs')
            value = (visibility, frames if measured else None,
                     presented if measured else None, clock if measured else None,
                     stall if finite(stall) else None)
            if key in seen and seen[key] != value:
                seen[key] = None  # Conflicts stay unknown, including later repeats.
            else:
                seen[key] = value
    values = [(key, value) for key, value in sorted(seen.items()) if value is not None]
    measurable = [(key, value) for key, value in values if value[1] is not None]
    progressing, nonadvancing, resets = 0, 0, 0
    visible_progressing, visible_nonadvancing, unmeasurable = 0, 0, 0
    # Do not bridge unavailable/conflicting snapshots. Frames advancing on
    # either side of missing evidence cannot qualify that intervening pair.
    ordered = sorted(seen.items())
    for (left_key, left), (right_key, right) in zip(ordered, ordered[1:]):
        if left_key[0] != right_key[0]:
            continue
        if left is None or right is None or left[1] is None or right[1] is None:
            unmeasurable += 1
            continue
        if right[1] < left[1] or right[2] < left[2]:
            resets += 1
        elif right[1] > left[1] and right[2] > left[2]:
            progressing += 1
            visible_progressing += left[0] is True and right[0] is True
        else:
            nonadvancing += 1
            visible_nonadvancing += left[0] is True and right[0] is True
    lags = [(value[3] - value[2]) * 1000 for _, value in measurable]
    stalls = [value[4] for _, value in values if value[4] is not None]
    return {'observedUniqueSamples': len(seen),
        'measurableSamples': len(measurable),
        'visibleSamples': sum(value[0] is True for _, value in values),
        'notVisibleSamples': sum(value[0] is False for _, value in values),
        'unknownVisibilitySamples': len(seen) - sum(value[0] is not None for _, value in values),
        'progressingSameGenerationPairs': progressing,
        'nonadvancingSameGenerationPairs': nonadvancing,
        'visibleProgressingSameGenerationPairs': visible_progressing,
        'visibleNonadvancingSameGenerationPairs': visible_nonadvancing,
        'unmeasurableSameGenerationPairs': unmeasurable,
        'presentationResetPairs': resets,
        'maximumObservedClockAheadOfPresentationMs': max(lags) if lags else None,
        'maximumObservedRunnableStallMs': max(stalls) if stalls else None,
        'scope': 'deduplicatedRetainedCallbacksNotContinuousPresentationOrOcclusionProof'}


def sampled_visibility_coverage(rows, window, player_ids):
    """Keep hidden/unknown receipts: filtering them out would invent visibility."""
    hidden = sum(row['snapshot'].get('visibilityState') == 'hidden' for row in rows)
    unknown = sum(row['snapshot'].get('visibilityState') not in ('visible', 'hidden') for row in rows)
    incomplete = 0
    for row in rows:
        snapshot = row['snapshot']
        players = snapshot.get('players')
        if (snapshot.get('path') != '/' or not isinstance(players, list) or len(players) != len(player_ids) or
                any(not isinstance(player, dict) for player in players) or
                {player.get('id') for player in players} != player_ids or snapshot.get('playersTruncated')):
            incomplete += 1
    gaps = [(right['receivedUnixMs'] - left['receivedUnixMs']) / 1000
            for left, right in zip(rows, rows[1:])]
    return {'scope': 'authenticatedSessionReceiptsWithinMeasurementWindowNotContinuousVisibilityProof',
        'retainedRows': len(rows), 'hiddenRows': hidden, 'unknownVisibilityRows': unknown,
        'incompletePlayerRows': incomplete,
        'startOffsetSeconds': (rows[0]['receivedUnixMs'] - window['startedUnixMs']) / 1000 if window else None,
        'endGapSeconds': (window['completedUnixMs'] - rows[-1]['receivedUnixMs']) / 1000 if window else None,
        'maximumReceiptGapSeconds': max(gaps) if gaps else None}


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
    summary_scope = 'authenticatedHealthWithoutRawWorkSamplesRequestAndResponseBodyOnly'
    summary_requests = [sample.get('summaryHttpMs') for sample in samples
                        if sample.get('summaryHttpScope') == summary_scope and
                        type(sample.get('summaryHttpMs')) in (int, float) and
                        math.isfinite(sample['summaryHttpMs']) and sample['summaryHttpMs'] >= 0]
    summary_complete = len(summary_requests) == len(samples)
    camera_ids = sorted(camera['cameraId'] for camera in samples[0]['cameras'])
    if any(sorted(camera['cameraId'] for camera in sample['cameras']) != camera_ids
           for sample in samples):
        raise ValueError('Camera set changed during measurement')
    # Preserve observed mode/profile provenance, not just caller-requested flags.
    # Missing or mixed evidence stays unavailable rather than qualifying a phase.
    modes = {name: [] for name in ('http', 'database', 'packet', 'mux', 'media')}
    profiles = {camera_id: [] for camera_id in camera_ids}
    for sample in samples:
        for name in ('http', 'database'):
            modes[name].append(sample['server'].get(name, {}).get('tracingEnabled'))
        for camera in sample['cameras']:
            main = [stream for stream in camera.get('streams', []) if stream.get('tier') == 'main']
            stream = main[0] if len(main) == 1 else {}
            for name in ('packet', 'mux', 'media'):
                modes[name].append(stream.get(name + 'TraceEnabled'))
            profile = {name: stream.get(name) for name in ('videoCodec', 'width', 'height', 'audioCodec')}
            valid = (isinstance(profile['videoCodec'], str) and bool(profile['videoCodec']) and
                     isinstance(profile['audioCodec'], str) and
                     all(type(profile[name]) is int and profile[name] > 0 for name in ('width', 'height')))
            profiles[camera['cameraId']].append(profile if valid else None)
    observer_modes = {name: values[0] if values and all(type(value) is bool and value == values[0]
                         for value in values) else None for name, values in modes.items()}
    main_profiles = [{'cameraId': camera_id, **values[0]} for camera_id, values in profiles.items()
                     if values[0] is not None and all(value == values[0] for value in values)]
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
    sessions, visibility_sessions = {}, {}
    for row in sorted(history.get('history', []), key=lambda row: row.get('receivedUnixMs', 0)):
        received = row.get('receivedUnixMs')
        if type(received) is not int:
            continue
        if window is not None and not window['startedUnixMs'] <= received <= window['completedUnixMs']:
            continue
        snapshot = row.get('snapshot', {})
        if (row.get('userUid') == 1 and row.get('username') == 'fixture-admin' and
                snapshot.get('buildHash') == build and
                all(isinstance(row.get(name), str) and row[name] for name in ('browserId', 'sessionId'))):
            visibility_sessions.setdefault((row['browserId'], row['sessionId']), []).append(row)
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
                'sampledPlaybackHealth': sampled_playback_health(timeline),
                'sampledBufferPosition': buffer_position_observations(timeline),
                'sampledPresentation': presentation_observations(timeline),
                'sampledScheduling': scheduling_observations(timeline),
                'finalLastEventType': last[player_id].get('lastEventType')})
        first_received = rows[0]['receivedUnixMs']
        last_received = rows[-1]['receivedUnixMs']
        receipt_gaps = [(right['receivedUnixMs'] - left['receivedUnixMs']) / 1000
                        for left, right in zip(rows, rows[1:])]
        boundary_observations = None
        if window:
            start_gap = (first_received - window['startedUnixMs']) / 1000
            end_gap = (window['completedUnixMs'] - last_received) / 1000
            boundary_observations = {
                'startOffsetSeconds': start_gap, 'endGapSeconds': end_gap,
                'startObservedWithin30Seconds': start_gap <= 30,
                'endObservedWithin30Seconds': end_gap <= 30,
                'scope': 'retainedEligibleSnapshotBoundariesNotContinuousPlaybackProof'}
        browser.append({'retainedRows': len(rows),
            'retainedSpanSeconds': (rows[-1]['receivedUnixMs'] - rows[0]['receivedUnixMs']) / 1000,
            'firstReceivedUnixMs': first_received, 'lastReceivedUnixMs': last_received,
            'maximumReceiptGapSeconds': max(receipt_gaps) if receipt_gaps else None,
            'measurementBoundaryObservations': boundary_observations,
            'players': player_reports})
    return {'nativeBuildId': native, 'webBuildId': build,
        'replaySourceIdentity': replay.get('replaySourceIdentity'),
        'observerModes': observer_modes,
        'mainStreamProfiles': main_profiles if len(main_profiles) == len(camera_ids) else None,
        'browserReadyBeforePhase': replay.get('browserReadyBeforePhase') is True,
        'browserLayoutEvidence': replay.get('browserLayoutEvidence'),
        'cameraCount': len(camera_ids), 'sampleCount': len(samples),
        'coverage': replay.get('coverage'), 'workload': replay.get('workload'),
        'measurementWindow': window,
        'browserHistoryScope': 'serverReceiptWithinMeasurementWindow' if window else 'legacyUnanchoredHistory',
        'processCpuHostCapacityPercent': {'p50': quantile(cpu, .5), 'p95': quantile(cpu, .95)},
        'hostCpuPercent': {'p50': quantile(host, .5), 'p95': quantile(host, .95)},
        'healthCollectionWallMs': {'p50': quantile(requests, .5), 'p95': quantile(requests, .95),
                                  'p99': quantile(requests, .99),
                                  'scope': 'perSampleHealthHttpScopeOrLegacyCollectionIncludingParsing'},
        'summaryHttpTiming': {'scope': summary_scope, 'availableSamples': len(summary_requests),
                              'totalSamples': len(samples), 'complete': summary_complete},
        'summaryHttpWallMs': {name: quantile(summary_requests, percentile) if summary_complete else None
                              for name, percentile in (('p50', .5), ('p95', .95), ('p99', .99))},
        'processing': processing, 'browserSessions': browser,
        'sampledBrowserVisibility': [sampled_visibility_coverage(rows, window,
            {f'{uid}_mse' for uid in camera_ids}) for rows in visibility_sessions.values()],
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
