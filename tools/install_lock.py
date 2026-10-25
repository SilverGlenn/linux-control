"""One destination-derived flock shared by Control Panel and deployment tools.

Lock files are permanent: never unlink/replace them. Nested exclusive scopes in
one process reuse the same descriptor, allowing a trusted consumer to hold the
lock across rc_bundle installation and verification.
"""
import contextlib
import fcntl
import math
import os
from pathlib import Path
import stat
import threading
import time


class Refusal(Exception):
    pass


class Busy(Refusal):
    """Only contention/timeouts are deferrable; trust failures need repair."""


def _require(condition, message):
    if not condition:
        raise Refusal(message)


def _absolute(path):
    value = Path(os.path.abspath(os.fspath(path)))
    for ancestor in reversed([value, *value.parents]):
        try:
            image = ancestor.lstat()
        except FileNotFoundError:
            continue
        _require(stat.S_ISDIR(image.st_mode) and not stat.S_ISLNK(image.st_mode),
                 f"Symlink/non-directory installation lock ancestor refused: {ancestor}")
    return value


def canonical_state(destination):
    return _absolute(destination).parent / ".control-panel-rc-deployment"


def _identity(image):
    return image.st_dev, image.st_ino


def _validate(directory, descriptor, path):
    directory_image = directory.lstat()
    _require(stat.S_ISDIR(directory_image.st_mode)
             and directory_image.st_uid == os.getuid()
             and stat.S_IMODE(directory_image.st_mode) == 0o700,
             f"Installation lock directory must be owned by this user with mode 0700: {directory}")
    image, named = os.fstat(descriptor), path.lstat()
    _require(stat.S_ISREG(image.st_mode) and image.st_uid == os.getuid()
             and stat.S_IMODE(image.st_mode) == 0o600 and image.st_nlink == 1
             and not stat.S_ISLNK(named.st_mode) and _identity(image) == _identity(named),
             f"Installation lock must be a private mode 0600 ordinary file with stable identity: {path}")


_held = {}
_mutex = threading.RLock()


@contextlib.contextmanager
def installation_lock(destination, exclusive=True, wait_seconds=0):
    """Hold the canonical app/deployment lock; busy acquisition fails or times out.

    Recursive same-thread scopes are supported. Upgrading a held shared lock is
    refused. Other threads must acquire their own lock; after fork inherited
    registrations are ignored, so children cannot mistake a parent's lock for
    their own acquisition.
    """
    _require(isinstance(wait_seconds, (int, float)) and math.isfinite(wait_seconds)
             and wait_seconds >= 0, "Invalid installation lock wait timeout")
    directory = canonical_state(destination)
    path = directory / "application.lock"
    key = (os.getpid(), threading.get_ident(), str(path))
    with _mutex:
        existing = _held.get(key)
        if existing:
            _require(existing[1] or not exclusive, "Cannot upgrade a shared installation lock")
            _validate(directory, existing[0], path)
            existing[2] += 1
    if existing:
        try:
            yield path
        finally:
            with _mutex:
                existing[2] -= 1
        return
    descriptor = None
    acquired = False
    try:
        _absolute(directory)
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        # Validate the directory before creating any file in it.
        image = directory.lstat()
        _require(stat.S_ISDIR(image.st_mode) and image.st_uid == os.getuid()
                 and stat.S_IMODE(image.st_mode) == 0o700,
                 f"Installation lock directory must be owned by this user with mode 0700: {directory}")
        descriptor = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK, 0o600)
        _validate(directory, descriptor, path)
        deadline = time.monotonic() + wait_seconds
        while True:
            try:
                fcntl.flock(descriptor, (fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH) | fcntl.LOCK_NB)
                break
            except BlockingIOError as error:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise Busy("Control Panel or another deployment is open; installation is deferred until it closes") from error
                time.sleep(min(0.05, remaining))
        _absolute(directory)
        _validate(directory, descriptor, path)
        with _mutex:
            _held[key] = [descriptor, exclusive, 1]
        acquired = True
        try:
            yield path
        finally:
            with _mutex:
                del _held[key]
    except OSError as error:
        if acquired:
            raise
        raise Refusal(f"Cannot safely acquire installation lock {path}: {error}") from error
    finally:
        if descriptor is not None:
            os.close(descriptor)
