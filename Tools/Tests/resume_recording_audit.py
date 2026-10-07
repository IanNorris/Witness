"""Resume a stopped, fixture-owned offline audit; never restart its measured run.

The original server and audit must be stopped first. Only retained recordings
in artifacts/run-*/full-server-isolation-*/private cache are accepted. This
continues recording inspection, not the original harness's other assertions.
"""
import argparse
from contextlib import closing
import json
import os
from pathlib import Path
import sqlite3

from test_full_server_isolation import verify_recordings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    parser.add_argument('progress', type=Path)
    parser.add_argument('--stopped-fixture', action='store_true', required=True)
    parser.add_argument('--frame-threads', type=int, choices=range(1, 9), default=1)
    parser.add_argument('--audio-cameras', type=int, nargs='*', default=[])
    args = parser.parse_args()
    artifacts = Path(__file__).resolve().parents[2] / 'artifacts'
    fixture, progress = args.fixture.resolve(strict=True), args.progress.resolve(strict=True)
    if (not fixture.is_relative_to(artifacts.resolve()) or
            not fixture.name.startswith('full-server-isolation-') or
            not fixture.parent.name.startswith('run-') or progress.parent != fixture.parent):
        raise ValueError('Expected a retained isolated run and its own progress file')
    for path in (args.fixture, args.progress, fixture / 'private cache', fixture / 'private data/server.db'):
        if path.is_symlink() or path.is_junction():
            raise ValueError('Linked audit inputs rejected')
    saved = json.loads(progress.read_text(encoding='utf-8'))
    reports = saved['recordingInspection']
    if (saved['schemaVersion'] != 1 or saved['inspectionComplete'] is not False or
            saved['completedOutputs'] != len(reports)):
        raise ValueError('Expected an incomplete consistent checkpoint')
    os.environ['WITNESS_TEST_LONG_RECORDING_INSPECTION'] = '1'
    os.environ['WITNESS_TEST_VERIFY_AUDIO_RECORDINGS'] = '1'
    database = fixture / 'private data/server.db'
    with closing(sqlite3.connect(database.as_uri() + '?mode=ro', uri=True)) as db:
        total = db.execute('SELECT count(*) FROM Clip').fetchone()[0] + db.execute(
            'SELECT count(*) FROM ContinuousSegment').fetchone()[0]
        if total != saved['expectedOutputs']:
            raise ValueError('Fixture output set changed since the checkpoint')
        result = verify_recordings(db, fixture / 'private cache', 0, 0, progress, fixture.parent,
                                  args.frame_threads, args.audio_cameras, reports)
    print(json.dumps({'inspectionComplete': True, 'completedOutputs': len(result),
                      'passed': bool(result) and all(row['passed'] for row in result)}), flush=True)
    return 0 if result and all(row['passed'] for row in result) else 1


if __name__ == '__main__':
    raise SystemExit(main())
