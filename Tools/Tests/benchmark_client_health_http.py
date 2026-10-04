"""Bounded disposable loopback benchmark; no live database or cameras."""
import argparse
import concurrent.futures
import contextlib
import json
import math
from pathlib import Path
import queue
import sqlite3
import subprocess
import tempfile
import threading
import time
import urllib.request


def distribution(values):
    values = sorted(values)
    return {"count": len(values), **{name: round(values[min(len(values) - 1, math.ceil(len(values) * p) - 1)], 3)
        for name, p in [("p50_ms", .5), ("p95_ms", .95), ("p99_ms", .99)]}, "max_ms": round(max(values), 3)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=30)
    args = parser.parse_args()
    if not 15 <= args.seconds <= 60:
        parser.error('seconds must be 15..60')
    if args.output.exists():
        parser.error('refusing to overwrite an existing report')
    repository = Path(__file__).resolve().parents[2]
    results = []
    for populated in (False, True):
        with tempfile.TemporaryDirectory(prefix='witness-health-http-benchmark-') as directory:
            root = Path(directory)
            with contextlib.closing(sqlite3.connect(root / 'auth.db')) as db:
                db.executescript('''
                    CREATE TABLE User(UserUID INTEGER PRIMARY KEY, Username TEXT, DisplayName TEXT, Enabled INTEGER, Admin INTEGER);
                    CREATE TABLE Session(SessionToken TEXT, CSRFToken TEXT, UserUID INTEGER);
                    INSERT INTO User VALUES(1,'admin','Admin',1,1),(100,'reporter-a','A',1,0),(101,'reporter-b','B',1,0);
                    INSERT INTO Session VALUES('admin-token','admin-csrf',1),('a-token','a-csrf',100),('b-token','b-csrf',101);
                ''')
                db.commit()
            if populated:
                # Deliberate worst-case storage/serialization padding, not a
                # claim that real sanitized 16-player snapshots reach this size.
                payload = json.dumps({'fixturePadding': 'x' * 65450}, separators=(',', ':'))
                with contextlib.closing(sqlite3.connect(root / 'history.db')) as db:
                    db.execute('CREATE TABLE samples(seq INTEGER PRIMARY KEY,received INTEGER NOT NULL,user_uid INTEGER NOT NULL,username TEXT NOT NULL,browser TEXT NOT NULL,session TEXT NOT NULL,payload TEXT NOT NULL)')
                    now = int(time.time() * 1000) - 1000
                    db.executemany('INSERT INTO samples VALUES(?,?,?,?,?,?,?)',
                        [(i + 1, now, 100 + i % 2, 'fixture-reporter', f'live-browser-{i % 16}', f'live-tab-{i % 16}', payload) for i in range(126)])
                    db.commit()
            process = subprocess.Popen([str(args.fixture.resolve()), str(root), str(repository / 'WitnessServer/Source/Database.cpp')],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            deadline = threading.Timer(110, process.kill); deadline.daemon = True; deadline.start()
            lines = queue.Queue()
            def read_lines():
                for line in process.stdout:
                    lines.put(line)
            reader = threading.Thread(target=read_lines, daemon=True); reader.start()
            base = None
            try:
                until = time.monotonic() + 15
                while base is None:
                    if time.monotonic() > until:
                        raise RuntimeError('Fixture did not become ready')
                    line = lines.get(timeout=5)
                    if line.startswith('fixturePort='):
                        port = int(line.split('=', 1)[1]); base = f'http://127.0.0.1:{port}'
                def request(path, token='admin-token', body=None):
                    headers = {'Cookie': f'SessionToken-{port}={token}'}
                    data = None if body is None else json.dumps(body).encode()
                    req = urllib.request.Request(base + path, data=data, headers=headers)
                    start = time.perf_counter()
                    with urllib.request.urlopen(req, timeout=10) as response:
                        content = response.read(); status = response.status
                    return (time.perf_counter() - start) * 1000, content, status
                baseline = [request('/fixture/ping')[0] for _ in range(40)]
                initial = json.loads(request('/debug/client-health')[1])
                assert len(initial['history']) == (126 if populated else 0)
                timings = {'exports': [], 'ping': [], 'posts': []}
                end = time.monotonic() + args.seconds
                def worker(kind):
                    iteration = 0
                    while time.monotonic() < end:
                        iteration += 1
                        if kind == 'posts':
                            slot = (iteration - 1) % 16
                            body = {'csrf': 'a-csrf' if slot % 2 == 0 else 'b-csrf', 'snapshot': {
                                'browserId': f'live-browser-{slot}', 'sessionId': f'live-tab-{slot}', 'visibilityState': 'visible',
                                'players': [{'id': f'{i}_mse', 'latencyMs': 1400, 'readyState': 4} for i in range(16)]}}
                            elapsed, content, status = request('/debug/client-health', 'a-token' if slot % 2 == 0 else 'b-token', body)
                            assert status == 202, (status, content)
                        else:
                            elapsed, content, status = request('/debug/client-health' if kind == 'exports' else '/fixture/ping')
                            assert status == 200
                        timings[kind].append(elapsed)
                        # Export flood is intentional; probes/reporting are paced.
                        time.sleep(1 if kind == 'posts' else .1 if kind == 'ping' else .02)
                with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                    futures = [pool.submit(worker, kind) for kind in ['exports', 'exports', 'ping', 'posts']]
                    for future in futures:
                        future.result()
                final = json.loads(request('/debug/client-health')[1])
                assert final['storageFailures'] == 0 and final['pending'] == 0
                if populated:
                    # Reporters reuse seeded identities. Rotating identities
                    # would evict the large rows and invalidate the load case.
                    assert sum(len(json.dumps(row['snapshot']).encode()) for row in final['history']) > 8_000_000
                results.append({'populated': populated, 'duration_seconds': args.seconds,
                    'baseline_ping': distribution(baseline), **{key: distribution(value) for key, value in timings.items()},
                    'initial_rows': len(initial['history']), 'initial_export_bytes': len(json.dumps(initial).encode()),
                    'final_rows': len(final['history']), 'durable_through_sequence': final['durableThroughSequence']})
            finally:
                if base and process.poll() is None:
                    try:
                        with urllib.request.urlopen(urllib.request.Request(base + '/fixture/stop', data=b''), timeout=5):
                            pass
                    except OSError:
                        pass
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
                deadline.cancel(); reader.join(timeout=5); process.stdout.close()
            if process.returncode != 0:
                raise RuntimeError(f'Fixture exited {process.returncode}')
    report = {'scope': 'Synthetic authenticated loopback HTTP; two Crow workers; not the 16-stream/30-minute camera gate', 'results': results}
    args.output.write_text(json.dumps(report, indent=2), encoding='utf8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
