"""Synthetic H.264/H.265 + AAC open-file DVR regression; no camera access."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from quiet_subprocess import quiet_process_options


@unittest.skipUnless(os.getenv('WITNESS_CONTINUOUS_OPEN_PROBE') and shutil.which('ffmpeg'),
                     'Set native probe and install FFmpeg')
class ContinuousOpenSegmentTests(unittest.TestCase):
    def test_complete_prefixes_decode_before_original_finalize(self):
        with tempfile.TemporaryDirectory(prefix='witness-open-dvr-') as temporary:
            root = Path(temporary)
            for codec in ['h264', 'hevc']:
                with self.subTest(codec=codec):
                    source = root / f'{codec}.mp4'
                    video = ['-c:v', 'libx264', '-threads', '2', '-g', '30', '-bf', '2'] if codec == 'h264' else [
                        '-c:v', 'libx265', '-threads', '2', '-x265-params',
                        'pools=2:frame-threads=2:keyint=30:min-keyint=30:bframes=2:log-level=error', '-tag:v', 'hvc1']
                    def run(arguments):
                        return subprocess.run(arguments, capture_output=True, text=True, timeout=45,
                                              **quiet_process_options())
                    generated = run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-n', '-f', 'lavfi',
                                     '-i', 'testsrc2=size=640x360:rate=15', '-f', 'lavfi', '-i',
                                     'sine=frequency=440:sample_rate=48000', '-t', '8', *video,
                                     '-c:a', 'aac', str(source)])
                    self.assertEqual(generated.returncode, 0, generated.stderr)
                    output = root / f'{codec}-open'
                    probe = run([os.environ['WITNESS_CONTINUOUS_OPEN_PROBE'], str(source), str(output)])
                    self.assertEqual(probe.returncode, 0, probe.stdout + probe.stderr)
                    self.assertIn('before original file closes', probe.stdout)
                    prefixes = sorted(output.glob('prefix-*.mp4'))
                    self.assertGreaterEqual(len(prefixes), 3)
                    for prefix in prefixes:
                        decoded = run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-xerror',
                                       '-i', str(prefix), '-map', '0:v', '-map', '0:a', '-f', 'null', '-'])
                        self.assertEqual(decoded.returncode, 0, decoded.stderr)
                    # Review needs seeking, not merely sequential demuxing.
                    sought = run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-xerror', '-ss', '3',
                                  '-i', str(prefixes[-1]), '-t', '1', '-map', '0:v', '-map', '0:a',
                                  '-f', 'null', '-'])
                    self.assertEqual(sought.returncode, 0, sought.stderr)
                    ordinary = root / f'{codec}-ordinary'
                    probe = run([os.environ['WITNESS_CONTINUOUS_OPEN_PROBE'], str(source), str(ordinary), '--unfragmented'])
                    self.assertEqual(probe.returncode, 0, probe.stdout + probe.stderr)
                    self.assertIn('saving metadata and reconnect isolation', probe.stdout)
                    recordings = list(ordinary.glob('*.mp4'))
                    self.assertEqual(len(recordings), 2)
                    for recording in recordings:
                        decoded = run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-xerror',
                                       '-i', str(recording), '-map', '0:v', '-map', '0:a', '-f', 'null', '-'])
                        self.assertEqual(decoded.returncode, 0, decoded.stderr)


if __name__ == '__main__':
    unittest.main()
