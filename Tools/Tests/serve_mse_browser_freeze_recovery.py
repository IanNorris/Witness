"""Fixed-asset, loopback-only production-composable browser recovery fixture."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import threading
from serve_mse_fragment_comparison import freeze_assets


def main():
    root = Path(__file__).resolve().parents[2]
    assets = freeze_assets({
        '/': (Path(__file__).with_name('mse_browser_freeze_recovery.html'), 'text/html; charset=utf-8'),
        '/fixture.mp4': (root / 'artifacts/mse-browser-freeze-gop10-input.mp4', 'video/mp4'),
        '/recovery.js': (root / 'artifacts/mse-browser-freeze-recovery.js', 'text/javascript; charset=utf-8'),
    }, {'/fixture.mp4': 'a68f1cbe6b0985370e4e383a4fecbe52fa62a29b366fa1f2756b5a152fed8c0f'})

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            asset = assets.get(self.path)
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
        print(f'recoveryUrl=http://127.0.0.1:{server.server_port}/', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            timer.cancel()


if __name__ == '__main__':
    main()
