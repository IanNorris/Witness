"""Exercise the native loopback fixture; set WITNESS_HEALTH_HTTP_FIXTURE to opt in."""
import contextlib
import json
import os
from pathlib import Path
import queue
import sqlite3
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request


@unittest.skipUnless(os.environ.get("WITNESS_HEALTH_HTTP_FIXTURE"), "native HTTP fixture not configured")
class HealthHttpTests(unittest.TestCase):
    def test_real_cookies_csrf_cross_client_export_and_restart(self):
        with tempfile.TemporaryDirectory(prefix="witness-health-http-") as directory:
            root = Path(directory)
            with contextlib.closing(sqlite3.connect(root / "auth.db")) as db:
                db.executescript("""
                    CREATE TABLE User(UserUID INTEGER PRIMARY KEY, Username TEXT, DisplayName TEXT, Enabled INTEGER, Admin INTEGER);
                    CREATE TABLE Session(SessionToken TEXT, CSRFToken TEXT, UserUID INTEGER);
                    INSERT INTO User VALUES(1,'admin','Admin',1,1),(7,'alice','Alice',1,0),(8,'bob','Bob',1,0),(9,'disabled','Disabled',0,1);
                    INSERT INTO Session VALUES('admin-token','admin-csrf',1),('alice-token','alice-csrf',7),('bob-token','bob-csrf',8),('disabled-token','disabled-csrf',9),('missing-user-token','missing-csrf',10);
                """)
                db.commit()

            def request(base, port, token=None, body=None, cookie_port=None, method=None, path='/debug/client-health'):
                headers = {}
                if token is not None:
                    headers["Cookie"] = f"unrelated=value; SessionToken-{port if cookie_port is None else cookie_port}={token}"
                if body is not None:
                    headers["Content-Type"] = "application/json"
                req = urllib.request.Request(base + path,
                    data=None if body is None else json.dumps(body).encode(), headers=headers, method=method)
                try:
                    response = urllib.request.urlopen(req, timeout=5)
                except urllib.error.HTTPError as error:
                    response = error
                with response:
                    return response.status, response.read(), response.headers

            payload = {"csrf": "alice-csrf", "snapshot": {"browserId": "browser-a", "sessionId": "tab-a",
                "username": "forged-admin", "userUid": 1, "visibilityState": "hidden", "players": [{"id": "8_mse", "latencyMs": 14000,
                    "driftSamples": [{"t": "2026-10-05T10:00:00Z", "bufferedRangeCount": 2,
                        "playheadBuffered": False, "nextBufferedStartSeconds": 13,
                        "playheadGapMs": 1600, "headroomInCurrentRangeMs": 0,
                        "hasAudio": True, "sourceBufferMode": "sequence", "timestampOffsetSeconds": -3,
                        "buffered": [[1, 2], [13, 14]], "url": "https://fixture-secret@camera"}]}]}}
            for restart in (False, True):
                process = subprocess.Popen([os.environ["WITNESS_HEALTH_HTTP_FIXTURE"], str(root),
                    str(Path(__file__).resolve().parents[2] / "WitnessServer/Source/Database.cpp")],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                deadline = threading.Timer(130, process.kill)
                deadline.daemon = True
                deadline.start()
                lines = queue.Queue()
                def read_lines():
                    for line in process.stdout:
                        lines.put(line)
                reader = threading.Thread(target=read_lines, daemon=True)
                reader.start()
                base = None
                try:
                    ready_until = time.monotonic() + 15
                    while base is None:
                        self.assertLess(time.monotonic(), ready_until, "fixture did not become ready")
                        line = lines.get(timeout=5)
                        if line.startswith("fixturePort="):
                            port = int(line.split("=", 1)[1])
                            base = f"http://127.0.0.1:{port}"
                    if not restart:
                        self.assertEqual(request(base, port, path='/fixture/ping')[0], 403)
                        self.assertEqual(request(base, port, 'alice-token', path='/fixture/ping')[0], 403)
                        self.assertEqual(request(base, port, 'admin-token', path='/fixture/ping')[0], 200)
                        self.assertEqual(request(base, port)[0], 403)
                        self.assertEqual(request(base, port, body=payload)[0], 403)
                        self.assertEqual(request(base, port, "alice-token")[0], 403)
                        self.assertEqual(request(base, port, "admin-token", cookie_port=port + 1)[0], 403)
                        self.assertEqual(request(base, port, "disabled-token")[0], 403)
                        self.assertEqual(request(base, port, "missing-user-token")[0], 403)
                        self.assertEqual(request(base, port, "alice-token", {**payload, "csrf": "wrong"})[0], 403)
                        self.assertEqual(request(base, port, "alice-token", {"snapshot": payload["snapshot"]})[0], 400)
                        self.assertEqual(request(base, port, "alice-token", payload)[0], 202)
                        self.assertEqual(request(base, port, "alice-token", payload)[0], 429)
                        bob = {"csrf": "bob-csrf", "snapshot": {**payload["snapshot"], "browserId": "browser-b", "sessionId": "tab-b"}}
                        self.assertEqual(request(base, port, "bob-token", bob)[0], 202)
                        # A deleted/disabled session cannot write, but retained evidence
                        # must still be included for a different administrator client.
                        with contextlib.closing(sqlite3.connect(root / "auth.db")) as db:
                            db.execute("DELETE FROM Session WHERE SessionToken='bob-token'")
                            db.commit()
                        self.assertEqual(request(base, port, "bob-token", bob)[0], 403)
                    until = time.monotonic() + 5
                    while True:
                        code, body, headers = request(base, port, "admin-token")
                        self.assertEqual(code, 200)
                        self.assertEqual(headers["Cache-Control"], "no-store")
                        export = json.loads(body)
                        if len(export["history"]) == 2 and export["durableThroughSequence"] >= 2:
                            break
                        self.assertLess(time.monotonic(), until, "history was not durably exported")
                        time.sleep(0.02)
                    owners = {(row["userUid"], row["username"], row["browserId"]) for row in export["history"]}
                    self.assertEqual(owners, {(7, "alice", "browser-a"), (8, "bob", "browser-b")})
                    self.assertNotIn("forged-admin", body.decode())
                    self.assertNotIn("fixture-secret", body.decode())
                    for row in export["history"]:
                        sample = row["snapshot"]["players"][0]["driftSamples"][0]
                        self.assertIs(sample["playheadBuffered"], False)
                        self.assertEqual(sample["bufferedRangeCount"], 2)
                        self.assertEqual(sample["nextBufferedStartSeconds"], 13)
                        self.assertEqual(sample["playheadGapMs"], 1600)
                        self.assertEqual(sample["headroomInCurrentRangeMs"], 0)
                        self.assertIs(sample["hasAudio"], True)
                        self.assertEqual(sample["sourceBufferMode"], "sequence")
                        self.assertEqual(sample["timestampOffsetSeconds"], -3)
                        self.assertNotIn("buffered", sample)
                        self.assertNotIn("url", sample)
                finally:
                    if base and process.poll() is None:
                        try:
                            with urllib.request.urlopen(urllib.request.Request(base + "/fixture/stop", data=b""), timeout=5):
                                pass
                        except (OSError, urllib.error.URLError):
                            pass
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                    deadline.cancel()
                    reader.join(timeout=5)
                    process.stdout.close()
                self.assertEqual(process.returncode, 0)


if __name__ == "__main__":
    unittest.main()
