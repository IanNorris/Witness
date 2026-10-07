"""Fail closed when an endurance replay uses a different codec/resolution workload."""
import json
import math
from datetime import datetime
from pathlib import Path
import subprocess
from quiet_subprocess import quiet_process_options


def qualify_profiles(inputs, expected, probe=subprocess.run):
    if not isinstance(expected, list) or len(expected) != len(inputs):
        raise ValueError('Expected one replay profile per input')
    keys = {'videoCodec', 'width', 'height', 'audioCodec'}
    cached = {}
    actual = []
    for source, profile in zip(inputs, expected):
        if (not isinstance(profile, dict) or set(profile) != keys or
                type(profile['width']) is not int or type(profile['height']) is not int or
                profile['width'] <= 0 or profile['height'] <= 0 or
                not isinstance(profile['videoCodec'], str) or not profile['videoCodec'] or
                not isinstance(profile['audioCodec'], str)):
            raise ValueError('Invalid expected replay profile')
        source = Path(source).resolve()
        if source not in cached:
            result = probe(['ffprobe', '-v', 'error', '-show_entries',
                            'stream=codec_type,codec_name,width,height', '-of', 'json', str(source)],
                           capture_output=True, text=True, timeout=15, **quiet_process_options())
            if result.returncode or result.stderr.strip() or len(result.stdout) > 65536:
                raise ValueError('Replay profile probe failed')
            streams = json.loads(result.stdout)['streams']
            video = next(row for row in streams if row.get('codec_type') == 'video')
            audio = next((row for row in streams if row.get('codec_type') == 'audio'), {})
            cached[source] = {'videoCodec': video['codec_name'], 'width': video['width'],
                              'height': video['height'], 'audioCodec': audio.get('codec_name', '')}
        observed = cached[source]
        if observed != profile:
            raise ValueError(f'Replay profile mismatch for {source.name}: expected {profile}, observed {observed}')
        actual.append(dict(observed))
    return actual


def validate_live_profiles(cameras, profiles):
    if len(cameras) != len(profiles):
        raise ValueError('Live replay camera membership changed')
    seen = set()
    for camera in cameras:
        uid = camera['cameraId']
        if type(uid) is not int or not 1 <= uid <= len(profiles) or uid in seen:
            raise ValueError('Invalid live replay camera membership')
        seen.add(uid)
        streams = [row for row in camera['streams'] if row.get('tier') == 'main' and row.get('available')]
        if len(streams) != 1 or any(streams[0].get(key) != value for key, value in profiles[uid - 1].items()):
            raise ValueError(f'Live replay profile mismatch for camera {uid}')


def validate_browser_layout_proof(proof, profiles, viewport, now_ms):
    """Require recent independently captured DOM evidence before load timing.

    Browser-side capture is performed through the approved browser UI tools,
    not this harness. browserPresented must come from the browser visibility
    capability, independently of DOM visibility. A page can report visible
    while its unpresented browser is throttled. Neither proves hidden/resume.
    """
    if not isinstance(proof, dict) or proof.get('viewport') != viewport:
        raise ValueError('Browser viewport does not match the measured baseline')
    if (proof.get('browserPresented') is not True or
            proof.get('visibility') != 'visible' or proof.get('fullscreen') is not True or
            proof.get('activityVisible') is not False):
        raise ValueError('Browser is not presented with the required visible fullscreen grid')
    captured = datetime.fromisoformat(proof['capturedUtc'].replace('Z', '+00:00'))
    if captured.tzinfo is None or not 0 <= now_ms - captured.timestamp() * 1000 <= 60000:
        raise ValueError('Browser DOM proof is stale or has no timezone')
    players = proof.get('players')
    if not isinstance(players, list) or len(players) != len(profiles):
        raise ValueError('Browser player membership does not match the replay')
    for player, profile in zip(players, profiles):
        if (not isinstance(player, dict) or player.get('insideViewport') is not True or
                player.get('paused') is not False or player.get('error') is not None or
                type(player.get('readyState')) is not int or player['readyState'] < 3 or
                player['readyState'] > 4 or player.get('width') != profile['width'] or
                player.get('height') != profile['height'] or
                type(player.get('currentTime')) not in (int, float) or
                not math.isfinite(player['currentTime']) or player['currentTime'] <= 0):
            raise ValueError('Browser player is not initialized, visible and playing')
