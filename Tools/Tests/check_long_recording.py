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


def inspect_long(path, ffmpeg='ffmpeg', ffprobe='ffprobe', timeout=1800):
    path = Path(path).resolve()
    if not path.is_file() or path.stat().st_size > 8 * 1024**3:
        raise ValueError('expected a local recording no larger than 8 GiB')
    if type(timeout) is not int or not 1 <= timeout <= 1800:
        raise ValueError('Invalid inspection timeout')
    common = ['-v', 'error', '-protocol_whitelist', 'file,pipe']
    code, lines, nonempty, _ = bounded_process(
        [ffmpeg, *common, '-xerror', '-i', str(path), '-map', '0:v:0',
         '-enc_time_base', '1:1000000', '-fps_mode', 'passthrough', '-f', 'null', '-'],
        timeout=timeout)
    report = dict(file=str(path), strictDecodeExit=code, decodeDiagnosticLines=lines,
                  inspectionMode='boundedStreamingFullRecording')
    for section, field in (('packets', 'dts'), ('frames', 'best_effort_timestamp')):
        accumulator = TimestampAccumulator(section[:-1], field)
        probe_code, probe_lines, probe_nonempty, diagnostics = bounded_process(
            [ffprobe, *common, '-select_streams', 'v:0', '-show_' + section,
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
