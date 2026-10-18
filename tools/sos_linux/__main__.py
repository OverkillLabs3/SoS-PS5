"""Command line entry point: prepare | launch | status | menu."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from . import DEFAULT_BUILD, DEFAULT_DUMP, DEFAULT_OUT, VERSION, checks, gamelog, layout, modinit, modules, neededorder, nids, prefs, relink, selfextract, settings, stubs
from .checks import PrepError

RESOLUTION_CHOICES = {index: label for index, label in settings.RESOLUTIONS}


def _dirs(args) -> tuple[Path, Path, Path]:
    return Path(args.dump).resolve(), Path(args.out).resolve(), Path(args.build).resolve()


def _convert_eboot(dump: Path, prep: Path, dry: bool) -> Path:
    source = checks.executable(dump)
    target = prep / "eboot.elf"
    fresh = target.exists() and target.stat().st_mtime >= source.stat().st_mtime
    if fresh or selfextract.is_elf(target):
        return target
    if dry:
        return target
    if not selfextract.convert_or_copy(source, target):
        target.unlink(missing_ok=True)
        raise PrepError("E_NOT_DECRYPTED", f"{source} is not a decrypted ELF or SELF")
    return target


def _plan(dump: Path, out: Path, build: Path, stamp: str, state: str, defer: tuple[str, ...]) -> list[str]:
    steps = [f"check dump {dump}", f"convert eboot -> {out / 'prep' / 'eboot.elf'}"]
    if state != "current":
        steps += [
            f"convert {len(modules.find_modules(dump))} guest modules -> {out / 'prep' / 'sce_module'}",
            f"copy host libraries -> {out / 'libs'}",
            f"link {', '.join(layout.RESOURCE_LINKS)} -> {out / 'app0'}",
            f"relink (state: {state}), then defer {', '.join(defer)}",
        ]
    steps += [f"seed settings -> {settings.path_of(out)}", f"stamp = {stamp}"]
    return steps


def _ensure_stubs(out: Path) -> None:
    missing = stubs.missing_symbols(out)
    if not missing:
        (out / "libs" / stubs.STUB_NAME).unlink(missing_ok=True)
        return
    shown = ", ".join(missing[:8]) + (" ..." if len(missing) > 8 else "")
    print(f"stubs: {len(missing)} unimplemented NID imports ({shown}) -> {out / 'libs' / stubs.STUB_NAME}")
    stubs.build(out, missing, out / "prep" / "stubs")


def _deferred(args) -> tuple[str, ...]:
    if args.defer is not None:
        return tuple(args.defer)
    if os.environ.get("SOS_NO_DEFER"):
        return ()
    return relink.DEFERRED


def _excluded(args) -> tuple[str, ...]:
    return tuple(args.exclude) if args.exclude is not None else relink.EXCLUDED


def cmd_prepare(args) -> int:
    dump, out, build = _dirs(args)
    args.defer = _deferred(args)
    args.exclude = _excluded(args)
    checks.dump_complete(dump)
    checks.build_tree(build)
    checks.libraries_fresh(build)
    checks.free_space(out)
    prep = out / "prep"
    prep.mkdir(parents=True, exist_ok=True)
    eboot = _convert_eboot(dump, prep, args.dry_run)
    relinker = layout.relinker_binary(build)
    stamp = layout.stamp_value(eboot, relinker, layout.libs_source(build))
    state = layout.state(out, stamp) if not args.dry_run else "dry-run"
    for step in _plan(dump, out, build, stamp, state, args.defer):
        print(f"  - {step}")
    if args.dry_run:
        return 0
    if state == "current":
        layout.copy_libs(build, out)
        layout.link_resources(dump, out)
        settings.ensure_seed(out)
        _ensure_stubs(out)
        _ensure_twinscope(out)
        print(f"prepared: already current ({stamp})")
        return 0
    layout.drop_outputs(out)
    converted, failed = modules.prepare_modules(dump, prep / "sce_module")
    if failed:
        raise PrepError("E_MODULE_CONVERT", f"could not convert modules: {', '.join(failed)}")
    count = layout.copy_libs(build, out)
    layout.link_resources(dump, out)
    log = out / "logs" / "prepare.log"
    code = relink.run(relinker, out, eboot, out / "app.elf", log, args.exclude)
    if code != 0 or not (out / "app.elf").exists():
        raise PrepError("E_RELINK", f"prepare failed (exit {code}), see {log}", log)
    if not args.keep_bundled_libc_init:
        target = out / "app0" / "sce_module" / "libc.prx.guest.prx"
        try:
            print(f"neutralised start-up callbacks: {target.name} ({modinit.neutralize(target)} stubbed)")
        except (OSError, ValueError) as error:
            print(f"bundled libc init left active: {error}")
    _ensure_twinscope(out)
    neededorder.order_host_libraries_first(out / "app.elf")
    (out / "defer-list").write_text("\n".join(args.defer) + ("\n" if args.defer else ""))
    deferred = neededorder.defer_guest_modules(out / "app.elf", args.defer, "libkernel.prx")
    if deferred:
        print(f"deferred modules: {', '.join(deferred)} (loaded on demand by the game)")
    if args.psn_probe:
        target = out / "app0" / "sce_module" / "PSN.prx.guest.prx"
        try:
            modinit.neutralize(target)
            (out / "psn-probe").write_text("PSN DT_INIT stubbed; launch adds SOS_PRELOAD_PLAIN=1\n")
            print("psn-probe: PSN start stubbed (diagnostic mode, see run report)")
        except (OSError, ValueError) as error:
            print(f"psn-probe: could not stub PSN start: {error}")
    (out / "app.elf").chmod(0o755)
    layout.write_stamp(prep, stamp)
    settings.ensure_seed(out)
    missing = stubs.missing_symbols(out)
    if missing:
        stubs.build(out, missing, prep / "stubs")
        shown = ", ".join(missing[:12]) + (" ..." if len(missing) > 12 else "")
        print(f"stubs: {len(missing)} unimplemented NID imports ({shown}) -> see {out / 'libs' / stubs.STUB_NAME}")
    print(f"prepared: {len(converted)} modules, {count} libraries, {out / 'app.elf'}")
    return 0


def _ensure_twinscope(out: Path) -> None:
    """Point a module's DT_NEEDED at the library that exports what it imports (see twinscope)."""
    from . import twinscope

    if os.environ.get("SOS_NO_TWINSCOPE"):
        print("twin scoping disabled by SOS_NO_TWINSCOPE")
        return
    try:
        for line in twinscope.apply(out, twinscope.SWAPS):
            print(f"twin scoping: {line}")
    except (OSError, ValueError, neededorder.LayoutError) as error:
        print(f"twin scoping skipped: {error}")


