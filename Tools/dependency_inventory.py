"""Capture/compare complete installed-package content without modifying it."""
import argparse
import hashlib
import json
import re
from pathlib import Path


def inventory(root):
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError('Installed root must be a directory')
    files = {}
    pending = [root]
    while pending:
        directory = pending.pop()
        for path in sorted(directory.iterdir()):
            if path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction()):
                raise ValueError('Inventory refuses linked entries: ' + str(path.relative_to(root)))
            if path.is_dir():
                pending.append(path)
            elif path.is_file():
                files[path.relative_to(root).as_posix()] = file_record(path, root)
    if not files:
        raise ValueError('Installed root is empty')
    return {'schema': 1, 'algorithm': 'sha256', 'root': str(root), 'files': files}


def file_record(path, root):
    before = path.stat()
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError('File changed while inventorying: ' + str(path.relative_to(root)))
    return {'bytes': after.st_size, 'sha256': digest}


def differences(before, after):
    for snapshot in (before, after):
        if snapshot.get('schema') != 1 or snapshot.get('algorithm') != 'sha256' or not snapshot.get('files'):
            raise ValueError('Invalid or empty inventory')
        if not isinstance(snapshot['files'], dict):
            raise ValueError('Invalid inventory file map')
        for name, record in snapshot['files'].items():
            if (not isinstance(name, str) or not name or '\\' in name or ':' in name or
                    name.startswith('/') or any(part in ('', '.', '..') for part in name.split('/')) or
                    not isinstance(record, dict) or type(record.get('bytes')) is not int or record['bytes'] < 0 or
                    not isinstance(record.get('sha256'), str) or not re.fullmatch('[0-9a-f]{64}', record['sha256'])):
                raise ValueError('Invalid inventory file record')
    left, right = before['files'], after['files']
    return {'added': sorted(right.keys() - left.keys()),
            'removed': sorted(left.keys() - right.keys()),
            'changed': sorted(name for name in left.keys() & right.keys() if left[name] != right[name])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    capture = commands.add_parser('capture')
    capture.add_argument('installed_root', type=Path)
    capture.add_argument('snapshot', type=Path)
    compare = commands.add_parser('compare')
    compare.add_argument('before', type=Path)
    compare.add_argument('after', type=Path)
    args = parser.parse_args()
    if args.command == 'capture':
        root, target = args.installed_root.resolve(strict=True), args.snapshot.resolve()
        if target == root or root in target.parents:
            parser.error('Snapshots must be outside the installed package root')
        if target.exists():
            parser.error('Snapshot already exists; choose a fresh file')
        snapshot = inventory(root)
        with target.open('x', encoding='utf-8') as stream:
            json.dump(snapshot, stream, indent=2)
        print(json.dumps({'files': len(snapshot['files']),
                          'bytes': sum(item['bytes'] for item in snapshot['files'].values())}))
        return 0
    before = json.loads(args.before.read_text(encoding='utf-8'))
    after = json.loads(args.after.read_text(encoding='utf-8'))
    delta = differences(before, after)
    print(json.dumps({kind: {'count': len(paths), 'firstPaths': paths[:20]} for kind, paths in delta.items()}, indent=2))
    return 1 if any(delta.values()) else 0


if __name__ == '__main__':
    raise SystemExit(main())
