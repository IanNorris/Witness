"""Bounded exact per-task percentile collection over explicit snapshot cursors."""
import math

SCOPE = 'filterStageWallTimeExcludingQueueWaitAndDecode'


def windows(health):
    result = {}
    for camera in health['cameras']:
        uid = camera['cameraId']
        work = camera['processing']['workLatency']
        values = work.get('rawSamplesNS')
        epoch, total, first = work.get('epoch'), work.get('samplesSinceReset'), work.get('firstSequence')
        if (uid in result or work.get('scope') != SCOPE or work.get('capacity') != 1024 or
                not isinstance(values, list) or len(values) > 1024 or
                any(type(value) is not int or not 0 <= value <= 2**63 - 1 for value in values) or
                type(epoch) is not int or epoch < 1 or type(total) is not int or total < 0 or
                type(work.get('samples')) is not int or work['samples'] != len(values) or
                len(values) != min(total, 1024) or
                type(first) is not int or
                (not values and (first != 0 or total != 0)) or
                (values and (first < 1 or first + len(values) - 1 != total))):
            raise ValueError('Invalid or unavailable raw processing window')
        result[uid] = (epoch, total, first, values)
    if not result or len(result) > 16 or health.get('coverage', {}).get('rawWorkSamplesTruncated'):
        raise ValueError('Raw processing camera coverage incomplete')
    return result


class ProcessingSampleCollector:
    def __init__(self, initial, capacity_per_camera=250000):
        if type(capacity_per_camera) is not int or not 1 <= capacity_per_camera <= 250000:
            raise ValueError('Invalid collector capacity')
        self.capacity = capacity_per_camera
        self.state = {uid: {'epoch': epoch, 'start': total, 'last': total, 'values': [], 'reasons': set()}
                      for uid, (epoch, total, _, _) in windows(initial).items()}

    def raw_evidence(self):
        return {'scope': SCOPE, 'boundary': 'perCameraSnapshotSequenceCursors',
                'cameraReports': self.report(),
                'valuesNS': {str(uid): state['values'] for uid, state in self.state.items()}}

    def add(self, health):
        current = windows(health)
        if current.keys() != self.state.keys():
            raise ValueError('Processing camera set changed')
        for uid, (epoch, total, first, values) in current.items():
            state = self.state[uid]
            if epoch != state['epoch']:
                state['reasons'].add('epochChanged')
                state['epoch'], state['last'] = epoch, 0
            if total < state['last']:
                state['reasons'].add('counterRegressed')
                state['last'] = 0
            if total > state['last'] and first > state['last'] + 1:
                state['reasons'].add('ringOverrun')
            offset = max(0, state['last'] - first + 1) if values else 0
            added = values[offset:]
            available = self.capacity - len(state['values'])
            if len(added) > available:
                state['reasons'].add('collectorCapacityExceeded')
            state['values'].extend(added[:available])
            state['last'] = total

    def report(self):
        result = []
        for uid, state in sorted(self.state.items()):
            values = sorted(state['values'])
            reasons = state['reasons'] | ({'noCompletedTasks'} if not values else set())
            if len(values) != state['last'] - state['start']:
                reasons |= {'sampleCountMismatch'}
            complete = not reasons and len(values) == state['last'] - state['start']
            def percentile(percent):
                return values[math.ceil(len(values) * percent) - 1] / 1000000 if complete else None
            result.append({'cameraId': uid, 'scope': SCOPE,
                'boundary': 'perCameraSnapshotSequenceCursors',
                'complete': complete, 'unavailableReasons': sorted(reasons),
                'retainedSamples': len(values), 'collectorCapacity': self.capacity,
                'startSequence': state['start'], 'endSequence': state['last'],
                'endEpoch': state['epoch'],
                'p50Ms': percentile(.50), 'p95Ms': percentile(.95), 'p99Ms': percentile(.99)})
        return result
