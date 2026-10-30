"""Windows: start with the user's session, through the HKCU Run key.
(On Linux the systemd user unit, host/volume-sync.service, does this.)"""
import sys
import winreg

RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
VALUE = "volume-keeb"


def _command():
    if getattr(sys, "frozen", False):  # the PyInstaller .exe
        return f'"{sys.executable}"'
    pythonw = sys.executable.replace("python.exe", "pythonw.exe")
    return f'"{pythonw}" "{sys.argv[0]}"'


def enabled():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            winreg.QueryValueEx(key, VALUE)
            return True
    except OSError:
        return False


def set_enabled(on):
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as key:
        if on:
            winreg.SetValueEx(key, VALUE, 0, winreg.REG_SZ, _command())
        else:
            try:
                winreg.DeleteValue(key, VALUE)
            except OSError:
                pass
