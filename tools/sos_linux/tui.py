"""Minimal curses launcher: display mode, resolution, Play."""

from __future__ import annotations

import curses
from pathlib import Path

from . import prefs, settings

ROWS = ("display", "resolution", "play", "exit")
TITLES = {
    "display": "Display mode (space/enter toggles)",
    "resolution": "Resolution (space/enter cycles)",
    "play": "Play",
    "exit": "Exit",
}


def current_resolution(prepared: Path) -> int:
    value = settings.read_int(prepared, "resolutionIndex")
    return value if value in {index for index, _ in settings.RESOLUTIONS} else -1


def _resolution_label(index: int) -> str:
    for value, label in settings.RESOLUTIONS:
        if value == index:
            return label
    return "Keep current setting"


def _cycle_resolution(index: int) -> int:
    values = [value for value, _ in settings.RESOLUTIONS]
    if index not in values:
        return values[0]
    return values[(values.index(index) + 1) % len(values)]


def _menu(stdscr, prepared: Path) -> tuple[str, str, int]:
    curses.curs_set(0)
    selected = 0
    mode = prefs.display_mode(prepared)
    resolution = current_resolution(prepared)
    while True:
        stdscr.erase()
        stdscr.addstr(0, 2, "PPSA28997 (PS5, Linux)")
        for row, key in enumerate(ROWS):
            marker = ">" if row == selected else " "
            detail = f"  [{mode}]" if key == "display" else (f"  [{_resolution_label(resolution)}]" if key == "resolution" else "")
            stdscr.addstr(2 + row, 3, f"{marker} {TITLES[key]}{detail}")
        stdscr.addstr(7, 2, "up/down move - enter or space applies - q quits")
        stdscr.addstr(8, 2, f"game folder: {prepared}")
        stdscr.refresh()
        key = stdscr.getch()
        if key in (curses.KEY_UP, ord("k")):
            selected = (selected - 1) % len(ROWS)
        elif key in (curses.KEY_DOWN, ord("j")):
            selected = (selected + 1) % len(ROWS)
        elif key in (ord("q"), ord("e")):
            return "", mode, resolution
        elif key in (10, 13, ord(" ")):
            choice = ROWS[selected]
            if choice == "play":
                return "play", mode, resolution
            if choice == "exit":
                return "", mode, resolution
            if choice == "display":
                mode = prefs.MODES[1] if mode == prefs.MODES[0] else prefs.MODES[0]
            elif choice == "resolution":
                resolution = _cycle_resolution(resolution)
    return "", mode, resolution


def run(prepared: Path) -> tuple[str, int]:
    """Returns ("play"|"", resolutionIndex). Display mode is stored when chosen."""
    try:
        action, mode, resolution = curses.wrapper(_menu, prepared)
    except curses.error:
        print("terminal too small for the menu, starting with saved settings")
        return "play", current_resolution(prepared)
    if action != "play":
        return "", resolution
    prefs.set_display_mode(prepared, mode)
    if resolution != current_resolution(prepared) and resolution != -1:
        settings.write_int(prepared, "resolutionIndex", resolution)
    return action, resolution
