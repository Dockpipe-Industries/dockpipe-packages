"""Native Windows ownership and process identity for disposable proof children."""
import ctypes
from ctypes import wintypes
import subprocess
import os
from pathlib import Path


def current_user_sid():
    result = subprocess.run([
        str(Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"), "-NoProfile", "-NonInteractive", "-Command",
        "[Security.Principal.WindowsIdentity]::GetCurrent().User.Value"],
        check=True, capture_output=True, text=True, timeout=10)
    sid = result.stdout.strip()
    if not sid.startswith("S-1-5-") or any(character not in "S-0123456789" for character in sid):
        raise ValueError("Could not obtain current Windows user identity")
    return sid


def protect_path(path, directory=False):
    # Explicit owner matters when the fixture runs with an elevated token whose
    # default object owner is Administrators. No existing paths are admitted here.
    sid = current_user_sid()
    grant = "*" + sid + (":(OI)(CI)F" if directory else ":F")
    for arguments in (
        ["/setowner", "*" + sid],
        ["/inheritance:r", "/grant:r", grant],
    ):
        subprocess.run([str(Path(os.environ["SystemRoot"]) / "System32/icacls.exe"), str(path), *arguments], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)


def process_start_token(pid):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME),
                                      ctypes.POINTER(wintypes.FILETIME),
                                      ctypes.POINTER(wintypes.FILETIME),
                                      ctypes.POINTER(wintypes.FILETIME)]
    kernel.GetProcessTimes.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        raise OSError("Could not inspect owned fixture child")
    try:
        creation = wintypes.FILETIME()
        exit_time = wintypes.FILETIME()
        kernel_time = wintypes.FILETIME()
        user_time = wintypes.FILETIME()
        if not kernel.GetProcessTimes(handle, ctypes.byref(creation), ctypes.byref(exit_time),
                                      ctypes.byref(kernel_time), ctypes.byref(user_time)):
            raise OSError("Could not read owned fixture creation token")
        return str((creation.dwHighDateTime << 32) | creation.dwLowDateTime)
    finally:
        kernel.CloseHandle(handle)
