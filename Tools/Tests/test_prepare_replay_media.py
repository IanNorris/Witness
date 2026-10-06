import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
from types import SimpleNamespace
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
SPEC = importlib.util.spec_from_file_location('prepare_replay_media', Path(__file__).with_name('prepare_replay_media.py'))
MEDIA = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MEDIA)
ARTIFACTS = Path(__file__).resolve().parents[2] / 'artifacts'


class ReplayPreparationTests(unittest.TestCase):
    def test_exact_rational_codec_cadence_is_supported_without_float_rounding(self):
        self.assertEqual(MEDIA.exact_rate('50/3'), MEDIA.Fraction(50, 3))
        self.assertEqual(MEDIA.exact_rate('20'), MEDIA.Fraction(20))
        for value in (True, 20.0, '0/1', '61', '1/0', 'not-a-rate', '10001/10000'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                MEDIA.exact_rate(value)

    def test_no_b_frames_and_supported_codec_are_required(self):
        valid = {'codec_name': 'hevc', 'has_b_frames': 0, 'width': 3840, 'height': 2160}
        MEDIA.validate_stream(valid, 25)
        for bad in ({**valid, 'has_b_frames': 1}, {key: value for key, value in valid.items() if key != 'has_b_frames'},
                    {**valid, 'codec_name': 'mjpeg'}, {**valid, 'width': 0}):
            with self.assertRaises(ValueError): MEDIA.validate_stream(bad, 25)
        for fps in (0, 61):
            with self.assertRaises(ValueError): MEDIA.validate_stream(valid, fps)

    def test_existing_output_is_rejected_before_probe(self):
        with tempfile.TemporaryDirectory(dir=ARTIFACTS) as directory:
            source = Path(directory) / 'source.mp4'; source.write_bytes(b'original')
            output = Path(directory) / 'output.mp4'; output.write_bytes(b'keep')
            with self.assertRaises(ValueError): MEDIA.prepare(source, output, 25)
            self.assertEqual(source.read_bytes(), b'original')
            self.assertEqual(output.read_bytes(), b'keep')

    def test_failed_validation_never_publishes_output(self):
        for failure in ('decode', 'payload', 'cadence'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory(dir=ARTIFACTS) as directory:
                source = Path(directory) / 'source.mp4'; source.write_bytes(b'original')
                output = Path(directory) / 'output.mp4'
                streams = [
                    {'streams': [{'codec_name': 'hevc', 'has_b_frames': 0, 'width': 1920, 'height': 1080}]},
                    {'streams': [{'avg_frame_rate': '24/1' if failure == 'cadence' else '25/1',
                                  'r_frame_rate': '25/1'}]},
                ]
                hashes = [['SHA256:a', 'SHA256:b'],
                          ['SHA256:a', 'SHA256:c' if failure == 'payload' else 'SHA256:b']]
                with patch.object(MEDIA, 'probe', side_effect=streams), \
                     patch.object(MEDIA, 'payload_hashes', side_effect=hashes), \
                     patch.object(MEDIA, 'inspect', return_value={'passed': failure != 'decode', 'frames': {'samples': 2}}), \
                     patch.object(MEDIA.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stderr='')):
                    with self.assertRaises(ValueError): MEDIA.prepare(source, output, 25)
                self.assertFalse(output.exists())
                self.assertEqual(source.read_bytes(), b'original')
                self.assertEqual(sorted(path.name for path in Path(directory).iterdir()), ['source.mp4'])

    def test_output_creation_race_does_not_replace_existing_file(self):
        with tempfile.TemporaryDirectory(dir=ARTIFACTS) as directory:
            source = Path(directory) / 'source.mp4'; source.write_bytes(b'original')
            output = Path(directory) / 'output.mp4'
            def remux(command, **kwargs):
                Path(command[-1]).write_bytes(b'generated fixture')
                output.write_bytes(b'concurrent owner')
                return SimpleNamespace(returncode=0, stderr='')
            with patch.object(MEDIA, 'probe', side_effect=[
                    {'streams': [{'codec_name': 'hevc', 'has_b_frames': 0, 'width': 1920, 'height': 1080}]},
                    {'streams': [{'avg_frame_rate': '25/1', 'r_frame_rate': '25/1'}]}]), \
                 patch.object(MEDIA, 'payload_hashes', return_value=['SHA256:a', 'SHA256:b']), \
                 patch.object(MEDIA, 'inspect', return_value={'passed': True, 'frames': {'samples': 2}}), \
                 patch.object(MEDIA.subprocess, 'run', side_effect=remux):
                with self.assertRaises(FileExistsError): MEDIA.prepare(source, output, 25)
            self.assertEqual(output.read_bytes(), b'concurrent owner')
            self.assertEqual(source.read_bytes(), b'original')
            self.assertEqual(len(list(Path(directory).iterdir())), 2)


if __name__ == '__main__': unittest.main()
