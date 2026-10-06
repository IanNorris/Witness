"""Strict streaming inspection for explicitly opted-in fixture-owned long media."""
from pathlib import Path
import subprocess
import threading

from check_preview_decode import RecordingInspectionError


class TimestampAccumulator:
    def __init__(self, section, field):
        self.section, self.field = section, field
        self.samples = self.missing = self.duplicates = self.regressions = 0
        self.previous = self.minimum = None

    def add(self, line):
        parts = line.decode('utf-8', errors='strict').rstrip('\r\n').split('|')
        if parts[0] != self.section:
            raise ValueError('Unexpected timestamp record')
        fields = [part.split('=', 1)[1] for part in parts[1:]
                  if part.startswith(self.field + '=')]
        if len(fields) > 1:
            raise ValueError('Duplicate timestamp field')
        if not fields or fields[0] == 'N/A':
            self.missing += 1
            return
        value = int(fields[0])
        if self.previous is not None:
            step = value - self.previous
            self.duplicates += step == 0
            self.regressions += step < 0
            self.minimum = step if self.minimum is None else min(self.minimum, step)
        self.previous = value
        self.samples += 1

    def report(self):
        return dict(samples=self.samples, missing=self.missing, duplicates=self.duplicates,
                    regressions=self.regressions, minimumStepTicks=self.minimum)


def bounded_process(command, consume=None, timeout=1800, maximum_rows=2000000):
    """Drain both pipes; retain only 16 KiB stderr and one bounded stdout line."""
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    diagnostic = bytearray()
    diagnostic_lines = 0
    diagnostic_nonempty = False
    timed_out = threading.Event()

    def expire():
        timed_out.set()
        if process.poll() is None:
            process.kill()

    def drain_errors():
        nonlocal diagnostic_lines, diagnostic_nonempty
        last = b''
        while True:
            chunk = process.stderr.read(4096)
            if not chunk:
                break
            diagnostic_lines += chunk.count(b'\n')
            diagnostic_nonempty |= bool(chunk.strip())
            diagnostic.extend(chunk[:max(0, 16384 - len(diagnostic))])
            last = chunk[-1:]
        if last and last != b'\n':
            diagnostic_lines += 1

    reader = threading.Thread(target=drain_errors, daemon=True)
    timer = threading.Timer(timeout, expire)
    timer.daemon = True
    reader.start()
    timer.start()
    try:
        rows = 0
        while True:
            line = process.stdout.readline(4097)
            if not line:
                break
            rows += 1
            if len(line) > 4096 or rows > maximum_rows:
                raise ValueError('Timestamp output exceeded bounded inspection limits')
            if consume:
                consume(line)
        code = process.wait()
        reader.join()
        if timed_out.is_set():
            raise RecordingInspectionError('timeout', code, diagnostic.decode('utf-8', errors='replace'))
        return code, diagnostic_lines, diagnostic_nonempty, diagnostic.decode('utf-8', errors='replace')
    finally:
        timer.cancel()
        if process.poll() is None:
            process.kill()
        process.wait()
        reader.join()
        process.stdout.close()
        process.stderr.close()


def inspection_stage(callback, phase, stream_index=None):
    if callback is not None:
        callback(phase, stream_index)


def inspect_long(path, ffmpeg='ffmpeg', ffprobe='ffprobe', timeout=1800, frame_decoder_threads=1,
                 progress_callback=None):
    # Preserve the old single-thread default. Explicitly bounded opt-in avoids
    # ffprobe's slow default without unbounded auto-threading or shorter scans.
    if type(frame_decoder_threads) is not int or not 1 <= frame_decoder_threads <= 8:
        raise ValueError('Frame decoder threads must be an integer in 1..8')
    if progress_callback is not None and not callable(progress_callback):
        raise ValueError('Inspection progress callback must be callable')
    path = Path(path).resolve()
    if not path.is_file() or path.stat().st_size > 8 * 1024**3:
        raise ValueError('expected a local recording no larger than 8 GiB')
    if type(timeout) is not int or not 1 <= timeout <= 1800:
        raise ValueError('Invalid inspection timeout')
    common = ['-v', 'error', '-protocol_whitelist', 'file,pipe']
    inspection_stage(progress_callback, 'videoStrictDecode')
    code, lines, nonempty, _ = bounded_process(
        [ffmpeg, *common, '-xerror', '-i', str(path), '-map', '0:v:0',
         '-enc_time_base', '1:1000000', '-fps_mode', 'passthrough', '-f', 'null', '-'],
        timeout=timeout)
    report = dict(file=str(path), strictDecodeExit=code, decodeDiagnosticLines=lines,
                  inspectionMode='boundedStreamingFullRecording', frameDecoderThreads=frame_decoder_threads)
    for section, field in (('packets', 'dts'), ('frames', 'best_effort_timestamp')):
        inspection_stage(progress_callback, 'video' + section.title())
        accumulator = TimestampAccumulator(section[:-1], field)
        probe_code, probe_lines, probe_nonempty, diagnostics = bounded_process(
            [ffprobe, *common, *(['-threads', str(frame_decoder_threads)] if section == 'frames' else []),
             '-select_streams', 'v:0', '-show_' + section,
             '-show_entries', section[:-1] + '=' + field, '-of', 'compact=p=1:nk=0', str(path)],
            accumulator.add, timeout)
        if probe_code:
            raise RecordingInspectionError(section, probe_code, diagnostics)
        report[section] = accumulator.report()
        report[section]['diagnosticLines'] = probe_lines
        report[section]['diagnosticNonempty'] = probe_nonempty
    report['passed'] = (code == 0 and not nonempty and
        all(report[s]['samples'] > 1 and report[s]['missing'] == 0 and
            report[s]['duplicates'] == 0 and report[s]['regressions'] == 0 and
            not report[s]['diagnosticNonempty'] for s in ('packets', 'frames')))
    return report


