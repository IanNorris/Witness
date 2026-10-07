"""Keep piped background tools in the caller's terminal, without Windows popups."""
import os
import subprocess


def quiet_process_options():
    # A detached parent otherwise gives each console executable a new window.
    # Do not use shell=True or alter pipes, timeouts, arguments or exit handling.
    return {'creationflags': subprocess.CREATE_NO_WINDOW} if os.name == 'nt' else {}
