"""The game's own settings file: header + every JSON character stored as 4 bytes."""

from __future__ import annotations

from pathlib import Path

HEADER = bytes(range(10)) + bytes(2)
RELATIVE_PATH = Path("_sd") / "GOWSOSSAVE999" / "GoW_SoS999.dat"
RESOLUTIONS = ((0, "3840 x 2160 (4K)"), (6, "2560 x 1440 (2K / QHD)"), (8, "1920 x 1080 (Full HD)"))
DEFAULT_BODY = r"""ersion":"0.1","globalSaveDataV1ref":{"previouslyBootedGame":true,"previouslySavedDifficultyLevel":0,"previouslyInitializedToDefaults":true,"vsyncOn":false,"isPitUnlocked":false,"previouslySelectedSaveSlotIndex":-1,"masterAudioVolume":0.0,"sfxAudioVolume":0.0,"bgmAudioVolume":0.0,"keybindingsOverride":"","pitData":"","hasBeatenGame":false,"aimAssist":0,"enablePuzzleAimAssist":false,"puzzleTiming":0,"hintsEnabled":true,"hintWaitDuration":1,"disableTutorial":true,"GuidedExperience":true,"GameDifficulty":1,"viewControls":false,"AutoPickupEnabled":false,"menuHolds":0,"repeatedButtonPress":0,"traversalInputs":0,"cinematicSkip":false,"touchpadButton":3,"swipeUp":0,"swipeLeft":0,"swipeRight":0,"swipeDown":0,"controllerVisualization":false,"visualizationContrast":0,"hudCustomization":0,"combatHUD":1,"aimReticle":1,"bossHealthBars":1,"enemyHealthBars":1,"gameplayNotificationToggle":1,"volumeBalance":0,"outputDevice":0,"monoAudioSystemIntegratedSupport":false,"globalVolume":10.0,"dialogueVolume":10.0,"musicVolume":10.0,"sfxVolume":10.0,"controllerSpeakerVolume":10.0,"enableSubtitles":true,"subtitleTextColor":0,"displaySpeakerName":true,"speakerTextColor":0,"enableCaptions":false,"captionTextColor":0,"subtitleAndCaptionTextSize":0,"blurSubtitleAndCaptionBackground":false,"subtitleAndCaptionBackground":1,"audioCue":false,"audioCueVolume":10.0,"audioPanning":0.0,"centerPanDialogue":false,"voiceBoost":false,"uiTextSize":0,"iconSize":0,"highContrastHUD":0,"colorFilter":0,"filterStrength":10.0,"highContrastDisplay":0,"HeroColor":0,"CompanionColor":0,"BossColor":0,"EnemyColor":0,"NPCColor":0,"TargetColor":0,"InteractColor":0,"HazardColor":0,"traversalColor":0,"backgroundColor":0,"enableVsync":true,"screenCalibration":false,"filterMode":0,"enableBloodAndGore":0,"enableBrutalKillPrompt":true,"enableBloodVFX":true,"enableCorpseBloodVFX":true,"fullScreenMode":0,"resolutionIndex":0,"aspectRatio":0,"cameraPanningSpeed":10,"cameraShakeIntensity":10,"activationToggle":false,"horizontalSpeed":0,"verticalSpeed":0,"accelerationSpeed":0,"reduceSmallMotions":0,"textLanguage":0,"speechLanguage":0,"controllerVibration":2,"enableAimToggling":false,"enableBlockToggling":false,"enableHDR":true,"gammaValue":50.5,"brightnessValue":50.5,"personalRecordSpeedrunDataBoy":{"isValid":false,"overallTime":0.0},"chapterBestSpeedrunDataBoy":{"isValid":false,"overallTime":0.0},"personalRecordSpeedrunDataCadet":{"isValid":false,"overallTime":0.0},"chapterBestSpeedrunDataCadet":{"isValid":false,"overallTime":0.0},"personalRecordSpeedrunDataSpartan":{"isValid":false,"overallTime":0.0},"chapterBestSpeedrunDataSpartan":{"isValid":false,"overallTime":0.0},"version":2}}"""


def encode(body: str) -> bytes:
    return HEADER + bytes(byte for char in body.encode("utf-8") for byte in (char, 0, 0, 0))


def seed() -> bytes:
    return encode(DEFAULT_BODY)


def path_of(prepared: Path) -> Path:
    return prepared / RELATIVE_PATH


def load(prepared: Path) -> bytes:
    target = path_of(prepared)
    if not target.exists():
        return seed()
    return target.read_bytes()


def ensure_seed(prepared: Path) -> None:
    target = path_of(prepared)
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(seed())


def reset(prepared: Path, resolution_index: int | None = None) -> Path:
    """Rewrite the settings file from the seed, because the title saves over it.

    The title writes its own choices back, so a measurement run that follows a dozen
    earlier runs is not comparable unless the file is reseeded first.
    """
    target = path_of(prepared)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(seed())
    if resolution_index is not None:
        write_int(prepared, "resolutionIndex", resolution_index)
    return target


def _pattern(key: str) -> bytes:
    return encode('"%s":' % key)[len(HEADER):]


def read_int(prepared: Path, key: str) -> int | None:
    return _find_int(load(prepared), key)[2]


def _find_int(data: bytes, key: str):
    pattern = _pattern(key)
    first = data.find(pattern)
    if first < 0 or first % 4 or data.find(pattern, first + 1) >= 0:
        return None, 0, None
    at = first + len(pattern)

    def unit(index: int) -> int:
        offset = at + index * 4
        if offset + 4 > len(data) or data[offset + 1:offset + 4] != b"\x00\x00\x00":
            return 0
        return data[offset]

    value = 0
    digits = 0
    while digits < 9 and 0x30 <= unit(digits) <= 0x39:
        value = value * 10 + (unit(digits) - 0x30)
        digits += 1
    if not digits or unit(digits) not in (0x2C, 0x7D):
        return None, 0, None
    return at, digits, value


def write_int(prepared: Path, key: str, value: int) -> bool:
    """Same rule as the launcher: only an equal-digit-count rewrite, nothing else changes."""
    data = load(prepared)
    at, digits, current = _find_int(data, key)
    if at is None:
        return False
    if current == value:
        return True
    text = str(value)
    if value < 0 or len(text) != digits:
        return False
    output = bytearray(data)
    for index, char in enumerate(text):
        output[at + index * 4] = ord(char)
    target = path_of(prepared)
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + ".tmp")
    temporary.write_bytes(bytes(output))
    temporary.replace(target)
    return True
