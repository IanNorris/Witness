"""Opt-in native input pacing policy integration; disposable loopback only."""
import json
import contextlib
import http.server
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import unittest
from test_full_server_isolation import loopback_port, replay_senders


@contextlib.contextmanager
def seekable_recording(source):
    """Serve exactly one test recording with byte ranges, bound only to loopback."""
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args): pass
        def do_GET(self):
            if self.path != '/recording':
                self.send_error(404)
                return
            size = source.stat().st_size
            requested = self.headers.get('Range')
            match = re.fullmatch(r'bytes=(\d+)-(\d*)', requested or '')
            if requested and not match:
                self.send_error(416)
                return
            start = int(match[1]) if match else 0
            end = min(int(match[2]) if match and match[2] else size - 1, size - 1)
            if start > end:
                self.send_error(416)
                return
            self.send_response(206 if match else 200)
            self.send_header('Accept-Ranges', 'bytes')
            self.send_header('Content-Type', 'video/mp4')
            self.send_header('Content-Length', str(end - start + 1))
            if match: self.send_header('Content-Range', f'bytes {start}-{end}/{size}')
            self.end_headers()
            try:
                with source.open('rb') as stream:
                    stream.seek(start)
                    remaining = end - start + 1
                    while remaining:
                        chunk = stream.read(min(remaining, 64 * 1024))
                        if not chunk: break
                        self.wfile.write(chunk)
                        remaining -= len(chunk)
            except (BrokenPipeError, ConnectionResetError): pass  # Probe may stop after reading headers/media metadata.
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f'http://127.0.0.1:{server.server_port}/recording'
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


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
            with seekable_recording(source) as recording_url:
                environment.update(WITNESS_TEST_PACING_SOURCE=recording_url, WITNESS_TEST_PACING_EXPECTED='1')
                remote = subprocess.run([str(probe)], env=environment, capture_output=True, timeout=30)
                self.assertEqual(remote.returncode, 0, 'seekable HTTP recording pacing failed')
                self.assertIn(b'requiresPacketPacing=1', remote.stdout)
            with tempfile.TemporaryDirectory(prefix='input-pacing-', dir=Path(__file__).resolve().parents[2] / 'artifacts') as directory:
                root = Path(directory)
                url = f'http://127.0.0.1:{loopback_port()}/replay'
                with replay_senders([source], [url], root):
                    environment.update(WITNESS_TEST_PACING_SOURCE=url, WITNESS_TEST_PACING_EXPECTED='0')
                    live = subprocess.run([str(probe)], env=environment, capture_output=True, timeout=30)
                    self.assertEqual(live.returncode, 0, 'live HTTP pacing/initialization/reset failed')
                    self.assertIn(b'requiresPacketPacing=0', live.stdout)


if __name__ == '__main__': unittest.main()
