#!/usr/bin/env python3
"""Local RC source evidence, staged bundles and conservative file-only deployment.

Uses only the standard library. Never runs the application, providers, sudo,
desktop refreshers or settings tools. Install destinations are explicit; use
--staging-root for all acceptance preparation before the live-install gate.
"""
import argparse
import contextlib
import fcntl
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tarfile
import tempfile
import uuid

# Keep importlib-based private fixtures and direct CLI use on the same module
# instance, so a consumer's outer installation_lock scope is reentrant here.
if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
from install_lock import Refusal, canonical_state, installation_lock


def require(condition, message):
    if not condition:
        raise Refusal(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=True) + "\n").encode()


def read_json(path):
    try:
        return json.loads(regular_bytes(path))
    except (ValueError, OSError) as error:
        raise Refusal(f"Cannot read {path}: {error}") from error


def safe_relative(name):
    require(isinstance(name, str) and name and "\x00" not in name, "Invalid relative path")
    path = PurePosixPath(name)
    require(not path.is_absolute() and all(p not in (".", "..", "") for p in name.split("/")),
            f"Unsafe relative path: {name}")
    return name


def absolute(path):
    value = Path(os.path.abspath(os.fspath(path)))
    no_symlink_ancestors(value, include_leaf=False)
    return value


def no_symlink_ancestors(path, include_leaf=True):
    parts = [path, *path.parents] if include_leaf else list(path.parents)
    for part in reversed(parts):
        try:
            mode = part.lstat().st_mode
        except FileNotFoundError:
            continue
        require(not stat.S_ISLNK(mode), f"Symlink ancestor refused: {part}")
        require(stat.S_ISDIR(mode), f"Non-directory ancestor refused: {part}")


def fingerprint(path):
    no_symlink_ancestors(path, include_leaf=False)
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return {"type": "absent"}
    if stat.S_ISLNK(metadata.st_mode):
        return {"type": "symlink", "target": os.readlink(path)}
    require(stat.S_ISREG(metadata.st_mode), f"Not an ordinary file or symlink: {path}")
    # Opening a symlink substituted after lstat must not read its target.
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as stream:
        before = os.fstat(stream.fileno())
        require(stat.S_ISREG(before.st_mode), f"File changed type: {path}")
        data = stream.read()
        after = os.fstat(stream.fileno())
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_mode)
            == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_mode),
            f"File changed while reading: {path}")
    return {"type": "file", "sha256": sha(data), "bytes": len(data), "mode": stat.S_IMODE(after.st_mode)}


def object_bytes(path, expected):
    require(fingerprint(path) == expected, f"File changed before copying: {path}")
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as stream:
        data = stream.read()
    require(sha(data) == expected["sha256"] and len(data) == expected["bytes"], f"File changed: {path}")
    return data


def regular_bytes(path):
    image = fingerprint(path)
    require(image["type"] == "file", f"Ordinary metadata file required: {path}")
    return object_bytes(path, image)


