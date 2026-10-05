import json
import threading
import unittest
import urllib.request
from ptz_update_scope_fixture import check_update_scope


class UpdateScopeFixtureTests(unittest.TestCase):
    def simulated_request(self, hold_query_during_io):
        query_mutex = threading.Lock()

        def request(path, authenticated=True, body=None, timeout=3):
            self.assertEqual(path, '/camera/admin_update')
            if not authenticated or body.get('csrf') != 'fixture-csrf':
                return 400, b''
            if body['id'] == 2:
                with query_mutex:
                    return 200, b'{}'
            self.assertEqual(body['ptzApiHost'], '127.0.0.1')
            payload = json.dumps([{'cmd': 'Login'}]).encode()

            def discover():
                req = urllib.request.Request(f"http://127.0.0.1:{body['ptzApiPort']}/api.cgi?cmd=Login",
                                             data=payload, headers={'Content-Type': 'application/json'})
                with urllib.request.urlopen(req, timeout=timeout) as response:
                    self.assertEqual(json.load(response)[0]['cmd'], 'Login')
                return 200, b'{}'
            if hold_query_during_io:
                with query_mutex:
                    return discover()
            with query_mutex:
                pass  # Represents the scoped database update, before discovery.
            return discover()
        return request

    def test_scoped_query_allows_independent_update_while_login_waits(self):
        result = check_update_scope(self.simulated_request(False), 'fixture-csrf', ['one', 'two'], 0.05)
        self.assertTrue(result['passed'])
        self.assertEqual(result['motorCommands'], 0)
        self.assertEqual(result['loginRequests'], 1)

    def test_fixture_detects_query_held_during_io_and_releases_workers(self):
        result = check_update_scope(self.simulated_request(True), 'fixture-csrf', ['one', 'two'], 0.05)
        self.assertFalse(result['passed'])
        self.assertEqual(result['slowUpdateStatus'], 200)
        self.assertEqual(result['independentUpdateStatus'], 200)

    def test_missing_second_disposable_camera_is_rejected(self):
        with self.assertRaises(ValueError):
            check_update_scope(None, 'fixture-csrf', ['one'])


if __name__ == '__main__':
    unittest.main()