def inspect_audio(path, ffmpeg='ffmpeg', ffprobe='ffprobe', timeout=1800, progress_callback=None):
    """Inspect every audio track fully; absence is reported, not invented as coverage."""
    if type(timeout) is not int or not 1 <= timeout <= 1800:
        raise ValueError('Invalid inspection timeout')
    if progress_callback is not None and not callable(progress_callback):
        raise ValueError('Inspection progress callback must be callable')
    path = Path(path).resolve()
    if not path.is_file() or not 0 < path.stat().st_size <= 8 * 1024**3:
        raise ValueError('expected a nonempty local recording no larger than 8 GiB')
    common = ['-v', 'error', '-protocol_whitelist', 'file,pipe']
    indices = []

    def stream_row(line):
        parts = line.decode('utf-8', errors='strict').strip().split('|')
        # MPEG-TS also lists each stream under its program. Only the canonical
        # top-level stream list owns the index; do not inspect the same track twice.
        if parts == ['']:
            return
        if (len(parts) == 3 and parts[:2] == ['program', 'stream'] and
                parts[2].startswith('index=') and int(parts[2][6:]) >= 0):
            return
        if len(parts) != 2 or parts[0] != 'stream' or not parts[1].startswith('index='):
            raise ValueError('Unexpected audio stream record')
        index = int(parts[1][6:])
        if index < 0 or index in indices:
            raise ValueError('Invalid or duplicate audio stream index')
        indices.append(index)

    inspection_stage(progress_callback, 'audioTrackDiscovery')
    code, _, nonempty, diagnostics = bounded_process(
        [ffprobe, *common, '-select_streams', 'a', '-show_entries', 'stream=index',
         '-of', 'compact=p=1:nk=0', str(path)], stream_row, timeout, maximum_rows=32)
    if code or nonempty:
        raise RecordingInspectionError('audioStreams', code, diagnostics)
    tracks = []
    for index in indices:
        inspection_stage(progress_callback, 'audioStrictDecode', index)
        code, lines, nonempty, _ = bounded_process(
            [ffmpeg, *common, '-xerror', '-i', str(path), '-map', f'0:{index}',
             '-f', 'null', '-'], timeout=timeout)
        track = dict(streamIndex=index, strictDecodeExit=code,
                     decodeDiagnosticLines=lines, decodeDiagnosticNonempty=nonempty)
        for section, field in (('packets', 'dts'), ('frames', 'best_effort_timestamp')):
            inspection_stage(progress_callback, 'audio' + section.title(), index)
            accumulator = TimestampAccumulator(section[:-1], field)
            probe_code, probe_lines, probe_nonempty, diagnostics = bounded_process(
                [ffprobe, *common, '-select_streams', str(index), '-show_' + section,
                 '-show_entries', section[:-1] + '=' + field, '-of', 'compact=p=1:nk=0', str(path)],
                accumulator.add, timeout)
            if probe_code:
                raise RecordingInspectionError('audio' + section, probe_code, diagnostics)
            track[section] = accumulator.report()
            track[section]['diagnosticLines'] = probe_lines
            track[section]['diagnosticNonempty'] = probe_nonempty
        track['passed'] = (code == 0 and not nonempty and all(
            track[s]['samples'] > 1 and track[s]['missing'] == 0 and
            track[s]['duplicates'] == 0 and track[s]['regressions'] == 0 and
            not track[s]['diagnosticNonempty'] for s in ('packets', 'frames')))
        tracks.append(track)
    return dict(inspectionMode='boundedStreamingFullAudio', audioPresent=bool(tracks),
                tracks=tracks, passed=all(track['passed'] for track in tracks))
