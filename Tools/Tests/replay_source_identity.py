"""Boundary fingerprints for matched load inputs, without retaining private paths."""
import hashlib
from pathlib import Path
import re

SCOPE = 'wholeFileSha256AtMeasurementBoundariesNotContinuousImmutabilityProof'
MAXIMUM_BYTES = 512 * 1024 * 1024


def fingerprint_inputs(inputs):
    if not 1 <= len(inputs) <= 16:
        raise ValueError('Expected one to sixteen replay inputs')
    cached, rows = {}, []
    for camera_id, source in enumerate(inputs, 1):
        path = Path(source).resolve(strict=True)
        if path not in cached:
            before = path.stat()
            if not path.is_file() or not 0 < before.st_size <= MAXIMUM_BYTES:
                raise ValueError('Replay input exceeds bounded media size')
            digest, size = hashlib.sha256(), 0
            with path.open('rb') as stream:
                while chunk := stream.read(1024 * 1024):
                    size += len(chunk)
                    if size > MAXIMUM_BYTES:
                        raise ValueError('Replay input grew beyond bounded media size')
                    digest.update(chunk)
            after = path.stat()
            if (size != before.st_size or before.st_size != after.st_size or
                    before.st_mtime_ns != after.st_mtime_ns):
                raise ValueError('Replay input changed while fingerprinting')
            cached[path] = {'bytes': size, 'sha256': digest.hexdigest()}
        rows.append({'cameraId': camera_id, **cached[path]})
    return rows


def validate_identity(report, camera_ids):
    if (not 1 <= len(camera_ids) <= 16 or
            any(type(uid) is not int or uid <= 0 for uid in camera_ids) or
            len(set(camera_ids)) != len(camera_ids)):
        raise ValueError('Invalid expected replay camera membership')
    if not isinstance(report, dict) or report.get('scope') != SCOPE:
        raise ValueError('Missing replay source boundary scope')
    before, after = report.get('before'), report.get('after')
    if (not isinstance(before, list) or not isinstance(after, list) or
            report.get('unchanged') is not True or before != after or
            len(before) != len(camera_ids)):
        raise ValueError('Replay sources changed or boundary evidence incomplete')
    seen = set()
    for row in before:
        if (not isinstance(row, dict) or set(row) != {'cameraId', 'bytes', 'sha256'} or
                type(row['cameraId']) is not int or row['cameraId'] not in camera_ids or
                row['cameraId'] in seen or type(row['bytes']) is not int or
                not 0 < row['bytes'] <= MAXIMUM_BYTES or
                not isinstance(row['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', row['sha256'])):
            raise ValueError('Invalid replay source fingerprint')
        seen.add(row['cameraId'])
    if seen != set(camera_ids):
        raise ValueError('Replay source camera coverage incomplete')
    return sorted(before, key=lambda row: row['cameraId'])
