import json
from types import SimpleNamespace
import unittest
from replay_profiles import qualify_profiles, validate_live_profiles


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
