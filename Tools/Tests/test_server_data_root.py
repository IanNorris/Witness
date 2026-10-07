"""Opt-in real /createdb checks; never start the server or use a live database."""
import contextlib
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from quiet_subprocess import quiet_process_options


@unittest.skipUnless(sys.platform == 'win32' and os.environ.get('WITNESS_TEST_DATA_ROOT_SERVER'),
                     'Set WITNESS_TEST_DATA_ROOT_SERVER and WITNESS_TEST_DATA_ROOT_NATIVE_ID')
class ServerDataRootTests(unittest.TestCase):
    def test_real_createdb_and_invalid_root_fail_before_database_access(self):
        server = Path(os.environ['WITNESS_TEST_DATA_ROOT_SERVER']).resolve(strict=True)
        identity = os.environ.get('WITNESS_TEST_DATA_ROOT_NATIVE_ID', '')
        self.assertRegex(identity, '^[0-9a-f]{12}$')
        binary = server.read_bytes()
        # Refuse accidental use of a previous build that ignores the override.
        self.assertIn(identity.encode('ascii'), binary)
        self.assertIn('WITNESS_DATA_ROOT'.encode('utf-16-le'), binary)
        self.assertIn(b'Witness data root must be an absolute dedicated directory', binary)
        repository = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(prefix='server-data-root-', dir=repository / 'artifacts') as directory:
            fixture = Path(directory).resolve()
            cwd = fixture / 'working-directory'; cwd.mkdir()
            data = fixture / 'private data'
            environment = dict(os.environ, WITNESS_DATA_ROOT=str(data))
            result = subprocess.run([str(server), '/createdb'], env=environment, cwd=cwd,
                                    capture_output=True, text=True, timeout=30, **quiet_process_options())
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((data / 'logs').is_dir())
            self.assertTrue((data / 'server.db').is_file())
            self.assertFalse((cwd / 'server.db').exists())
            with contextlib.closing(sqlite3.connect((data / 'server.db').as_uri() + '?mode=ro', uri=True)) as db:
                tables = {row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
                self.assertTrue({'User', 'Camera', 'Clip'} <= tables, tables)
                columns = [row[1] for row in db.execute('PRAGMA table_info(Clip)')]
                self.assertIn('RecordingFinalized', columns)
                self.assertEqual(columns[11:14], ['DetectionVersion', 'Lighting', 'Reviewed'])
            # Recreate the immediately preceding schema, including its existing
            # audio column. Exercise the executable's real upgrade twice rather
            # than merely executing an extracted ALTER TABLE in memory.
            with contextlib.closing(sqlite3.connect(data / 'server.db')) as db:
                db.execute('ALTER TABLE Clip DROP COLUMN RecordingFinalized')
                legacy_columns = [row[1] for row in db.execute('PRAGMA table_info(Clip)')]
                db.executemany("INSERT INTO Clip (Timestamp,Camera,Duration,Tags,DetectionVersion,AudioDetectionVersion) "
                               "VALUES (?,1,?,'synthetic-pir',0,0)", [(100, 12), (200, 0)])
                db.commit()
                legacy_rows = db.execute('SELECT * FROM Clip ORDER BY Timestamp').fetchall()
            for upgrade_attempt in range(2):
                upgraded = subprocess.run([str(server), '/createdb'], env=environment, cwd=cwd,
                                          capture_output=True, text=True, timeout=30, **quiet_process_options())
                self.assertEqual(upgraded.returncode, 0, upgraded.stdout + upgraded.stderr)
                with contextlib.closing(sqlite3.connect((data / 'server.db').as_uri() + '?mode=ro', uri=True)) as db:
                    upgraded_columns = [row[1] for row in db.execute('PRAGMA table_info(Clip)')]
                    self.assertEqual(upgraded_columns, legacy_columns + ['RecordingFinalized'])
                    upgraded_rows = db.execute('SELECT * FROM Clip ORDER BY Timestamp').fetchall()
                    self.assertEqual([row[:-1] for row in upgraded_rows], legacy_rows)
                    self.assertEqual([row[-1] for row in upgraded_rows], [None, None])
                    self.assertEqual(db.execute('PRAGMA integrity_check').fetchall(), [('ok',)])
            before = (data / 'server.db').read_bytes()
            environment['WITNESS_DATA_ROOT'] = 'relative-fixture'
            invalid = subprocess.run([str(server), '/createdb'], env=environment, cwd=cwd,
                                     capture_output=True, text=True, timeout=30, **quiet_process_options())
            self.assertEqual(invalid.returncode, 1, invalid.stdout + invalid.stderr)
            self.assertIn('absolute dedicated directory', invalid.stderr)
            self.assertFalse((cwd / 'relative-fixture').exists())
            self.assertEqual((data / 'server.db').read_bytes(), before)
            # Cleanup owns only the new fixture. Refuse linked/escaping entries.
            for path in fixture.rglob('*'):
                self.assertFalse(path.is_symlink() or path.is_junction())
                self.assertTrue(path.resolve().is_relative_to(fixture))


if __name__ == '__main__':
    unittest.main()
