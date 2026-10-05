"""Serve exactly one test page and two pre-existing fixture outputs on loopback."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hashlib
from pathlib import Path
import threading

FIXTURE_HASHES = {
    '/adts.mp4': '1bd3faa45658799f10ecae10cb1498a311eda413a349736862e5d30f26b37e0c',
    '/raw.mp4': 'fe21b6ab2fb71521ee491953febb5a3bb59bf299c97ebc193129d76ae2f39a87',
}


def freeze_assets(assets, expected_hashes):
    frozen = {}
    for route, (path, mime) in assets.items():
        if not path.is_file() or path.stat().st_size > 16 * 1024**2:
            raise ValueError('Missing or oversized fixed fixture asset')
        with path.open('rb') as source:
            data = source.read(16 * 1024**2 + 1)
        if len(data) > 16 * 1024**2:
            raise ValueError('Fixture grew past its bounded size')
        if route in expected_hashes and hashlib.sha256(data).hexdigest() != expected_hashes[route]:
            raise ValueError('Fixture changed: revalidate codec and late-join keyframe before use')
        frozen[route] = data, mime
    return frozen


def main():
    root = Path(__file__).resolve().parents[2]
    frozen = freeze_assets({
        '/': (Path(__file__).with_name('mse_fragment_mode_comparison.html'), 'text/html; charset=utf-8'),
        '/adts.mp4': (root / 'artifacts/adts-aac-remux-output.mp4', 'video/mp4'),
        '/raw.mp4': (root / 'artifacts/raw-aac-remux-output.mp4', 'video/mp4'),
    }, FIXTURE_HASHES)

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            asset = frozen.get(self.path)
            if asset is None:
                self.send_error(404)
                return
            data, mime = asset
            self.send_response(200)
            self.send_header('Content-Type', mime)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *_):
            pass

    with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        server.daemon_threads = True
        timer = threading.Timer(900, server.shutdown)
        timer.daemon = True
        timer.start()
        print(f'comparisonUrl=http://127.0.0.1:{server.server_port}/', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            timer.cancel()


if __name__ == '__main__':
    main()