GC_WAIT_DEFAULT = "5"


def apply_gc_wait(extra: dict) -> dict:
    """Cap the guest-exception confirmation wait unless the caller already chose one.

    The engine default is 1000 ms. A thread parked in a kernel wait never confirms the
    exception, so every such round costs a full second. On PPSA28997 that adds about
    25 minutes to boot: the main menu arrives at 1635 s by default and at 86 s here.
    """
    if "APS5_GC_WAIT_MS" not in extra and "APS5_GC_WAIT_MS" not in os.environ:
        extra["APS5_GC_WAIT_MS"] = GC_WAIT_DEFAULT
    return extra


def cmd_launch(args) -> int:
    _, out, build = _dirs(args)
    if not (out / "app.elf").exists():
        raise PrepError("E_RELINK", f"{out / 'app.elf'} is missing - run prepare first")
    checks.build_tree(build)
    stamp = layout.stamp_value(
        out / "prep" / "eboot.elf", layout.relinker_binary(build), layout.libs_source(build)
    )
    if layout.state(out, stamp) != "current":
        print("warning: prepared tree does not match the build tree - run prepare again", file=sys.stderr)
    checks.display()
    checks.vulkan()
    if args.resolution is not None and not settings.write_int(out, "resolutionIndex", args.resolution):
        print(f"warning: resolution {args.resolution} could not be saved", file=sys.stderr)
    if not args.headless_start:
        from . import tui

        if tui.run(out)[0] != "play":
            print("cancelled")
            return 0
        print(f"starting ({prefs.display_mode(out)}); F11 toggles fullscreen, middle mouse toggles mouse-aim")
    extra = apply_gc_wait(_env(args.env))
    # Host libraries reach each other by name; DT_RUNPATH is not transitive, so the
    # prepared library folder goes on the search path as well.
    search = str(out / "libs")
    existing_path = extra.get("LD_LIBRARY_PATH") or __import__("os").environ.get("LD_LIBRARY_PATH", "")
    extra["LD_LIBRARY_PATH"] = f"{search}{':' + existing_path if existing_path else ''}"
    stub_library = out / "libs" / stubs.STUB_NAME
    if (out / "psn-probe").exists():
        extra.setdefault("SOS_PRELOAD_PLAIN", "1")
    if stub_library.exists():
        existing = extra.get("LD_PRELOAD")
        extra["LD_PRELOAD"] = f"{stub_library}{':' + existing if existing else ''}"
    process = gamelog.launch(out, extra)
    print(f"game exited with code {process.returncode}; logs in {out / 'logs'}")
    return process.returncode


