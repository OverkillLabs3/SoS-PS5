"""Stub library for PS5 NID imports that this build does not implement.

Windows relinker turns unresolved imports into diagnostics stubs; the Linux
path leaves them for ld.so, which refuses to bind. Here the same idea is
reproduced with real ELF symbols: any undefined import that has the shape of a
PS5 NID (11 characters of the project's base64 alphabet) and is exported by no
host library gets a thunk that reports the call once and returns 0.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

STUB_NAME = "sos-stubs.prx"
NID_SHAPE = re.compile(r"^[A-Za-z0-9+/._-]{11}$")
ALPHABET = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-")


def _symbols(nm_args: list[str]) -> set[str]:
    result = subprocess.run(["nm", "-D", *nm_args], capture_output=True, text=True)
    names = set()
    for line in result.stdout.splitlines():
        parts = line.split()
        if parts:
            names.add(parts[-1])
    return names


def _is_nid_like(name: str) -> bool:
    return bool(NID_SHAPE.fullmatch(name)) and all(char in ALPHABET for char in name)


def _dynamic_field(path: Path, tag: str) -> list[str]:
    result = subprocess.run(["readelf", "--wide", "-d", str(path)], capture_output=True, text=True)
    found = []
    for line in result.stdout.splitlines():
        if f"({tag})" in line:
            found.append(line.split("[", 1)[1].rstrip("]") if "[" in line else line.split()[-1])
    return found


def loaded_set(prepared: Path) -> tuple[list[Path], list[str]]:
    """Transitive closure of objects ld.so will load, following DT_NEEDED the way the
    loader does: $ORIGIN relative to the referring object, bare names from libs/."""
    start = prepared / "app.elf"
    queue = [start]
    loaded: list[Path] = []
    seen: set[Path] = set()
    unresolved: list[str] = []
    while queue:
        current = queue.pop(0)
        resolved = current.resolve()
        if resolved in seen or not resolved.exists():
            continue
        seen.add(resolved)
        loaded.append(current)
        for needed in _dynamic_field(current, "NEEDED"):
            if needed.startswith("$ORIGIN/"):
                candidate = (current.parent / needed[len("$ORIGIN/"):]).resolve()
            elif "/" in needed:
                candidate = Path(needed).resolve()
            else:
                candidate = (prepared / "libs" / needed)
            if candidate.exists():
                queue.append(candidate)
            else:
                unresolved.append(needed)
    return loaded, sorted(set(unresolved))


def _tree(prepared: Path) -> list[Path]:
    """Everything that can reach the loader: the executable plus every converted module, because
    deferred modules arrive later through sceKernelLoadStartModule/dlopen."""
    return [path for path in [prepared / "app.elf", *sorted((prepared / "app0" / "sce_module").glob("*.prx"))]
            if path.exists()]


def missing_symbols(prepared: Path) -> list[str]:
    """NID-shaped imports that no host library, no other module and no C runtime provides."""
    targets = _tree(prepared)
    provided: set[str] = set()
    for library in (prepared / "libs").glob("*.prx"):
        if library.name != STUB_NAME:
            provided |= _symbols(["--defined-only", str(library)])
    for target in targets:
        provided |= _symbols(["--defined-only", str(target)])
    for name in _libc_symbols():
        provided.add(name)
    wanted: set[str] = set()
    for target in targets:
        wanted |= {name for name in _symbols(["--undefined-only", str(target)]) if _is_nid_like(name)}
    return sorted(wanted - provided)


def _libc_symbols() -> set[str]:
    import ctypes.util

    path = ctypes.util.find_library("c")
    candidate = Path("/lib64/libc.so.6") if path is None else Path(f"/lib/{path}")
    if not candidate.exists():
        for guess in ("/lib/x86_64-linux-gnu/libc.so.6", "/usr/lib/libc.so.6", "/usr/lib/x86_64-linux-gnu/libc.so.6"):
            if Path(guess).exists():
                candidate = Path(guess)
                break
        else:
            return set()
    return _symbols(["--defined-only", str(candidate)])


def twin_table(prepared: Path) -> str:
    """Plain libraries that must join the global scope to satisfy `.native` wrapper imports.

    A `.native` wrapper has no DT_NEEDED edge to its plain twin (boykopovar/AnyPS5#906), so a guest
    module naming the wrapper cannot bind a symbol only the plain library exports. Loading every twin
    is not an option - some of them disturb start-up - so this returns only the twins that actually
    define a symbol some loaded object still has undefined.
    """
    libs = prepared / "libs"
    objects = [prepared / "app.elf", *sorted((prepared / "app0" / "sce_module").glob("*.prx"))]
    wanted: set[str] = set()
    for target in objects:
        if not target.exists():
            continue
        undefined = {n for n in _symbols(["--undefined-only", str(target)]) if _is_nid_like(n)}
        if not undefined:
            continue
        for needed in _dynamic_field(target, "NEEDED"):
            name = Path(needed).name
            if not name.endswith(".native.prx"):
                continue
            twin = name[: -len(".native.prx")] + ".prx"
            twin_path = libs / twin
            if not twin_path.exists() or twin in wanted:
                continue
            if undefined & _symbols(["--defined-only", str(twin_path)]) - _symbols(["--defined-only", str(libs / name)]):
                wanted.add(twin)
    return "".join(f'        "{name}",\n' for name in sorted(wanted))


def build(prepared: Path, names: list[str], work: Path) -> Path:
    work.mkdir(parents=True, exist_ok=True)
    (work / "stub.c").write_text(
        "#define _GNU_SOURCE 1\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <dlfcn.h>\n"
        "static int reported;\n"
        "static const char* const kPlainTwins[] = {\n"
        + twin_table(prepared)
        + "        NULL\n        };\n"
        "__attribute__((constructor)) static void sos_stub_link_native_wrappers(void) {\n"
        "    const char* on = getenv(\"SOS_PRELOAD_PLAIN\");\n"
        "    if (on == NULL || on[0] != '1') return;\n"
        "    for (unsigned i = 0; kPlainTwins[i] != NULL; ++i) dlopen(kPlainTwins[i], RTLD_NOW | RTLD_GLOBAL);\n"
        "}\n"
        "void sos_stub_report(const char* name) {\n"
        "    if (__sync_fetch_and_add(&reported, 1) < 24)\n"
        "        fprintf(stderr, \"[sos-stub] unimplemented NID import called: %s\\n\", name);\n"
        "}\n",
        encoding="utf-8",
    )
    asm = [".section .rodata"]
    for index, name in enumerate(names):
        asm.append(f'.Lstr{index}: .asciz "{name}"')
    asm.append(".text")
    for index, name in enumerate(names):
        asm.append(f".Lfun{index}:")
        asm.append(f"\tlea .Lstr{index}(%rip), %rdi")
        asm.append(f"\tcall sos_stub_report")
        asm.append("\txorl %eax, %eax")
        asm.append("\tret")
        asm.append(f'.globl "{name}"')
        asm.append(f'.set "{name}", .Lfun{index}')
    asm.append(f'.globl "{STUB_NAME.replace(".prx", "")}"') if False else None
    (work / "stub.S").write_text("\n".join(asm) + "\n", encoding="utf-8")
    output = prepared / "libs" / STUB_NAME
    subprocess.run(
        ["gcc", "-shared", "-fPIC", "-O2", "-o", str(output), str(work / "stub.c"), str(work / "stub.S")],
        check=True,
    )
    return output
