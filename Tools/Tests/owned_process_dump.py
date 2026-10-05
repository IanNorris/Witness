"""Capture stacks for a child owned by the calling test, without PID attachment."""
import ctypes
import os
from pathlib import Path
import subprocess


def capture_owned_process_dump(process, destination):
    """Use Popen's retained CreateProcess handle; never open an arbitrary PID.

    Normal + thread-info dumps intentionally exclude full process memory.
    The caller must capture before killing/reaping its child. Output is exclusive.
    """
    if os.name != 'nt':
        raise OSError('Owned process dumps require Windows')
    if not isinstance(process, subprocess.Popen) or process.poll() is not None:
        raise ValueError('Expected a live child owned by this caller')
    import msvcrt
    writer = ctypes.WinDLL('Dbghelp.dll', use_last_error=True).MiniDumpWriteDump
    writer.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p,
                       ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    writer.restype = ctypes.c_int
    destination = Path(destination)
    descriptor = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_BINARY, 0o600)
    try:
        if not writer(int(process._handle), process.pid, msvcrt.get_osfhandle(descriptor),
                      0x1000, None, None, None):  # MiniDumpWithThreadInfo | MiniDumpNormal
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        os.close(descriptor)
    return destination
