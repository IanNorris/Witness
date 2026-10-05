"""Opt-in native input pacing policy integration; disposable loopback only."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_full_server_isolation import loopback_port, replay_senders


@unittest.skipUnless(os.environ.get('WITNESS_TEST_PACING_PROBE'), 'native pacing probe not configured')
class InputPacketPacingTests(unittest.TestCase):
    def test_seekable_recording_and_nonseekable_live_http(self):
        probe = Path(os.environ['WITNESS_TEST_PACING_PROBE']).resolve(strict=True)
        inputs = json.loads(os.environ['WITNESS_TEST_PACING_INPUTS'])
        self.assertTrue(1 <= len(inputs) <= 4)
        for value in inputs:
            source = Path(value).resolve(strict=True)
            self.assertTrue(source.is_file())
            environment = dict(os.environ, WITNESS_TEST_PACING_SOURCE=str(source), WITNESS_TEST_PACING_EXPECTED='1')
            recorded = subprocess.run([str(probe)], env=environment, capture_output=True, timeout=30)
            self.assertEqual(recorded.returncode, 0, 'recording pacing/initialization/reset failed')
            self.assertIn(b'requiresPacketPacing=1', recorded.stdout)
            with tempfile.TemporaryDirectory(prefix='input-pacing-', dir=Path(__file__).resolve().parents[2] / 'artifacts') as directory:
                root = Path(directory)
                url = f'http://127.0.0.1:{loopback_port()}/replay'
                with replay_senders([source], [url], root):
                    environment.update(WITNESS_TEST_PACING_SOURCE=url, WITNESS_TEST_PACING_EXPECTED='0')
                    live = subprocess.run([str(probe)], env=environment, capture_output=True, timeout=30)
                    self.assertEqual(live.returncode, 0, 'live HTTP pacing/initialization/reset failed')
                    self.assertIn(b'requiresPacketPacing=0', live.stdout)


if __name__ == '__main__': unittest.main()