def sync_directory(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def write_atomic(path, data, mode=0o600):
    no_symlink_ancestors(path, include_leaf=False)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=".rc-write-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fchmod(stream.fileno(), mode)
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        sync_directory(path.parent)
    finally:
        if os.path.lexists(temporary):
            os.unlink(temporary)


def apply_object(path, image, data=None):
    no_symlink_ancestors(path, include_leaf=False)
    if image["type"] == "file":
        require(data is not None and sha(data) == image["sha256"], f"Invalid restore bytes: {path}")
        write_atomic(path, data, image["mode"])
    elif image["type"] == "symlink":
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.parent / (".rc-link-" + uuid.uuid4().hex)
        try:
            os.symlink(image["target"], temporary)
            os.replace(temporary, path)
            sync_directory(path.parent)
        finally:
            if os.path.lexists(temporary):
                temporary.unlink()
    elif image["type"] == "absent":
        if os.path.lexists(path):
            path.unlink()
            sync_directory(path.parent)
    else:
        raise Refusal(f"Invalid image type: {path}")


def managed_path(name):
    safe_relative(name)
    allowed = {
        "bin/control", "share/applications/linux-controlpanel.desktop",
        "libexec/controlpanel-display-guard", "libexec/controlpanel-time-helper",
        "libexec/controlpanel-aero-login-helper",
        "share/polkit-1/actions/org.linuxcontrol.time.policy",
        "share/polkit-1/actions/org.linuxcontrol.aerologin.policy",
    }
    require(name in allowed or name.startswith("share/linux-control/"),
            f"Outside the application's managed payload: {name}")
    return name


def inventory(root, bounded=False):
    no_symlink_ancestors(root)
    result = {}
    for directory, directories, files in os.walk(root, followlinks=False):
        base = Path(directory)
        for name in list(directories):
            if (base / name).is_symlink():
                directories.remove(name)
                files.append(name)
        for name in files:
            path = base / name
            relative = path.relative_to(root).as_posix()
            (managed_path if bounded else safe_relative)(relative)
            image = fingerprint(path)
            # Candidate payload must contain ordinary files. Pre-existing
            # symlinks are backed up/restored as links, without dereferencing.
            require(image["type"] == "file", f"Payload symlink/non-file refused: {path}")
            result[relative] = image
    return dict(sorted(result.items()))


def source(args):
    root, output = absolute(args.root), absolute(args.output)
    require(not output.exists(), f"Output already exists: {output}")
    require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.+_-]*", args.version), "Unsafe release version")
    def git(*arguments):
        return subprocess.check_output(["git", "-C", str(root), *arguments])
    revision = git("rev-parse", "HEAD").decode().strip()
    names = sorted(set(git("ls-files", "--cached", "--others", "--exclude-standard", "-z").decode().split("\0")) - {""})
    # An ignored output directory (usually build/...) must not feed its own
    # identity. Reject a nonignored in-tree output before creating anything.
    try:
        relative_output = output.relative_to(root).as_posix()
    except ValueError:
        relative_output = None
    if relative_output:
        ignored = subprocess.run(["git", "-C", str(root), "check-ignore", "-q", relative_output], check=False)
        require(ignored.returncode == 0, "Source output inside the checkout must be ignored by Git")
    entries, contents = {}, {}
    for name in names:
        safe_relative(name)
        require(not name.startswith(".git/"), "Git internals must not be source inputs")
        path = root / name
        image = fingerprint(path)
        if image["type"] == "absent":
            continue  # A tracked deletion is represented by absence in this complete archive.
        entries[name] = image
        if image["type"] == "file":
            contents[name] = object_bytes(path, image)
    manifest = {"schema": 1, "version": args.version, "revision": revision, "entries": entries}
    manifest_bytes = encoded(manifest)
    digest = sha(manifest_bytes)
    identity = {"version": args.version, "revision": revision, "source_manifest_sha256": digest,
                "build_id": args.version + "+" + digest[:16]}
    output.mkdir(parents=True)
    write_atomic(output / "source-manifest.json", manifest_bytes, 0o644)
    archive = output / "source.tar.gz"
    with archive.open("wb") as raw, gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as tar:
            for name, image in entries.items():
                info = tarfile.TarInfo(name)
                info.mtime = 0
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                if image["type"] == "symlink":
                    info.type = tarfile.SYMTYPE
                    info.linkname = image["target"]
                    info.mode = 0o777
                    tar.addfile(info)
                else:
                    info.mode, info.size = image["mode"], image["bytes"]
                    tar.addfile(info, io.BytesIO(contents[name]))
    identity["source_archive_sha256"] = sha(archive.read_bytes())
    write_atomic(output / "identity.json", encoded(identity), 0o644)
    variables = {"CONTROL_RELEASE_VERSION": identity["version"], "CONTROL_SOURCE_REVISION": revision,
                 "CONTROL_SOURCE_MANIFEST_SHA256": digest, "CONTROL_BUILD_ID": identity["build_id"]}
    cmake = "".join(f'set({key} "{value}" CACHE STRING "Frozen local RC identity" FORCE)\n' for key, value in variables.items())
    write_atomic(output / "build-identity.cmake", cmake.encode(), 0o644)
    print(json.dumps({**identity, "inputs": len(entries), "output": str(output)}, indent=2))


