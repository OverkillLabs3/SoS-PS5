"""Host preferences, stored the way the Windows launcher stores them."""

from __future__ import annotations

import configparser
from pathlib import Path

MODES = ("Windowed", "Borderless")
FILE_NAME = "launcher.ini"


def _read(prepared: Path) -> configparser.ConfigParser:
    parser = configparser.ConfigParser()
    path = prepared / FILE_NAME
    if path.exists():
        parser.read(path, encoding="utf-8")
    return parser


def display_mode(prepared: Path) -> str:
    return _read(prepared).get("Display", "Mode", fallback=MODES[0])


def set_display_mode(prepared: Path, mode: str) -> None:
    if mode not in MODES:
        raise ValueError(f"unknown display mode {mode}")
    parser = _read(prepared)
    if not parser.has_section("Display"):
        parser.add_section("Display")
    parser.set("Display", "Mode", mode)
    with (prepared / FILE_NAME).open("w", encoding="utf-8") as handle:
        parser.write(handle)
