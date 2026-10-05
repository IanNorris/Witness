"""Cross-principal history checks against a private, normally authenticated server.

HTTP clients are not independent browser profiles or proof of hidden-tab playback.
Only the fixture's disposable authentication database is modified.
"""
from concurrent.futures import ThreadPoolExecutor
import contextlib
import json
import sqlite3
import time


def check_multi_client_history(request, principals, database, restarted=False):
    alice, bob, exporter = (principals[name] for name in ('alice', 'bob', 'exporter'))
    snapshot = {'browserId': 'shared-untrusted-browser', 'sessionId': 'shared-untrusted-tab',
                'username': 'forged-owner', 'userUid': 1, 'visibilityState': 'hidden',
                'players': [{'id': '8_mse', 'latencyMs': 14000}]}
    payloads = [{'csrf': user['csrf'], 'snapshot': snapshot} for user in (alice, bob)]
    for user in (alice, bob):
        assert request('/debug/client-health', token_override=user['token'])[0] == 403
    if not restarted:
        assert request('/debug/client-health', body={**payloads[0], 'csrf': bob['csrf']},
                       token_override=alice['token'])[0] == 403
        with ThreadPoolExecutor(max_workers=2) as clients:
            submissions = [clients.submit(request, '/debug/client-health', body=body,
                                          token_override=user['token'])
                           for user, body in zip((alice, bob), payloads)]
            assert [future.result()[0] for future in submissions] == [202, 202]
        # Authorization must be fresh even after this principal submitted data.
        with contextlib.closing(sqlite3.connect(database)) as db:
            db.execute('UPDATE User SET Enabled=0 WHERE UserUID=?', (alice['uid'],))
            db.commit()
        assert request('/debug/client-health', body=payloads[0], token_override=alice['token'])[0] == 403
        with contextlib.closing(sqlite3.connect(database)) as db:
            db.execute('UPDATE User SET Enabled=1 WHERE UserUID=?', (alice['uid'],))
            db.commit()
        # Revoking a session must block new writes without erasing its evidence.
        with contextlib.closing(sqlite3.connect(database)) as db:
            db.execute('DELETE FROM Session WHERE SessionToken=?', (bob['token'],))
            db.commit()
    assert request('/debug/client-health', body=payloads[1], token_override=bob['token'])[0] == 403
    deadline = time.monotonic() + 10
    while True:
        status, raw = request('/debug/client-health')
        assert status == 200
        export = json.loads(raw)
        rows = [row for row in export['history'] if row['browserId'] == snapshot['browserId']]
        if len(rows) == 2 and export['durableThroughSequence'] >= max(row['sequence'] for row in rows):
            break
        if time.monotonic() >= deadline:
            raise AssertionError('Both owners must be retained durably')
        time.sleep(0.025)
    assert {(row['userUid'], row['username']) for row in rows} == {
        (alice['uid'], 'fixture-alice'), (bob['uid'], 'fixture-bob')}
    assert {row['sessionId'] for row in rows} == {snapshot['sessionId']}
    assert all(row['snapshot']['visibilityState'] == 'hidden' for row in rows)
    assert 'forged-owner' not in raw.decode()
    if not restarted:
        assert request('/debug/client-health', token_override=exporter['token'])[0] == 200
        with contextlib.closing(sqlite3.connect(database)) as db:
            db.execute('UPDATE User SET Admin=0 WHERE UserUID=?', (exporter['uid'],))
            db.commit()
        assert request('/debug/client-health', token_override=exporter['token'])[0] == 403
        with contextlib.closing(sqlite3.connect(database)) as db:
            db.execute('UPDATE User SET Admin=1 WHERE UserUID=?', (exporter['uid'],))
            db.commit()
        assert request('/debug/client-health', token_override=exporter['token'])[0] == 200
        with contextlib.closing(sqlite3.connect(database)) as db:
            db.execute('DELETE FROM Session WHERE SessionToken=?', (exporter['token'],))
            db.commit()
    assert request('/debug/client-health', token_override=exporter['token'])[0] == 403
    return {'passed': True, 'restarted': restarted, 'retainedOwners': len(rows),
            'scope': 'Actual HTTP clients and server sessions; not independent browser profiles or hidden playback'}