def cmd_backlog(args) -> int:
    _, out, _build = _dirs(args)
    root = Path(__file__).resolve().parents[2]
    if not nids.cache_dir(root).exists():
        print("downloading the PS5 NID tables (one time)...")
        nids.sync(root)
    rows = nids.report(out, root)
    named = [row for row in rows if row[0] != "?"]
    print(f"{len(rows)} unimplemented imports, {len(named)} of them named:")
    for name, library, nid in named:
        print(f"  {library:28} {name}")
    libraries = {}
    for name, library, nid in rows:
        libraries[library] = libraries.get(library, 0) + 1
    print("by library:", ", ".join(f"{k}={v}" for k, v in sorted(libraries.items(), key=lambda kv: -kv[1])[:10]))
    return 0


def cmd_status(args) -> int:
    dump, out, build = _dirs(args)
    relinker = layout.relinker_binary(build)
    eboot = out / "prep" / "eboot.elf"
    stamp = (
        layout.stamp_value(eboot, relinker, layout.libs_source(build))
        if eboot.exists() and relinker.exists()
        else "unknown"
    )
    print(f"dump     : {dump} ({len(modules.find_modules(dump))} modules)" if dump.exists() else f"dump     : {dump} MISSING")
    print(f"build    : {'ok' if relinker.exists() else 'missing - run scripts/build.sh'}")
    print(f"prepared : {layout.state(out, stamp) if eboot.exists() else 'absent'} (stamp {stamp})")
    print(f"libraries: {len(list((out / 'libs').glob('*.prx')))}")
    print(f"modules  : {len(list((out / 'app0' / 'sce_module').glob('*.prx')))}")
    print(f"settings : resolutionIndex={settings.read_int(out, 'resolutionIndex')} display={prefs.display_mode(out)}")
    print(f"logs     : {out / 'logs'}")
    return 0


def _env(items: list[str]) -> dict[str, str]:
    values: dict[str, str] = {}
    for item in items:
        key, separator, value = item.partition("=")
        if not separator or not key:
            print(f"warning: ignoring --env {item!r}, expected KEY=VALUE", file=sys.stderr)
            continue
        values[key] = value
    return values


def parser() -> argparse.ArgumentParser:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--dump", default=DEFAULT_DUMP, help="player's game folder")
    common.add_argument("--out", default=DEFAULT_OUT, help="prepared (tool-owned) tree")
    common.add_argument("--build", default=DEFAULT_BUILD, help="CMake build directory of the fork")
    top = argparse.ArgumentParser(prog="sos_linux", description=f"PPSA28997 PS5 Linux runner v{VERSION}")
    sub = top.add_subparsers(dest="command", required=True)

    prepare = sub.add_parser("prepare", parents=[common], help="convert, relink and lay out the game for Linux")
    prepare.add_argument("--dry-run", action="store_true", help="print planned actions only")
    prepare.add_argument("--psn-probe", action="store_true",
                         help="diagnostic: stub the PSN module's start callback and make launch set SOS_PRELOAD_PLAIN=1 (trades the PSN retry stall for a later crash; see the run report)")
    prepare.add_argument("--keep-bundled-libc-init", action="store_true",
                         help="let the dump's own libc.prx copy run its start-up callbacks as well (it then repeats the application-heap handshake and the title builds its main pool twice)")
    prepare.add_argument("--exclude", action="append", dest="exclude", default=None, metavar="MODULE.prx",
                         help="sce_module whose converted copy is not loaded (repeatable; default: the launcher's list)")
    prepare.add_argument("--defer", action="append", dest="defer", default=None, metavar="MODULE.prx",
                         help="module whose lifecycle is deferred (repeatable; default: the launcher's list)")
    prepare.set_defaults(func=cmd_prepare)

    launch = sub.add_parser("launch", parents=[common], help="start the prepared game")
    launch.add_argument("--headless-start", action="store_true", help="skip the menu, use saved choices")
    launch.add_argument("--resolution", type=int, choices=sorted(RESOLUTION_CHOICES), help="set resolutionIndex before starting")
    launch.add_argument("--env", action="append", default=[], metavar="KEY=VALUE", help="extra environment for the game")
    launch.set_defaults(func=cmd_launch)

    status = sub.add_parser("status", parents=[common], help="show what is present, missing, or stale")
    status.set_defaults(func=cmd_status)

    backlog = sub.add_parser("backlog", parents=[common], help="name every unimplemented import in the prepared tree")
    backlog.set_defaults(func=cmd_backlog)
    return top


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        return args.func(args)
    except PrepError as error:
        print(f"{error.code}: {error}", file=sys.stderr)
        if error.log:
            print(f"       log: {error.log}", file=sys.stderr)
        return 2
    except gamelog.AlreadyRunning as error:
        print(f"E_RUNNING: {error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"E_START: {error}", file=sys.stderr)
        return 3
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
