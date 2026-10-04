"""Bounded manual browser fixture; temporary synthetic auth/history only."""
import argparse
import contextlib
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--module', type=Path, required=True)
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='witness-health-http-browser-') as directory:
        root = Path(directory)
        shutil.copyfile(args.module, root / 'reporter.js')
        with contextlib.closing(sqlite3.connect(root / 'auth.db')) as db:
            db.executescript("""
                CREATE TABLE User(UserUID INTEGER PRIMARY KEY, Username TEXT, DisplayName TEXT, Enabled INTEGER, Admin INTEGER);
                CREATE TABLE Session(SessionToken TEXT, CSRFToken TEXT, UserUID INTEGER);
                INSERT INTO User VALUES(1,'fixture-admin','Fixture Admin',1,1);
                INSERT INTO Session VALUES('admin-token','admin-csrf',1);
            """)
            db.commit()
        with subprocess.Popen([str(args.fixture.resolve()), str(root),
                               str(repository / 'WitnessServer/Source/Database.cpp'),
                               str(root / 'reporter.js')]) as process:
            deadline = threading.Timer(315, process.kill)
            deadline.daemon = True
            deadline.start()
            try:
                return process.wait(timeout=320)
            finally:
                deadline.cancel()
                if process.poll() is None:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    raise SystemExit(main())
