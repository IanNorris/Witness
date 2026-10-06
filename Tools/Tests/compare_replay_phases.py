"""Compare qualified replay measurements; never certify production stability."""
import argparse
import json
import math
from pathlib import Path
from replay_profiles import validate_browser_layout_proof
from replay_source_identity import validate_identity


def finite(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def compare(off, on, off_recordings, on_recordings):
    missing, failures, metrics = [], [], {}
    for name in ('nativeBuildId', 'webBuildId', 'workload', 'mainStreamProfiles'):
        if not off.get(name) or not on.get(name):
            missing.append('missing ' + name)
        elif off[name] != on[name]:
            missing.append('mismatched ' + name)
    profiles = off.get('mainStreamProfiles') or []
    ids = [row.get('cameraId') for row in profiles]
    if len(ids) != 16 or any(type(uid) is not int or uid <= 0 for uid in ids) or len(set(ids)) != 16:
        missing.append('expected sixteen uniquely identified replay profiles')
    if {row.get('videoCodec') for row in profiles} != {'h264', 'hevc'}:
        missing.append('expected H.264 and HEVC profiles')
    identities = []
    for label, phase in (('off', off), ('on', on)):
        try:
            phase_ids = [row.get('cameraId') for row in phase.get('mainStreamProfiles') or []]
            identities.append(validate_identity(phase.get('replaySourceIdentity'), phase_ids))
        except (ValueError, TypeError, KeyError, AttributeError):
            missing.append(label + ' replay source boundary identity missing or invalid')
    if len(identities) == 2 and identities[0] != identities[1]:
        missing.append('mismatched whole-file replay sources')
    browser_viewports = []
    for label, phase, recordings, enabled in (('off', off, off_recordings, False),
                                              ('on', on, on_recordings, True)):
        workload = phase.get('workload') or {}
        if (workload.get('objectDetection') is not True or workload.get('provider') != 'cpu' or
                workload.get('maximumMotionInferenceFps') != 1 or
                workload.get('recording') != 'continuous DVR plus authenticated manual clips' or
                workload.get('strictRecordingInspection') is not True):
            missing.append(label + ' CPU inference and recording workload unqualified')
        modes = phase.get('observerModes', {})
        if any(modes.get(name) is not enabled for name in ('http', 'database', 'packet', 'mux', 'media')):
            missing.append(label + ' observer modes missing, mixed or incorrect')
        window = phase.get('measurementWindow') or {}
        # Revalidate retained independent DOM evidence against the phase start,
        # not the time this offline comparison happens. A readiness boolean is
        # not evidence of the actual viewport or visible player workload.
        try:
            evidence = phase['browserLayoutEvidence']
            proof = evidence['proof']
            viewport = proof['viewport']
            if (not isinstance(viewport, dict) or set(viewport) != {'width', 'height'} or
                    any(type(value) is not int or not 1 <= value <= 10000 for value in viewport.values()) or
                    type(evidence['validatedUnixMs']) is not int or
                    type(window['startedUnixMs']) is not int or
                    not 0 <= window['startedUnixMs'] - evidence['validatedUnixMs'] <= 60000):
                raise ValueError('Invalid browser proof validation boundary')
            validate_browser_layout_proof(proof, phase['mainStreamProfiles'], viewport,
                                          evidence['validatedUnixMs'])
            validate_browser_layout_proof(proof, phase['mainStreamProfiles'], viewport,
                                          window['startedUnixMs'])
            browser_viewports.append(viewport)
        except (ValueError, KeyError, TypeError, AttributeError, OverflowError):
            missing.append(label + ' independent browser layout evidence missing or invalid')
        coverage = phase.get('coverage') or {}
        # Startup DOM proof cannot qualify a later hidden or unobserved phase.
        # Require one entire same-session sampled window; never combine tabs.
        visibility = phase.get('sampledBrowserVisibility') or []
        if not any(isinstance(row, dict) and
                row.get('scope') == 'authenticatedSessionReceiptsWithinMeasurementWindowNotContinuousVisibilityProof' and
                type(row.get('retainedRows')) is int and row['retainedRows'] >= 2 and
                all(type(row.get(name)) is int and row[name] == 0 for name in
                    ('hiddenRows', 'unknownVisibilityRows', 'incompletePlayerRows')) and
                all(finite(row.get(name)) and row[name] <= 30 for name in
                    ('startOffsetSeconds', 'endGapSeconds', 'maximumReceiptGapSeconds'))
                for row in visibility):
            missing.append(label + ' sampled visible browser coverage incomplete or hidden')
        if (not finite(window.get('observedDurationSeconds')) or window['observedDurationSeconds'] < 1800 or
                not finite(coverage.get('maximumGapSeconds')) or coverage['maximumGapSeconds'] > 10 or
                phase.get('cameraCount') != 16 or phase.get('browserReadyBeforePhase') is not True):
            missing.append(label + ' full-load phase coverage incomplete')
        rows = recordings.get('recordingInspection', [])
        if (recordings.get('inspectionComplete') is not True or recordings.get('passed') is not True or
                type(recordings.get('expectedOutputs')) is not int or recordings['expectedOutputs'] <= 0 or
                recordings.get('completedOutputs') != recordings['expectedOutputs'] or
                len(rows) != recordings['expectedOutputs'] or any(row.get('passed') is not True for row in rows)):
            missing.append(label + ' recording inspection incomplete or failed')
        if any({row.get('cameraId') for row in rows if row.get('kind') == kind} != set(ids)
               for kind in ('clip', 'continuous')):
            missing.append(label + ' clip/DVR inspection camera coverage incomplete')
        audio_ids = {p.get('cameraId') for p in phase.get('mainStreamProfiles') or []
                     if p.get('audioCodec')}
        for row in rows:
            if row.get('cameraId') not in audio_ids:
                continue
            audio = row.get('audio') or {}
            tracks = audio.get('tracks')
            if (audio.get('inspectionMode') != 'boundedStreamingFullAudio' or
                    audio.get('required') is not True or audio.get('audioPresent') is not True or
                    audio.get('passed') is not True or not isinstance(tracks, list) or not tracks or
                    any(not isinstance(t, dict) or t.get('passed') is not True for t in tracks)):
                missing.append(label + ' expected recording audio inspection missing or failed')
                break
        timing = phase.get('summaryHttpTiming') or {}
        p95 = (phase.get('summaryHttpWallMs') or {}).get('p95')
        if (timing.get('scope') != 'authenticatedHealthWithoutRawWorkSamplesRequestAndResponseBodyOnly' or
                timing.get('complete') is not True or timing.get('availableSamples', 0) != phase.get('sampleCount') or
                timing.get('totalSamples', 0) != phase.get('sampleCount') or not finite(p95)):
            missing.append(label + ' ordinary-summary timing unavailable')
        elif p95 >= 100:
            failures.append(label + ' summary HTTP P95 is not below 100ms')
        metrics[label + 'SummaryHttpP95Ms'] = p95
    if len(browser_viewports) == 2 and browser_viewports[0] != browser_viewports[1]:
        missing.append('mismatched independently observed browser viewport')
    for percentile in ('p50', 'p95'):
        left = off.get('processCpuHostCapacityPercent', {}).get(percentile)
        right = on.get('processCpuHostCapacityPercent', {}).get(percentile)
        if not finite(left) or not finite(right):
            missing.append('CPU ' + percentile + ' unavailable')
        else:
            metrics['cpu' + percentile.upper() + 'IncreaseHostPercentagePoints'] = right - left
            if right - left >= 1:
                failures.append('CPU ' + percentile + ' increment is not below one host percentage point')
    tasks = []
    distributions = []
    for label, phase in (('off', off), ('on', on)):
        rows = phase.get('processingTaskDistributions', [])
        task_ids = [row.get('cameraId') for row in rows]
        if len(task_ids) != 16 or len(set(task_ids)) != 16 or set(task_ids) != set(ids):
            missing.append(label + ' exact task distribution camera coverage incomplete')
        distributions.append({row['cameraId']: row for row in rows if 'cameraId' in row})
    for camera_id in ids:
        pair = [distribution.get(camera_id, {}) for distribution in distributions]
        if any(row.get('complete') is not True or row.get('unavailableReasons') != [] or
               row.get('scope') != 'filterStageWallTimeExcludingQueueWaitAndDecode' or
               row.get('boundary') != 'perCameraSnapshotSequenceCursors' or
               not finite(row.get('p95Ms')) or row['p95Ms'] <= 0 for row in pair):
            missing.append('camera ' + str(camera_id) + ' exact task P95 unavailable')
            continue
        increase = (pair[1]['p95Ms'] / pair[0]['p95Ms'] - 1) * 100
        tasks.append({'cameraId': camera_id, 'offP95Ms': pair[0]['p95Ms'],
                      'onP95Ms': pair[1]['p95Ms'], 'increasePercent': increase})
        if increase >= 5:
            failures.append('camera ' + str(camera_id) + ' task P95 increment is not below 5%')
    return {'scope': 'Same-candidate declared replay numeric thresholds only',
            'status': 'incomplete' if missing else 'failed' if failures else 'numericThresholdsMet',
            'incompleteReasons': missing, 'thresholdFailures': failures, 'metrics': metrics,
            'processing': tasks,
            'limits': ['Not production stability or sixteen independent physical cameras.',
                       'Playback, hidden/resume and independent browser-profile gates require separate evidence.',
                       'DOM layout proof qualifies startup only, not continuous visibility or unchanged layout.',
                       'Visible heartbeats qualify sampled coverage only, not visibility between receipts or occlusion.',
                       'CPU P50 and P95 are both compared; task distributions exclude decode and queue wait.']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('off', 'on', 'off_recordings', 'on_recordings'):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    result = compare(*(json.loads(getattr(args, name).read_text(encoding='utf8'))
                       for name in ('off', 'on', 'off_recordings', 'on_recordings')))
    print(json.dumps(result, indent=2))
    return 0 if result['status'] == 'numericThresholdsMet' else 1


if __name__ == '__main__':
    raise SystemExit(main())
