"""Run the vendored relinker for the Linux target."""

from __future__ import annotations

import subprocess
from pathlib import Path

# Same module choices as launcher/launcher.cpp (see research.md R4.4).
EXCLUDED = ("CommonDialog.prx", "VoiceInputPlugin.prx", "libSceNpCppWebApi.prx")
DEFERRED = ("PSN.prx", "SaveData.prx", "lib_burst_generated.prx")


def command(relinker: Path, eboot: Path, output: Path, exclude: tuple[str, ...] = EXCLUDED,
            deferred: tuple[str, ...] | None = None) -> list[str]:
    """`deferred=None` means the launcher's list. An empty tuple asks the relinker to place every
    converted module in the executable's `DT_NEEDED`, which is what the Windows branch effectively
    does: those modules are loaded *and initialised* before the title runs. Deferring on Linux instead
    postpones each module's `DT_INIT` into the title's own `dlopen`."""
    if deferred is None:
        deferred = () if __import__("os").environ.get("SOS_NO_DEFER") else DEFERRED
    argv = [str(relinker), "--registry", "--skip-syscall-check"]
    for name in exclude:
        argv += ["--exclude-sce-module", name]
    for name in deferred:
        argv += ["--defer-sce-module", name]
    return argv + [str(eboot), str(output)]


def run(relinker: Path, prepared: Path, eboot: Path, output: Path, log: Path,
        exclude: tuple[str, ...] = EXCLUDED) -> int:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("wb") as handle:
        return subprocess.run(
            command(relinker, eboot, output, exclude), cwd=prepared, stdout=handle, stderr=subprocess.STDOUT
        ).returncode