def bundle(args):
    stage, output, source_dir = absolute(args.stage), absolute(args.output), absolute(args.source)
    require(not output.exists(), f"Output already exists: {output}")
    prefix = Path(args.prefix)
    require(prefix.is_absolute() and str(prefix) == args.prefix and ".." not in prefix.parts,
            "Compiled prefix must be an absolute normalized path")
    entries = inventory(stage, bounded=True)
    require("bin/control" in entries and "share/applications/linux-controlpanel.desktop" in entries,
            "Staged application/desktop entry missing")
    desktop = object_bytes(stage / "share/applications/linux-controlpanel.desktop",
                           entries["share/applications/linux-controlpanel.desktop"]).decode()
    require(f'Exec={prefix}/bin/control\n' in desktop or f'Exec="{prefix}/bin/control"\n' in desktop,
            "Desktop Exec must name the compiled prefix's absolute control path")
    identity = read_json(source_dir / "identity.json")
    require(sha(regular_bytes(source_dir / "source-manifest.json")) == identity["source_manifest_sha256"], "Source manifest hash mismatch")
    require(sha(regular_bytes(source_dir / "source.tar.gz")) == identity["source_archive_sha256"], "Source archive hash mismatch")
    dependencies = []
    for supplied in args.dependency:
        path = Path(os.path.abspath(supplied))
        resolved = path.resolve(strict=True)
        dependencies.append({"path": str(path), "resolved_path": str(resolved),
                             "image": fingerprint(resolved), "ownership": "external; never installed or removed by this bundle"})
    output.mkdir(parents=True)
    payload = output / "payload"
    payload.mkdir()
    for name, image in entries.items():
        write_atomic(payload / name, object_bytes(stage / name, image), image["mode"])
    for name in ("source-manifest.json", "source.tar.gz", "identity.json", "build-identity.cmake"):
        write_atomic(output / "source" / name, regular_bytes(source_dir / name), 0o644)
    manifest = {"schema": 1, "prefix": str(prefix), "source": identity, "entries": entries,
                "external_dependencies": dependencies}
    data = encoded(manifest)
    write_atomic(output / "payload-manifest.json", data, 0o644)
    write_atomic(output / "payload-manifest.sha256", (sha(data) + "\n").encode(), 0o644)
    print(json.dumps({"payload_files": len(entries), "manifest_sha256": sha(data), "output": str(output)}, indent=2))


def load_bundle(directory):
    directory = absolute(directory)
    no_symlink_ancestors(directory)
    data = regular_bytes(directory / "payload-manifest.json")
    require(sha(data) == regular_bytes(directory / "payload-manifest.sha256").decode().strip(), "Payload manifest hash mismatch")
    manifest = json.loads(data)
    require(manifest.get("schema") == 1, "Unsupported bundle schema")
    for name in manifest["entries"]:
        managed_path(name)
    require(inventory(directory / "payload", bounded=True) == manifest["entries"], "Payload content/type/mode differs from its manifest")
    return directory, manifest


def empty_state(destination):
    return {"schema": 1, "destination": str(destination), "history": [], "managed": {}}


