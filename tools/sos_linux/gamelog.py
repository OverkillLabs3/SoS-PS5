"""Start the relinked game with logs and a single-instance lock."""

from __future__ import annotations

import fcntl
import os
import subprocess
from pathlib import Path

LOCK_NAME = "app.lock"


class AlreadyRunning(Exception):
    pass


def acquire_lock(prepared: Path):
    prepared.mkdir(parents=True, exist_ok=True)
    # "a" rather than "w": opening the lock must not erase the running instance's pid, or a second
    # launch that is about to be refused destroys the evidence of who owns the game.
    handle = (prepared / LOCK_NAME).open("a")
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        handle.close()
        raise AlreadyRunning("the game is already running") from None
    handle.seek(0)
    handle.truncate()
    handle.write(str(os.getpid()))
    handle.flush()
    return handle


def environment(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = dict(os.environ)
    env.update(extra or {})
    return env


def launch(prepared: Path, env: dict[str, str] | None = None, wait: bool = True) -> subprocess.CompletedProcess:
    app = prepared / "app.elf"
    if not app.exists():
        raise FileNotFoundError(app)
    logs = prepared / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    # Take the lock first: opening the logs truncates the previous run's evidence, and a refused
    # launch must never destroy it.
    lock = acquire_lock(prepared)
    out = (logs / "game.log").open("wb")
    err = (logs / "game.err").open("wb")
    try:
        process = subprocess.Popen([str(app)], cwd=prepared, stdout=out, stderr=err, env=environment(env))
        code = process.wait() if wait else 0
    finally:
        out.close()
        err.close()
        fcntl.flock(lock, fcntl.LOCK_UN)
        lock.close()
    return subprocess.CompletedProcess([str(app)], code if wait else process.poll())
