import json
from types import SimpleNamespace
import unittest
from replay_profiles import qualify_profiles, validate_live_profiles, validate_browser_layout_proof
from copy import deepcopy
from datetime import datetime, timezone


class BrowserLayoutProofTests(unittest.TestCase):
    def setUp(self):
        self.now = 1791210000000
        self.profiles = [{'width': 1280, 'height': 720}]
        self.viewport = {'width': 2172, 'height': 1550}
        self.proof = {'capturedUtc': datetime.fromtimestamp(self.now / 1000, timezone.utc).isoformat(),
                      'viewport': dict(self.viewport), 'visibility': 'visible', 'browserPresented': True,
                      'fullscreen': True,
                      'activityVisible': False, 'players': [{'width': 1280, 'height': 720,
                          'insideViewport': True, 'paused': False, 'readyState': 4,
                          'error': None, 'currentTime': 10.0}]}

    def test_matching_recent_playback_is_accepted(self):
        validate_browser_layout_proof(self.proof, self.profiles, self.viewport, self.now)

    def test_viewport_visibility_layout_age_and_membership_fail_closed(self):
        for key, value in [('viewport', {'width': 1280, 'height': 720}),
                           ('visibility', 'hidden'), ('browserPresented', False),
                           ('browserPresented', None), ('browserPresented', 1), ('fullscreen', False),
                           ('activityVisible', True), ('players', []),
                           ('capturedUtc', '2020-01-01T00:00:00Z'),
                           ('capturedUtc', '2030-01-01T00:00:00Z'),
                           ('capturedUtc', '2026-10-05T00:00:00')]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                proof = deepcopy(self.proof); proof[key] = value
                validate_browser_layout_proof(proof, self.profiles, self.viewport, self.now)

    def test_dom_visibility_alone_cannot_qualify_presented_playback(self):
        proof = deepcopy(self.proof)
        del proof['browserPresented']
        with self.assertRaisesRegex(ValueError, 'presented'):
            validate_browser_layout_proof(proof, self.profiles, self.viewport, self.now)

    def test_player_state_must_be_observed_not_assumed(self):
        for key, value in [('insideViewport', False), ('paused', True), ('readyState', 2),
                           ('readyState', True), ('width', 256), ('height', 144),
                           ('currentTime', 0), ('currentTime', float('nan')),
                           ('currentTime', True), ('error', {'code': 3})]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                proof = deepcopy(self.proof); proof['players'][0][key] = value
                validate_browser_layout_proof(proof, self.profiles, self.viewport, self.now)


class ReplayProfileTests(unittest.TestCase):
    profile = {'videoCodec': 'h264', 'width': 1280, 'height': 720, 'audioCodec': ''}

    def probe(self, *args, **kwargs):
        self.calls += 1
        return SimpleNamespace(returncode=0, stderr='', stdout=json.dumps({'streams': [
            {'codec_type': 'video', 'codec_name': 'h264', 'width': 1280, 'height': 720}]}))

    def setUp(self):
        self.calls = 0

    def test_repeated_sources_are_probed_once_and_profiles_copied(self):
        result = qualify_profiles(['fixture.mp4'] * 4, [self.profile] * 4, self.probe)
        self.assertEqual(self.calls, 1)
        self.assertEqual(result, [self.profile] * 4)
        result[0]['width'] = 1
        self.assertEqual(result[1]['width'], 1280)

    def test_tiny_fixture_cannot_qualify_as_representative(self):
        def tiny_probe(*args, **kwargs):
            return SimpleNamespace(returncode=0, stderr='', stdout=json.dumps({'streams': [
                {'codec_type': 'video', 'codec_name': 'h264', 'width': 256, 'height': 144}]}))
        with self.assertRaisesRegex(ValueError, 'mismatch'):
            qualify_profiles(['fixture.mp4'], [self.profile], tiny_probe)

    def test_shape_boolean_dimensions_and_failed_probe_rejected(self):
        for expected in (None, [], [{**self.profile, 'width': True}], [{**self.profile, 'extra': 1}]):
            with self.assertRaises(ValueError):
                qualify_profiles(['fixture.mp4'], expected, self.probe)
        for result in (SimpleNamespace(returncode=1, stderr='', stdout=''),
                       SimpleNamespace(returncode=0, stderr='probe error', stdout='{}')):
            with self.assertRaises(ValueError):
                qualify_profiles(['fixture.mp4'], [self.profile], lambda *a, **k: result)

    def test_audio_presence_and_codec_are_part_of_workload(self):
        with self.assertRaises(ValueError):
            qualify_profiles(['fixture.mp4'], [{**self.profile, 'audioCodec': 'aac'}], self.probe)
        with self.assertRaises(ValueError):
            qualify_profiles(['fixture.mp4'], [{**self.profile, 'videoCodec': 'hevc'}], self.probe)

    def test_actual_live_tier_resolution_and_membership_required(self):
        camera = {'cameraId': 1, 'streams': [{'tier': 'main', 'available': True, **self.profile}]}
        validate_live_profiles([camera], [self.profile])
        for invalid in ([], [camera, camera], [{**camera, 'cameraId': True}],
                        [{**camera, 'streams': [{'tier': 'preview', 'available': True, **self.profile}]}],
                        [{**camera, 'streams': [{'tier': 'main', 'available': True, **self.profile, 'width': 256}]}]):
            with self.assertRaises(ValueError):
                validate_live_profiles(invalid, [self.profile])


if __name__ == '__main__':
    unittest.main()