@contextlib.contextmanager
def state_lock(path):
    path = absolute(path)
    no_symlink_ancestors(path)
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    metadata = path.stat()
    require(metadata.st_uid == os.getuid() and not (metadata.st_mode & 0o077),
            "Deployment state must be owned by this user and private (mode 0700)")
    lock = path / "lock"
    descriptor = os.open(lock, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as error:
        os.close(descriptor)
        raise Refusal("Another deployment command owns this state directory") from error
    try:
        yield path
    finally:
        os.close(descriptor)


def load_state(state_dir, destination=None):
    path = state_dir / "state.json"
    require(not path.is_symlink(), "State file symlink refused")
    state = read_json(path) if path.exists() else empty_state(destination)
    require(state.get("schema") == 1, "Unsupported deployment state")
    require(destination is None or state["destination"] == str(destination), "State belongs to another installation destination")
    for name in state["managed"]:
        managed_path(name)
    return state


def installation_state_path(state_dir, destination):
    state_dir, destination = absolute(state_dir), absolute(destination)
    require(not state_dir.is_relative_to(destination) and not destination.is_relative_to(state_dir),
            "Deployment state must be separate from the managed prefix")
    if destination == Path.home() / ".local":
        require(state_dir == canonical_state(destination),
                "Production deployment must preserve the canonical ~/.control-panel-rc-deployment journal; "
                "a different journal would lose original rollback state")
    return state_dir


@contextlib.contextmanager
def existing_installation(args, recovering=False):
    # Read only enough to identify the destination, then take locks in a single
    # global order (destination first, journal second) and reread under both.
    # Never hold the journal lock while waiting for a running app to close.
    state_dir = absolute(args.state)
    if recovering:
        pending = read_json(state_dir / "pending.json")
        _, journal = read_transaction(state_dir, pending["transaction"])
        destination = absolute(journal["previous_state"]["destination"])
    else:
        destination = absolute(load_state(state_dir)["destination"])
    state_dir = installation_state_path(state_dir, destination)
    with installation_lock(destination), state_lock(state_dir):
        if recovering:
            pending = read_json(state_dir / "pending.json")
            transaction, journal = read_transaction(state_dir, pending["transaction"])
            require(absolute(journal["previous_state"]["destination"]) == destination,
                    "Recovery destination changed while acquiring installation lock")
            yield state_dir, destination, (transaction, journal)
        else:
            state = load_state(state_dir, destination)
            yield state_dir, destination, state


def preflight(destination, expected):
    no_symlink_ancestors(destination)
    conflicts = []
    for name, image in expected.items():
        managed_path(name)
        if fingerprint(destination / name) != image:
            conflicts.append(name)
    require(not conflicts, "Intervening edits; no managed files changed: " + ", ".join(conflicts))


def snapshot(path, image, transaction, index):
    result = {"image": image}
    if image["type"] == "file":
        result["backup"] = f"backups/{index}"
        write_atomic(transaction / result["backup"], object_bytes(path, image), image["mode"])
    return result


def restore_data(transaction, saved):
    if saved["image"]["type"] != "file":
        return None
    name = safe_relative(saved["backup"])
    return object_bytes(transaction / name, saved["image"])


def read_transaction(state_dir, identity):
    require(re.fullmatch(r"[0-9a-f]{32}", identity) is not None, "Invalid transaction identity")
    transaction = state_dir / "transactions" / identity
    no_symlink_ancestors(transaction)
    return transaction, read_json(transaction / "transaction.json")


def commit_change(state_dir, state, desired, data, next_state, operation, keep_history=False):
    require(not os.path.lexists(state_dir / "pending.json"), "Unfinished deployment exists; use recover first")
    destination = absolute(state["destination"])
    # ALL existing managed fingerprints are checked before any payload mutation.
    preflight(destination, state["managed"])
    before = {name: fingerprint(destination / name) for name in desired}
    require(all(before.get(name) == image for name, image in state["managed"].items()),
            "Intervening edit during preflight; no managed files changed")
    identity = uuid.uuid4().hex
    transaction = state_dir / "transactions" / identity
    transaction.mkdir(parents=True)
    sync_directory(transaction.parent)
    saved = {name: snapshot(destination / name, image, transaction, i)
             for i, (name, image) in enumerate(sorted(before.items()))}
    if keep_history:
        next_state["history"] = state["history"] + [identity]
    journal = {"operation": operation, "before": saved, "after": desired,
               "previous_state": state, "next_state": next_state}
    write_atomic(transaction / "transaction.json", encoded(journal))
    # Durable before the first file mutation. recover can distinguish our output
    # from both the original and an intervening edit after an interrupted run.
    write_atomic(state_dir / "pending.json", encoded({"transaction": identity}))
    for name, image in sorted(desired.items()):
        require(fingerprint(destination / name) == before[name], f"Intervening edit during deployment: {name}; use recover")
        apply_object(destination / name, image, data.get(name))
        require(fingerprint(destination / name) == image, f"Write did not verify: {name}; use recover")
    preflight(destination, desired)
    write_atomic(state_dir / "state.json", encoded(next_state))
    (state_dir / "pending.json").unlink()
    sync_directory(state_dir)
    print(json.dumps({"operation": operation, "transaction": identity, "destination": str(destination), "managed_paths": len(next_state["managed"])}))


def install(args):
    directory, manifest = load_bundle(args.bundle)
    prefix = Path(manifest["prefix"])
    require(prefix.is_absolute() and str(prefix) == manifest["prefix"] and ".." not in prefix.parts,
            "Invalid compiled prefix")
    if args.staging_root:
        staging = absolute(args.staging_root)
        destination = staging / prefix.relative_to("/")
    else:
        require(prefix == Path.home() / ".local", "Live deployment is limited to this user's ~/.local")
        destination = prefix
    state_dir = installation_state_path(args.state, destination)
    with installation_lock(destination), state_lock(state_dir):
        state = load_state(state_dir, destination)
        desired = {name: {"type": "absent"} for name in state["managed"]}
        desired.update(manifest["entries"])
        data = {name: object_bytes(directory / "payload" / name, image) for name, image in manifest["entries"].items()}
        next_state = {**state, "managed": desired, "source": manifest["source"],
                      "payload_manifest_sha256": sha(regular_bytes(directory / "payload-manifest.json"))}
        commit_change(state_dir, state, desired, data, next_state, "install/upgrade", keep_history=True)


def rollback_or_remove(args):
    with existing_installation(args) as (state_dir, destination, state):
        require(state["history"], "No active installation in this state directory")
        desired, data = {}, {}
        if args.command == "rollback":
            transaction, journal = read_transaction(state_dir, state["history"][-1])
            next_state = journal["previous_state"]
            for name, saved in journal["before"].items():
                desired[managed_path(name)] = saved["image"]
                data[name] = restore_data(transaction, saved)
        else:
            # First ownership of each path supplies its original object, even
            # when that path was introduced by a later upgrade.
            for identity in state["history"]:
                transaction, journal = read_transaction(state_dir, identity)
                for name, saved in journal["before"].items():
                    managed_path(name)
                    if name not in desired:
                        desired[name], data[name] = saved["image"], restore_data(transaction, saved)
            next_state = empty_state(state["destination"])
        commit_change(state_dir, state, desired, data, next_state, args.command)


def recover(args):
    with existing_installation(args, recovering=True) as (state_dir, destination, recovery):
        transaction, journal = recovery
        current, data = {}, {}
        for name, saved in journal["before"].items():
            managed_path(name)
            current[name] = fingerprint(destination / name)
            require(current[name] in (saved["image"], journal["after"][name]),
                    f"Intervening edit; recovery changed no files: {name}")
            data[name] = restore_data(transaction, saved)
        for name, saved in journal["before"].items():
            require(fingerprint(destination / name) == current[name], f"Intervening edit during recovery: {name}")
            apply_object(destination / name, saved["image"], data[name])
        preflight(destination, {name: saved["image"] for name, saved in journal["before"].items()})
        write_atomic(state_dir / "state.json", encoded(journal["previous_state"]))
        (state_dir / "pending.json").unlink()
        sync_directory(state_dir)
        print("Recovered the interrupted application-file transaction; desktop preferences were not changed.")


def parser():
    cli = argparse.ArgumentParser(description=__doc__)
    commands = cli.add_subparsers(dest="command", required=True)
    item = commands.add_parser("source", help="Freeze tracked and nonignored untracked source inputs")
    item.add_argument("--root", required=True)
    item.add_argument("--output", required=True)
    item.add_argument("--version", required=True)
    item.set_defaults(action=source)
    item = commands.add_parser("bundle", help="Record a final CMake DESTDIR prefix and external dependencies")
    item.add_argument("--stage", required=True, help="Prefix directory INSIDE DESTDIR")
    item.add_argument("--prefix", required=True, help="Absolute prefix configured before compilation")
    item.add_argument("--source", required=True, help="Output directory from source")
    item.add_argument("--output", required=True)
    item.add_argument("--dependency", action="append", default=[], help="External dependency path to hash; never copied/owned")
    item.set_defaults(action=bundle)
    item = commands.add_parser("install", help="Install/upgrade the bundle; no sudo or settings changes")
    item.add_argument("--bundle", required=True)
    item.add_argument("--state", required=True, help="Private persistent backup/journal directory outside the prefix")
    item.add_argument("--staging-root", help="Install under this private root instead of the real compiled prefix")
    item.set_defaults(action=install)
    for name in ("rollback", "remove", "recover"):
        item = commands.add_parser(name)
        item.add_argument("--state", required=True)
        item.set_defaults(action=recover if name == "recover" else rollback_or_remove)
    return cli


def main():
    try:
        args = parser().parse_args()
        args.action(args)
        return 0
    except (Refusal, OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
