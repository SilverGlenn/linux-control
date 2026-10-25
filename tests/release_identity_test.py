#!/usr/bin/env python3
"""Queries the built executable without a graphical session or usable bus."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable = str(Path(sys.argv[1]).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="control-headless-identity-") as directory:
        # A real QApplication would fail loading this deliberately invalid QPA
        # plugin. Invalid bus endpoints also prevent an accidental live bus use.
        environment = {key: value for key, value in os.environ.items()
                       if key not in ("DISPLAY", "WAYLAND_DISPLAY", "LD_LIBRARY_PATH", "LD_PRELOAD", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH")}
        environment.update({"HOME": directory, "XDG_CONFIG_HOME": directory + "/config",
                            "XDG_DATA_HOME": directory + "/data", "XDG_CACHE_HOME": directory + "/cache",
                            "XDG_STATE_HOME": directory + "/state", "XDG_RUNTIME_DIR": directory,
                            "QT_QPA_PLATFORM": "control-deliberately-missing-platform",
                            "DBUS_SESSION_BUS_ADDRESS": "unix:path=" + directory + "/absent-session-bus",
                            "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=" + directory + "/absent-system-bus"})
        before = set(Path(directory).rglob("*"))
        version_output = None
        for argument in ("--version", "-v", "--help", "-h"):
            result = subprocess.run([executable, argument], cwd=directory, env=environment,
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode == 0, (argument, result.returncode, result.stderr)
            assert not result.stderr, (argument, result.stderr)
            if argument in ("--version", "-v"):
                assert "Control Panel " in result.stdout and "Source manifest SHA-256: " in result.stdout, result.stdout
                if version_output is not None:
                    assert result.stdout == version_output
                version_output = result.stdout
            else:
                assert "Usage: control" in result.stdout
        for arguments in (("--version", "--help"), ("--version", "--unknown"), ("--help", "-platform", "offscreen")):
            result = subprocess.run([executable, *arguments], cwd=directory, env=environment,
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode == 2, (arguments, result.returncode, result.stderr)
        assert set(Path(directory).rglob("*")) == before, "Identity query created user files"
    print("Headless identity/help queries passed; no application-owned files created.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
