"""Read-only verification of a Witness Windows deployment against build output.

Native and web build identities are intentionally independent. This does not
prove runtime ABI compatibility or production stability; it detects missing,
extra and mismatched packaged files without modifying either tree.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def digest(path):
    before = path.stat()
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError(f'File changed during hashing: {path.name}')
    return result.hexdigest()


def files(root):
    if root.is_symlink() or root.is_junction():
        raise ValueError(f'Linked tree root: {root.name}')
    found = {}
    # Refuse linked directories before descending, including Windows junctions.
    for entry in root.iterdir():
        if entry.is_symlink() or entry.is_junction():
            raise ValueError(f'Linked package entry: {entry.name}')
        if entry.is_dir():
            for name, path in files(entry).items():
                found[f'{entry.name}/{name}'] = path
        elif entry.is_file():
            found[entry.name] = entry
        else:
            raise ValueError(f'Unsupported package entry: {entry.name}')
    return found


def verify(runtime, package, native_id):
    if not re.fullmatch('[0-9a-f]{12}', native_id):
        raise ValueError('Native identity must be 12 lowercase hexadecimal characters')
    if runtime.resolve() == package.resolve():
        raise ValueError('Package must be separate from build output')
    for root in [runtime, package]:
        if not root.is_dir() or root.is_symlink() or root.is_junction():
            raise ValueError('Expected unlinked runtime/package directories')
    expected = {entry.name: entry for entry in runtime.iterdir() if entry.suffix.lower() == '.dll' and entry.is_file()}
    for name in ['WitnessServer.exe', 'WitnessCamera.dll', 'WitnessCommon.dll', 'openssl.exe', 'openssl.cnf']:
        expected[name] = runtime / name
        if not expected[name].is_file():
            raise ValueError(f'Required build output missing: {name}')
    for name in ['WitnessServer.pdb', 'WitnessCamera.pdb', 'WitnessCommon.pdb']:
        if (package / name).exists():
            expected[name] = runtime / name
    for directory in ['Web', 'models']:
        for name, path in files(runtime / directory).items():
            expected[f'{directory}/{name}'] = path
    actual = files(package)
    missing, extra = sorted(expected.keys() - actual.keys()), sorted(actual.keys() - expected.keys())
    if missing or extra:
        raise ValueError(f'Package inventory mismatch: missing={missing}, extra={extra}')
    for name in actual:
        path = Path(name)
        if path.suffix.lower() in {'.db', '.sqlite', '.sqlite3', '.pem', '.key'} or path.name.lower().endswith(('.db-wal', '.db-shm')):
            raise ValueError(f'Private state is not deployable: {name}')
    hashes = {}
    for name, path in sorted(actual.items()):
        if expected[name].is_symlink() or expected[name].is_junction():
            raise ValueError(f'Linked runtime file: {name}')
        hashes[name] = digest(path)
        if hashes[name] != digest(expected[name]):
            raise ValueError(f'Packaged file differs from build output: {name}')
    for name in ['WitnessServer.exe', 'WitnessCamera.dll']:
        if native_id.encode('ascii') not in actual[name].read_bytes():
            raise ValueError(f'Native build identity absent from {name}')
    web_id = actual['Web/build-hash.txt'].read_text(encoding='utf8').strip()
    if not re.fullmatch('[0-9a-f]{12}', web_id):
        raise ValueError('Invalid web build identity')
    if not any(web_id in path.read_text(encoding='utf8') for name, path in actual.items()
               if name.startswith('Web/assets/') and name.endswith('.js')):
        raise ValueError('Web build identity absent from compiled assets')
    return {'verified': True, 'nativeBuildId': native_id, 'webBuildId': web_id,
        'fileCount': len(actual), 'bytes': sum(path.stat().st_size for path in actual.values()), 'sha256': hashes,
        'limits': 'Byte/identity coherence only; not runtime, ABI or production stability validation'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--native-id', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.resolve().is_relative_to(args.runtime.resolve()) or args.output.resolve().is_relative_to(args.package.resolve()):
        parser.error('Report must be outside both verified trees')
    report = verify(args.runtime, args.package, args.native_id)
    # Exclusive creation prevents overwriting an earlier evidence report.
    with args.output.open('x', encoding='utf8') as stream:
        json.dump(report, stream, indent=2)
    print(json.dumps({key: value for key, value in report.items() if key != 'sha256'}, indent=2))


if __name__ == '__main__':
    main()
