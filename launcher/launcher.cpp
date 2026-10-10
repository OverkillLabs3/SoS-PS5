#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <shlobj.h>
#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>
using std::max;
using std::min;
#include <gdiplus.h>
#include <psapi.h>
#include <cstdio>

#define LAUNCHER_TITLE L"SonsOfSparta-PS5 Native"
#define LAUNCHER_NAME L"God of War Sons of Sparta"
#define LAUNCHER_RUNTIME L"sos_runtime.exe"
#define LAUNCHER_MUTEX L"Local\\SonsOfSpartaLauncher"
#define LAUNCHER_ICON L"sons_of_sparta.ico"
#define LAUNCHER_TITLE_ID L"PPSA28997"
#define LAUNCHER_REQUIRED L"sce_sys;Media"

namespace {

const wchar_t* kTitle = LAUNCHER_TITLE;
const wchar_t* kRuntime = LAUNCHER_RUNTIME;

void Fail(const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | MB_ICONERROR);
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

const unsigned char kSettingsHeader[12] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x00, 0x00};
const char kSettingsBody[] = R"json(ersion":"0.1","globalSaveDataV1ref":{"previouslyBootedGame":true,"previouslySavedDifficultyLevel":0,"previouslyInitializedToDefaults":true,"vsyncOn":false,"isPitUnlocked":false,"previouslySelectedSaveSlotIndex":-1,"masterAudioVolume":0.0,"sfxAudioVolume":0.0,"bgmAudioVolume":0.0,"keybindingsOverride":"","pitData":"","hasBeatenGame":false,"aimAssist":0,"enablePuzzleAimAssist":false,"puzzleTiming":0,"hintsEnabled":true,"hintWaitDuration":1,"disableTutorial":true,"GuidedExperience":true,"GameDifficulty":1,"viewControls":false,"AutoPickupEnabled":false,"menuHolds":0,"repeatedButtonPress":0,"traversalInputs":0,"cinematicSkip":false,"touchpadButton":3,"swipeUp":0,"swipeLeft":0,"swipeRight":0,"swipeDown":0,"controllerVisualization":false,"visualizationContrast":0,"hudCustomization":0,"combatHUD":1,"aimReticle":1,"bossHealthBars":1,"enemyHealthBars":1,"gameplayNotificationToggle":1,"volumeBalance":0,"outputDevice":0,"monoAudioSystemIntegratedSupport":false,"globalVolume":10.0,"dialogueVolume":10.0,"musicVolume":10.0,"sfxVolume":10.0,"controllerSpeakerVolume":10.0,"enableSubtitles":true,"subtitleTextColor":0,"displaySpeakerName":true,"speakerTextColor":0,"enableCaptions":false,"captionTextColor":0,"subtitleAndCaptionTextSize":0,"blurSubtitleAndCaptionBackground":false,"subtitleAndCaptionBackground":1,"audioCue":false,"audioCueVolume":10.0,"audioPanning":0.0,"centerPanDialogue":false,"voiceBoost":false,"uiTextSize":0,"iconSize":0,"highContrastHUD":0,"colorFilter":0,"filterStrength":10.0,"highContrastDisplay":0,"HeroColor":0,"CompanionColor":0,"BossColor":0,"EnemyColor":0,"NPCColor":0,"TargetColor":0,"InteractColor":0,"HazardColor":0,"traversalColor":0,"backgroundColor":0,"enableVsync":true,"screenCalibration":false,"filterMode":0,"enableBloodAndGore":0,"enableBrutalKillPrompt":true,"enableBloodVFX":true,"enableCorpseBloodVFX":true,"fullScreenMode":0,"resolutionIndex":0,"aspectRatio":0,"cameraPanningSpeed":10,"cameraShakeIntensity":10,"activationToggle":false,"horizontalSpeed":0,"verticalSpeed":0,"accelerationSpeed":0,"reduceSmallMotions":0,"textLanguage":0,"speechLanguage":0,"controllerVibration":2,"enableAimToggling":false,"enableBlockToggling":false,"enableHDR":true,"gammaValue":50.5,"brightnessValue":50.5,"personalRecordSpeedrunDataBoy":{"isValid":false,"overallTime":0.0},"chapterBestSpeedrunDataBoy":{"isValid":false,"overallTime":0.0},"personalRecordSpeedrunDataCadet":{"isValid":false,"overallTime":0.0},"chapterBestSpeedrunDataCadet":{"isValid":false,"overallTime":0.0},"personalRecordSpeedrunDataSpartan":{"isValid":false,"overallTime":0.0},"chapterBestSpeedrunDataSpartan":{"isValid":false,"overallTime":0.0},"version":2}})json";

const wchar_t* kSettingsFile = L"\\_sd\\GOWSOSSAVE999\\GoW_SoS999.dat";
const char kResolutionKey[] = "resolutionIndex";

// resolutionIndex picks from the list the game builds at run time from the PS5 display manager: the 16:9 sizes
// 1920 to 3200 wide in steps of 320, then to 3840 in steps of 160, sorted from the widest. Only the common ones are offered.
struct Resolution { int index; const wchar_t* label; };
const Resolution kResolutions[] = {{0, L"3840 x 2160 (4K)"}, {6, L"2560 x 1440 (2K / QHD)"}, {8, L"1920 x 1080 (Full HD)"}};

// Host settings that do not belong in the game's save, read with the Windows profile API from launcher.ini.
const wchar_t* kLauncherIni = L"\\launcher.ini";

// Canonical names are lowercase snake_case. A historical name is read only where the canonical one is missing, and saving renames it.
struct IniSection { const wchar_t* name; const wchar_t* legacyName; };
const IniSection kDisplaySection{L"display", L"Display"};
const IniSection kStartupSection{L"startup", L"Startup"};
const IniSection kQualitySection{L"quality_of_life", L"QualityOfLife"};
const IniSection kIniSections[] = {kDisplaySection, kStartupSection, kQualitySection};
struct IniKeyAlias { const wchar_t* legacy; const wchar_t* name; };
const IniKeyAlias kIniKeyAliases[] = {{L"Mode", L"mode"}, {L"SkipDialogue", L"skip_dialogue"}, {L"RevealFullMap", L"reveal_full_map"}};
// Settings of features the project no longer has. Nothing reads them; a save deletes them.
const wchar_t* const kCheatsSection = L"Cheats";
struct IniKey { const wchar_t* section; const wchar_t* key; };
const IniKey kObsoleteSettings[] = {{kCheatsSection, L"enable_cheats"}, {kCheatsSection, L"enable_cheat_menu"},
                                    {kQualitySection.legacyName, L"LadderGrabWithUp"}, {kQualitySection.name, L"LadderGrabWithUp"}};

// A canonical key wins over a historical one, then the canonical section over the historical one; the profile API ignores case.
bool FindSetting(const std::wstring& directory, const IniSection& section, const wchar_t* key, const wchar_t* legacyKey, std::wstring& value) {
    const std::wstring path = directory + kLauncherIni;
    const wchar_t absent[] = L"\x01";
    const wchar_t* const keys[] = {key, legacyKey != nullptr ? legacyKey : key};
    const wchar_t* const sections[] = {section.name, section.legacyName};
    for (const wchar_t* keyName : keys) {
        for (const wchar_t* sectionName : sections) {
            wchar_t text[64] = {};
            GetPrivateProfileStringW(sectionName, keyName, absent, text, 64, path.c_str());
            if (wcscmp(text, absent) != 0) {
                value = text;
                return true;
            }
        }
    }
    return false;
}

std::wstring ReadSetting(const std::wstring& directory, const IniSection& section, const wchar_t* key, const wchar_t* legacyKey,
                         const wchar_t* fallback) {
    std::wstring value;
    return FindSetting(directory, section, key, legacyKey, value) ? value : fallback;
}

bool ReadBorderless(const std::wstring& directory) {
    return _wcsicmp(ReadSetting(directory, kDisplaySection, L"mode", nullptr, L"Windowed").c_str(), L"Borderless") == 0;
}

bool ReadSkipIntro(const std::wstring& directory) {
    return ReadSetting(directory, kStartupSection, L"skip_intro", nullptr, L"0") == L"1";
}

bool ReadSkipDialogue(const std::wstring& directory) {
    return ReadSetting(directory, kQualitySection, L"skip_dialogue", L"SkipDialogue", L"0") == L"1";
}

bool ReadRevealFullMap(const std::wstring& directory) {
    return ReadSetting(directory, kQualitySection, L"reveal_full_map", L"RevealFullMap", L"0") == L"1";
}

int ReadFrameRate(const std::wstring& directory) {
    const std::wstring value = ReadSetting(directory, kDisplaySection, L"frame_rate", nullptr, L"60");
    if (value == L"120") return 1;
    if (value == L"unlocked") return 2;
    return 0;
}

bool ReadShowInGameFps(const std::wstring& directory) {
    return ReadSetting(directory, kQualitySection, L"show_in_game_fps", nullptr, L"0") == L"1";
}

// False when the file cannot be read or is UTF-16, which the launcher does not write.
bool ReadIniText(const std::wstring& path, std::string& text) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return false;
    char buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, count);
    fclose(file);
    return text.rfind("\xFF\xFE", 0) != 0;
}

// The profile API adds a new section right below the last line. After Play has saved, every section header but the first is
// preceded by exactly one blank line and trailing blank lines are dropped; other lines stay as they are. A UTF-16 file is left alone.
void SeparateIniSections(const std::wstring& path) {
    std::string text, formatted;
    if (!ReadIniText(path, text)) return;
    size_t blanks = 0;
    for (size_t start = 0; start < text.size();) {
        const size_t end = min(text.find('\n', start), text.size());
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) {
            ++blanks;
            continue;
        }
        if (line[first] == '[') blanks = formatted.empty() ? 0 : 1;
        for (; blanks > 0; --blanks) formatted += "\r\n";
        formatted += line + "\r\n";
    }
    if (formatted == text) return;
    const std::wstring temporary = path + L".tmp";
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || file == nullptr) return;
    const bool written = fwrite(formatted.data(), 1, formatted.size(), file) == formatted.size();
    if (fclose(file) != 0 || !written || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileW(temporary.c_str());
}

using IniPairs = std::vector<std::pair<std::wstring, std::wstring>>;

IniPairs SectionPairs(const std::wstring& path, const wchar_t* section) {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetPrivateProfileSectionW(section, buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    IniPairs pairs;
    for (const wchar_t* entry = buffer.data(); entry < buffer.data() + length && *entry != L'\0'; entry += wcslen(entry) + 1) {
        const std::wstring text = entry;
        const size_t equals = text.find(L'=');
        if (equals != std::wstring::npos) pairs.emplace_back(text.substr(0, equals), text.substr(equals + 1));
    }
    return pairs;
}

std::wstring CanonicalKey(const std::wstring& key) {
    for (const IniKeyAlias& alias : kIniKeyAliases) {
        if (_wcsicmp(key.c_str(), alias.legacy) == 0) return alias.name;
    }
    return key;
}

bool WriteIniValue(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    return WritePrivateProfileStringW(section, key, value, path.c_str()) != FALSE;
}

bool IsObsoleteSetting(const wchar_t* section, const std::wstring& key) {
    for (const IniKey& setting : kObsoleteSettings) {
        if (_wcsicmp(section, setting.section) == 0 && _wcsicmp(key.c_str(), setting.key) == 0) return true;
    }
    return false;
}

// The section's keys move under the canonical name and the historical section is removed. Where spellings of one key meet, the value
// ReadSetting returns is kept, so Play does not change a setting.
bool MoveToCanonical(const std::wstring& path, const IniSection& section) {
    struct Entry { std::wstring key, value; int rank; };
    std::vector<Entry> merged;
    for (const wchar_t* source : {section.name, section.legacyName}) {
        for (const auto& [key, value] : SectionPairs(path, source)) {
            if (IsObsoleteSetting(source, key)) continue;
            const std::wstring name = CanonicalKey(key);
            const int rank = (name != key ? 2 : 0) + (source == section.legacyName ? 1 : 0);
            const auto found = std::find_if(merged.begin(), merged.end(),
                                            [&](const Entry& entry) { return _wcsicmp(entry.key.c_str(), name.c_str()) == 0; });
            if (found == merged.end()) {
                merged.push_back({name, value, rank});
            } else if (rank < found->rank) {
                found->value = value;
                found->rank = rank;
            }
        }
    }
    // The profile API keeps the spelling of an existing header, so the section is deleted and written again under the canonical name.
    bool ok = WriteIniValue(path, section.legacyName, nullptr, nullptr);
    ok = WriteIniValue(path, section.name, nullptr, nullptr) && ok;
    for (const Entry& entry : merged) ok = WriteIniValue(path, section.name, entry.key.c_str(), entry.value.c_str()) && ok;
    return ok;
}

std::wstring Trim(const std::wstring& text) {
    const size_t first = text.find_first_not_of(L" \t\r");
    return first == std::wstring::npos ? std::wstring() : text.substr(first, text.find_last_not_of(L" \t\r") - first + 1);
}

// Renames the historical section and key names of launcher.ini. A file that has none of them is not written. Returns true if it was written.
bool MigrateIniNames(const std::wstring& directory) {
    const std::wstring path = directory + kLauncherIni;
    std::string text;
    if (!ReadIniText(path, text)) return false;
    std::vector<IniSection> sections;
    bool inKnownSection = false;
    bool historical = false;
    for (size_t start = 0; start <= text.size();) {
        const size_t end = min(text.find('\n', start), text.size());
        const std::wstring line = Trim(std::wstring(text.begin() + start, text.begin() + end));
        start = end + 1;
        if (line.size() >= 2 && line.front() == L'[' && line.back() == L']') {
            const std::wstring name = line.substr(1, line.size() - 2);
            inKnownSection = false;
            for (const IniSection& known : kIniSections) {
                if (_wcsicmp(name.c_str(), known.name) != 0 && _wcsicmp(name.c_str(), known.legacyName) != 0) continue;
                inKnownSection = true;
                historical = historical || name == known.legacyName;
                const bool seen = std::any_of(sections.begin(), sections.end(),
                                              [&](const IniSection& section) { return section.name == known.name; });
                if (!seen) sections.push_back(known);
            }
        } else if (inKnownSection && line.find(L'=') != std::wstring::npos) {
            const std::wstring key = Trim(line.substr(0, line.find(L'=')));
            for (const IniKeyAlias& alias : kIniKeyAliases) historical = historical || key == alias.legacy;
        }
    }
    if (!historical) return false;
    bool ok = true;
    for (const IniSection& section : sections) {
        const bool moved = MoveToCanonical(path, section);
        ok = moved && ok;
    }
    if (!ok) MessageBoxW(nullptr, L"The launcher.ini section names could not be updated to the new format.", kTitle, MB_OK | MB_ICONWARNING);
    return true;
}

bool HasIniKey(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
    const wchar_t absent[] = L"\x01";
    wchar_t value[64] = {};
    GetPrivateProfileStringW(section, key, absent, value, 64, path.c_str());
    return wcscmp(value, absent) != 0;
}

// Deletes the removed features' settings, and a section that this leaves empty. Other content stays. Returns true if the file was written.
bool RemoveObsoleteSettings(const std::wstring& directory) {
    const std::wstring path = directory + kLauncherIni;
    bool written = false, ok = true;
    for (const IniKey& setting : kObsoleteSettings) {
        if (!HasIniKey(path, setting.section, setting.key)) continue;
        written = true;
        ok = WriteIniValue(path, setting.section, setting.key, nullptr) && ok;
        if (SectionPairs(path, setting.section).empty()) ok = WriteIniValue(path, setting.section, nullptr, nullptr) && ok;
    }
    if (!ok) MessageBoxW(nullptr, L"The removed settings could not be deleted from launcher.ini.", kTitle, MB_OK | MB_ICONWARNING);
    return written;
}

// The launcher's save step for launcher.ini: renames historical names, then deletes removed settings. Returns true if the file was written.
bool MigrateIniFile(const std::wstring& directory) {
    const bool renamed = MigrateIniNames(directory);
    const bool removed = RemoveObsoleteSettings(directory);
    return renamed || removed;
}

const wchar_t* DisplayModeValue(bool borderless) { return borderless ? L"borderless" : L"windowed"; }

// A missing mode counts as Windowed, so only Borderless is written into a file without one. Any other spelling is replaced by the canonical form.
bool NeedsDisplayModeWrite(const std::wstring& directory, bool borderless) {
    std::wstring stored;
    if (!FindSetting(directory, kDisplaySection, L"mode", nullptr, stored)) return borderless;
    return stored != DisplayModeValue(borderless);
}

// The settings file is kSettingsHeader followed by JSON text with every character stored as four bytes.
std::vector<unsigned char> SeedSettings() {
    std::vector<unsigned char> data(kSettingsHeader, kSettingsHeader + sizeof(kSettingsHeader));
    for (size_t i = 0; kSettingsBody[i] != 0; ++i) {
        data.push_back(static_cast<unsigned char>(kSettingsBody[i]));
        data.insert(data.end(), 3, 0);
    }
    return data;
}

// Before the game has saved its settings once, the seeded defaults stand in for the file.
bool LoadSettings(const std::wstring& directory, std::vector<unsigned char>& data) {
    const std::wstring path = directory + kSettingsFile;
    if (!Exists(path)) {
        data = SeedSettings();
        return true;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return false;
    data.clear();
    unsigned char buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) data.insert(data.end(), buffer, buffer + count);
    const bool ok = ferror(file) == 0;
    fclose(file);
    return ok;
}

bool StoreSettings(const std::wstring& directory, const std::vector<unsigned char>& data) {
    CreateDirectoryW((directory + L"\\_sd").c_str(), nullptr);
    CreateDirectoryW((directory + L"\\_sd\\GOWSOSSAVE999").c_str(), nullptr);
    const std::wstring path = directory + kSettingsFile, temporary = path + L".tmp";
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || file == nullptr) return false;
    const bool written = fwrite(data.data(), 1, data.size(), file) == data.size();
    if (fclose(file) == 0 && written && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(temporary.c_str());
    return false;
}

// Offset of the unsigned integer stored under a key that must occur exactly once, or npos.
size_t FindInteger(const std::vector<unsigned char>& data, const char* key, size_t& digits, int& value) {
    const std::string text = std::string("\"") + key + "\":";
    std::vector<unsigned char> pattern;
    for (const char character : text) {
        pattern.push_back(static_cast<unsigned char>(character));
        pattern.insert(pattern.end(), 3, 0);
    }
    const auto found = std::search(data.begin(), data.end(), pattern.begin(), pattern.end());
    if (found == data.end() || (found - data.begin()) % 4 != 0 || std::search(found + 1, data.end(), pattern.begin(), pattern.end()) != data.end()) return std::string::npos;
    const size_t at = static_cast<size_t>(found - data.begin()) + pattern.size();
    auto unit = [&](size_t index) {
        const size_t offset = at + index * 4;
        return offset + 4 <= data.size() && data[offset + 1] == 0 && data[offset + 2] == 0 && data[offset + 3] == 0 ? data[offset] : 0;
    };
    value = 0;
    for (digits = 0; digits < 9 && unit(digits) >= '0' && unit(digits) <= '9'; ++digits) value = value * 10 + (unit(digits) - '0');
    return digits != 0 && (unit(digits) == ',' || unit(digits) == '}') ? at : std::string::npos;
}

int ReadIntSetting(const std::wstring& directory, const char* key) {
    std::vector<unsigned char> data;
    size_t digits = 0;
    int value = -1;
    return LoadSettings(directory, data) && FindInteger(data, key, digits, value) != std::string::npos ? value : -1;
}

// Rewrites only the digits of one value, in place and with the same length, so the rest of the file stays byte for byte
// the same. Nothing is written when the value is already set, so a missing file is seeded only when something changes.
bool WriteIntSetting(const std::wstring& directory, const char* key, int value) {
    std::vector<unsigned char> data;
    size_t digits = 0;
    int current = -1;
    if (!LoadSettings(directory, data)) return false;
    const size_t at = FindInteger(data, key, digits, current);
    if (at == std::string::npos) return false;
    if (current == value) return true;
    const std::string text = std::to_string(value);
    if (value < 0 || text.size() != digits) return false;
    for (size_t i = 0; i < digits; ++i) data[at + i * 4] = static_cast<unsigned char>(text[i]);
    return StoreSettings(directory, data);
}

// resolution.txt was the earlier way to choose the resolution. Its value only preselects the launcher's choice; the
// file is renamed once Play has saved a choice, so that it cannot override the launcher on later starts.
int ReadResolutionFile(const std::wstring& directory) {
    FILE* choice = nullptr;
    if (_wfopen_s(&choice, (directory + L"\\resolution.txt").c_str(), L"r") != 0 || choice == nullptr) return -1;
    char word[32] = {};
    fgets(word, sizeof(word), choice);
    fclose(choice);
    std::string value = word;
    for (auto& character : value) character = static_cast<char>(tolower(static_cast<unsigned char>(character)));
    if (value.rfind("1080", 0) == 0) return 8;
    if (value.rfind("1440", 0) == 0) return 6;
    if (value.rfind("4k", 0) == 0 || value.rfind("2160", 0) == 0) return 0;
    return -1;
}

bool IsDirectory(const std::wstring& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

FILETIME WriteTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return FILETIME{};
    return data.ftLastWriteTime;
}

long long FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return -1;
    return (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

std::wstring FinalPath(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return L"";
    wchar_t buffer[2048];
    const DWORD length = GetFinalPathNameByHandleW(handle, buffer, 2048, VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (length == 0 || length >= 2048) return L"";
    std::wstring result = buffer;
    for (auto& character : result) character = static_cast<wchar_t>(towlower(character));
    return result;
}

std::wstring PreparedStamp(const std::wstring& directory, const std::wstring& source) {
    return L"1 " + std::to_wstring(FileSize(source)) + L" " + std::to_wstring(FileSize(directory + L"\\tools\\relinker.exe"));
}

std::wstring ReadStamp(const std::wstring& path) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"r") != 0 || file == nullptr) return L"";
    wchar_t line[256] = {};
    fgetws(line, 256, file);
    fclose(file);
    std::wstring text = line;
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) text.pop_back();
    return text;
}

void WriteStamp(const std::wstring& path, const std::wstring& text) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"w") != 0 || file == nullptr) return;
    fputws(text.c_str(), file);
    fclose(file);
}

bool IsElf(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    unsigned char magic[4] = {};
    DWORD read = 0;
    const bool ok = ReadFile(file, magic, sizeof(magic), &read, nullptr) && read == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
    CloseHandle(file);
    return ok;
}

bool ExtractSelf(const std::wstring& selfPath, const std::wstring& elfPath) {
    HANDLE in = CreateFileW(selfPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE) return false;
    auto readAt = [&](unsigned long long offset, void* buffer, DWORD size) {
        LARGE_INTEGER position;
        position.QuadPart = static_cast<LONGLONG>(offset);
        DWORD got = 0;
        return SetFilePointerEx(in, position, nullptr, FILE_BEGIN) && ReadFile(in, buffer, size, &got, nullptr) && got == size;
    };
    struct Entry { unsigned long long props, offset, fileSize, memSize; };
    struct Header { unsigned type, flags; unsigned long long offset, vaddr, paddr, fileSize, memSize, align; };
    unsigned char head[0x20] = {};
    bool ok = readAt(0, head, sizeof(head)) && head[0] == 0x4f && head[1] == 0x15 && head[2] == 0x3d && head[3] == 0x1d;
    const unsigned entryCount = ok ? *reinterpret_cast<unsigned short*>(head + 0x18) : 0;
    std::vector<Entry> entries(entryCount);
    unsigned char elfHead[64] = {};
    ok = ok && entryCount != 0 && entryCount < 4096 && readAt(0x20, entries.data(), entryCount * sizeof(Entry)) &&
         readAt(0x20 + entryCount * sizeof(Entry), elfHead, sizeof(elfHead)) && elfHead[0] == 0x7f && elfHead[1] == 'E';
    std::vector<Header> headers;
    unsigned long long tableOffset = 0;
    if (ok) {
        tableOffset = *reinterpret_cast<unsigned long long*>(elfHead + 0x20);
        const unsigned count = *reinterpret_cast<unsigned short*>(elfHead + 0x38);
        headers.resize(count);
        ok = count != 0 && count < 512 && readAt(0x20 + entryCount * sizeof(Entry) + tableOffset, headers.data(), count * sizeof(Header));
    }
    HANDLE out = INVALID_HANDLE_VALUE;
    if (ok) {

        for (const auto& entry : entries) {
            if ((entry.props & 0x800) != 0 && (entry.props & 0xA) != 0) ok = false;
        }
        out = ok ? CreateFileW(elfPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
        ok = out != INVALID_HANDLE_VALUE;
    }
    auto writeAt = [&](unsigned long long offset, const void* buffer, DWORD size) {
        LARGE_INTEGER position;
        position.QuadPart = static_cast<LONGLONG>(offset);
        DWORD put = 0;
        return SetFilePointerEx(out, position, nullptr, FILE_BEGIN) && WriteFile(out, buffer, size, &put, nullptr) && put == size;
    };
    if (ok) {
        ok = writeAt(0, elfHead, sizeof(elfHead)) && writeAt(tableOffset, headers.data(), static_cast<DWORD>(headers.size() * sizeof(Header)));
        std::vector<char> chunk(1 << 22);
        unsigned long long end = 0;
        for (const auto& header : headers) end = max(end, header.offset + header.fileSize);
        for (const auto& entry : entries) {
            if (!ok || (entry.props & 0x800) == 0) continue;
            const unsigned index = static_cast<unsigned>((entry.props >> 20) & 0xfff);
            if (index >= headers.size()) continue;
            unsigned long long remaining = min(entry.fileSize, headers[index].fileSize);
            unsigned long long from = entry.offset, to = headers[index].offset;
            while (ok && remaining != 0) {
                const DWORD part = static_cast<DWORD>(min<unsigned long long>(remaining, chunk.size()));
                ok = readAt(from, chunk.data(), part) && writeAt(to, chunk.data(), part);
                from += part;
                to += part;
                remaining -= part;
            }
        }
        if (ok) {
            LARGE_INTEGER size;
            size.QuadPart = static_cast<LONGLONG>(end);
            ok = SetFilePointerEx(out, size, nullptr, FILE_BEGIN) && SetEndOfFile(out);
        }
    }
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    CloseHandle(in);
    if (!ok) DeleteFileW(elfPath.c_str());
    return ok;
}

long RunHidden(const std::wstring& command, const std::wstring& directory, const std::wstring& log) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (output != INVALID_HANDLE_VALUE) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = output;
        startup.hStdError = output;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command;
    const BOOL started = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (!started) return -1;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return static_cast<long>(code);
}

bool PngEncoderClsid(CLSID* clsid) {
    UINT count = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&count, &size);
    if (size == 0) return false;
    std::vector<char> buffer(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(count, size, encoders);
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            *clsid = encoders[i].Clsid;
            return true;
        }
    }
    return false;
}

HICON LoadGameIcon(const std::wstring& png, const std::wstring& icoPath) {
    Gdiplus::Bitmap source(png.c_str());
    if (source.GetLastStatus() != Gdiplus::Ok) return nullptr;
    Gdiplus::Bitmap scaled(256, 256, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&scaled);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.DrawImage(&source, 0, 0, 256, 256);
    }
    HICON icon = nullptr;
    scaled.GetHICON(&icon);
    CLSID encoder{};
    IStream* stream = nullptr;
    if (PngEncoderClsid(&encoder) && SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        if (scaled.Save(stream, &encoder, nullptr) == Gdiplus::Ok) {
            STATSTG stat{};
            stream->Stat(&stat, STATFLAG_NONAME);
            const auto size = static_cast<DWORD>(stat.cbSize.QuadPart);
            HGLOBAL memory = nullptr;
            if (SUCCEEDED(GetHGlobalFromStream(stream, &memory))) {
                const void* data = GlobalLock(memory);
                HANDLE file = CreateFileW(icoPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (data != nullptr && file != INVALID_HANDLE_VALUE) {
#pragma pack(push, 1)
                    struct { WORD reserved, type, count; BYTE width, height, colors, zero; WORD planes, bits; DWORD bytes, offset; } header =
                        {0, 1, 1, 0, 0, 0, 0, 1, 32, size, 22};
#pragma pack(pop)
                    DWORD written = 0;
                    WriteFile(file, &header, sizeof(header), &written, nullptr);
                    WriteFile(file, data, size, &written, nullptr);
                }
                if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
                if (data != nullptr) GlobalUnlock(memory);
            }
        }
        stream->Release();
    }
    return icon;
}

void CreateShortcut(const std::wstring& link, const std::wstring& target, const std::wstring& directory, const std::wstring& icoPath) {
    IShellLinkW* shell = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&shell)))) return;
    shell->SetPath(target.c_str());
    shell->SetWorkingDirectory(directory.c_str());
    shell->SetDescription(LAUNCHER_NAME);
    if (Exists(icoPath)) shell->SetIconLocation(icoPath.c_str(), 0);
    IPersistFile* file = nullptr;
    if (SUCCEEDED(shell->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
        file->Save(link.c_str(), TRUE);
        file->Release();
    }
    shell->Release();
}

struct WindowSearch {
    DWORD process;
    HICON icon;
    bool found;
};

BOOL CALLBACK FindGameWindow(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == search->process && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(search->icon));
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(search->icon));
        search->found = true;
    }
    return TRUE;
}

// Borderless fullscreen is AnyPS5's own F11 toggle. Once the SDL game window is the active window and presents frames
// (its title shows the FPS counter), one F11 press and release is posted to it. SDL turns them into its normal key
// events, so its fullscreen state stays consistent and F11 keeps working both ways. If the game window does not become
// the active window, nothing is sent, so keys are never sent to another window.
void RequestFullscreen(const PROCESS_INFORMATION& process) {
    for (int i = 0; i < 240 && WaitForSingleObject(process.hProcess, 250) == WAIT_TIMEOUT; ++i) {
        HWND window = GetForegroundWindow();
        DWORD owner = 0;
        wchar_t name[16] = {};
        wchar_t title[256] = {};
        GetWindowThreadProcessId(window, &owner);
        if (owner != process.dwProcessId || GetClassNameW(window, name, 16) == 0 || wcscmp(name, L"SDL_app") != 0) continue;
        if (GetWindowTextW(window, title, 256) == 0 || wcsstr(title, L" | FPS: ") == nullptr) continue;
        const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyW(VK_F11, MAPVK_VK_TO_VSC)) << 16;
        PostMessageW(window, WM_KEYDOWN, VK_F11, scan | 1);
        PostMessageW(window, WM_KEYUP, VK_F11, scan | 1 | 0xC0000000);
        return;
    }
}

bool ConvertOrCopy(const std::wstring& from, const std::wstring& to) {
    if (IsElf(from)) return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
    return ExtractSelf(from, to);
}

void PrepareModules(const std::wstring& directory, const std::wstring& prep, std::wstring& failed) {
    CreateDirectoryW(prep.c_str(), nullptr);
    CreateDirectoryW((prep + L"\\sce_module").c_str(), nullptr);
    for (const wchar_t* sub : {L"\\sce_module", L"\\Media\\Modules", L"\\Media\\Plugins"}) {
        WIN32_FIND_DATAW found;
        HANDLE search = FindFirstFileW((directory + sub + L"\\*.prx").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) continue;
        do {
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            const std::wstring target = prep + L"\\sce_module\\" + found.cFileName;
            const std::wstring source = directory + sub + L"\\" + found.cFileName;
            if (!ConvertOrCopy(source, target)) failed += (failed.empty() ? L"" : L", ") + std::wstring(found.cFileName);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
}

void LinkIntoApp0(const std::wstring& directory, const wchar_t* name) {
    const std::wstring link = directory + L"\\app0\\" + name;
    if (!Exists(directory + L"\\" + name)) return;
    if (Exists(link)) {
        if (FinalPath(link) == FinalPath(directory + L"\\" + name)) return;
        RemoveDirectoryW(link.c_str());
        if (Exists(link)) return;
    }
    CreateDirectoryW((directory + L"\\app0").c_str(), nullptr);
    RunHidden(L"cmd.exe /c mklink /J \"" + link + L"\" \"" + directory + L"\\" + name + L"\"", directory, directory + L"\\logs\\link.log");
}

}

struct Splash {
    static constexpr UINT kCloseMessage = WM_APP + 1;
    static constexpr UINT_PTR kTimer = 1;
    static constexpr UINT kGetDpiScaledSize = 0x02E4;  // WM_GETDPISCALEDSIZE (Windows 10 1703+), not declared for the default WINVER
    // The layout stops growing at 175%; the window stays DPI aware, so above that it keeps its 175% size and text is still drawn sharply.
    static constexpr UINT kMaxLayoutDpi = 168;
    HANDLE thread = nullptr;
    HWND window = nullptr;
    HANDLE ready = nullptr;
    HWND status = nullptr;
    HWND title = nullptr, intro = nullptr, progress = nullptr, elapsed = nullptr, note = nullptr;
    HFONT fonts[5] = {};
    ULONGLONG started = 0, shown = ~0ull;
    UINT currentDpi = USER_DEFAULT_SCREEN_DPI;
    template <typename Function> static Function User32(const char* name) {
        return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), name)));
    }
    static UINT Dpi(HWND hwnd) {
        static const auto forWindow = User32<UINT(WINAPI*)(HWND)>("GetDpiForWindow");
        UINT dpi = forWindow != nullptr ? forWindow(hwnd) : 0;
        if (dpi == 0) {
            HDC screen = GetDC(nullptr);
            dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSY));
            ReleaseDC(nullptr, screen);
        }
        return dpi != 0 ? dpi : USER_DEFAULT_SCREEN_DPI;
    }
    static int LayoutDpi(UINT dpi) { return static_cast<int>(min(dpi, kMaxLayoutDpi)); }
    // Grows a client rectangle to the window rectangle; the caption and borders follow the real DPI.
    void Frame(RECT* rect, UINT dpi) const {
        const DWORD style = static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)), exStyle = static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE));
        static const auto adjust = User32<BOOL(WINAPI*)(RECT*, DWORD, BOOL, DWORD, UINT)>("AdjustWindowRectExForDpi");
        if (adjust == nullptr || !adjust(rect, style, FALSE, exStyle, dpi)) AdjustWindowRectEx(rect, style, FALSE, exStyle);
    }
    // Because of the cap the size is not proportional to the DPI, so Windows is told the size the window will get on the new monitor.
    SIZE ScaledSize(UINT dpi) const {
        RECT client{};
        GetClientRect(window, &client);
        RECT frame{0, 0, MulDiv(client.right, LayoutDpi(dpi), LayoutDpi(currentDpi)), MulDiv(client.bottom, LayoutDpi(dpi), LayoutDpi(currentDpi))};
        Frame(&frame, dpi);
        return SIZE{frame.right - frame.left, frame.bottom - frame.top};
    }
    // Everything is laid out in 96-DPI units scaled by the capped layout DPI; label heights come from measuring their text.
    void Layout(UINT dpi, const RECT* suggested) {
        currentDpi = dpi;
        auto scale = [layoutDpi = LayoutDpi(dpi)](int value) { return MulDiv(value, layoutDpi, USER_DEFAULT_SCREEN_DPI); };
        // GDI only fakes FW_SEMIBOLD for "Segoe UI", so the real semibold face is named; "Please keep this window open" alone is bold.
        auto font = [&](int size, int weight, const wchar_t* face) {
            return CreateFontW(-scale(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        };
        decltype(fonts) old;
        std::copy(std::begin(fonts), std::end(fonts), old);
        fonts[0] = font(22, FW_SEMIBOLD, L"Segoe UI Semibold");
        fonts[1] = font(15, FW_SEMIBOLD, L"Segoe UI Semibold");
        fonts[2] = font(15, FW_NORMAL, L"Segoe UI");
        fonts[3] = font(13, FW_NORMAL, L"Segoe UI");
        fonts[4] = font(13, FW_BOLD, L"Segoe UI");
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(suggested != nullptr ? MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST) : MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const RECT work = monitor.rcWork;
        const int margin = scale(24), width = min(scale(500), static_cast<int>(work.right - work.left) - scale(32)) - 2 * margin;
        const struct { HWND control; HFONT font; int gap; } items[] = {
            {title, fonts[0], 0}, {intro, fonts[2], 10}, {progress, nullptr, 24}, {status, fonts[1], 16}, {elapsed, fonts[3], 2}, {note, fonts[4], 20}};
        HDC dc = GetDC(window);
        int y = margin;
        for (const auto& item : items) {
            y += scale(item.gap);
            int height = scale(14);
            if (item.font != nullptr) {
                SendMessageW(item.control, WM_SETFONT, reinterpret_cast<WPARAM>(item.font), FALSE);
                wchar_t text[512] = {};
                GetWindowTextW(item.control, text, 512);
                const bool single = (GetWindowLongW(item.control, GWL_STYLE) & SS_ENDELLIPSIS) != 0;
                RECT bounds{0, 0, width, 0};
                HGDIOBJ previous = SelectObject(dc, item.font);
                DrawTextW(dc, text[0] != 0 ? text : L"X", -1, &bounds, DT_CALCRECT | DT_NOPREFIX | DT_EXPANDTABS | (single ? DT_SINGLELINE : DT_WORDBREAK));
                SelectObject(dc, previous);
                height = bounds.bottom;
            }
            MoveWindow(item.control, margin, y, width, height, FALSE);
            y += height;
        }
        ReleaseDC(window, dc);
        for (HFONT font : old) if (font != nullptr) DeleteObject(font);
        const int clientWidth = width + 2 * margin, clientHeight = y + margin;
        RECT frame{0, 0, clientWidth, clientHeight};
        Frame(&frame, dpi);
        const int windowWidth = frame.right - frame.left, windowHeight = frame.bottom - frame.top;
        const int x = suggested != nullptr ? suggested->left : work.left + max(0, static_cast<int>(work.right - work.left) - windowWidth) / 2;
        const int top = suggested != nullptr ? suggested->top : work.top + max(0, static_cast<int>(work.bottom - work.top) - windowHeight) / 2;
        SetWindowPos(window, nullptr, x, top, windowWidth, windowHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        // If the frame the system draws differs from the computed one, correct the size once so the client area fits the content exactly.
        RECT client{};
        GetClientRect(window, &client);
        if (client.right != clientWidth || client.bottom != clientHeight)
            SetWindowPos(window, nullptr, 0, 0, windowWidth + clientWidth - client.right, windowHeight + clientHeight - client.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    void Tick() {
        const ULONGLONG seconds = (GetTickCount64() - started) / 1000;
        if (seconds == shown) return;
        shown = seconds;
        wchar_t text[64];
        const unsigned hours = static_cast<unsigned>(seconds / 3600), minutes = static_cast<unsigned>(seconds / 60 % 60), rest = static_cast<unsigned>(seconds % 60);
        if (hours == 0) std::swprintf(text, 64, L"Elapsed time: %02u:%02u", minutes, rest);
        else std::swprintf(text, 64, L"Elapsed time: %u:%02u:%02u", hours, minutes, rest);
        SetWindowTextW(elapsed, text);
    }
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* self = reinterpret_cast<Splash*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (message) {
        case WM_CLOSE: return 0;
        case kCloseMessage: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: KillTimer(hwnd, kTimer); PostQuitMessage(0); return 0;
        case WM_TIMER:
            if (wParam == kTimer && self != nullptr) self->Tick();
            return 0;
        case kGetDpiScaledSize:
            if (self == nullptr) break;
            *reinterpret_cast<SIZE*>(lParam) = self->ScaledSize(static_cast<UINT>(wParam));
            return TRUE;
        case WM_DPICHANGED:
            if (self != nullptr) self->Layout(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
            return 0;
        case WM_CTLCOLORSTATIC: {
            // Labels share the window background instead of the default grey; secondary text is muted unless high contrast is on.
            HDC dc = reinterpret_cast<HDC>(wParam);
            const HWND control = reinterpret_cast<HWND>(lParam);
            HIGHCONTRASTW contrast{};
            contrast.cbSize = sizeof(contrast);
            const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
            const bool muted = self != nullptr && (control == self->intro || control == self->elapsed);
            SetTextColor(dc, muted && !highContrast ? RGB(0x5c, 0x5c, 0x5c) : GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    static DWORD WINAPI Run(LPVOID parameter) {
        auto* self = static_cast<Splash*>(parameter);
        // Only this UI thread becomes per-monitor DPI aware, so the window is drawn sharply at the real scaling while the rest of the
        // launcher keeps its DPI mode. Windows before 10 1607 lack the API and keep scaling the window as a bitmap, as before.
        if (const auto aware = User32<DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT)>("SetThreadDpiAwarenessContext")) {
            if (aware(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == nullptr) aware(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
        }
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
        InitCommonControlsEx(&controls);
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = Proc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"SplashPrepare";
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&windowClass);
        // Created hidden on the primary monitor so its DPI is known before the final size and position are chosen.
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
        self->window = CreateWindowExW(WS_EX_TOPMOST, windowClass.lpszClassName, LAUNCHER_TITLE, WS_CAPTION, monitor.rcWork.left, monitor.rcWork.top, 0, 0, nullptr, nullptr, windowClass.hInstance, self);
        if (self->window == nullptr) {
            SetEvent(self->ready);
            return 0;
        }
        auto label = [&](const wchar_t* text, DWORD style) {
            return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_NOPREFIX | style, 0, 0, 0, 0, self->window, nullptr, windowClass.hInstance, nullptr);
        };
        self->title = label(L"Preparing the game for first launch", 0);
        self->intro = label(L"The game is being prepared from your own files\nThis only happens once, usually takes around two minutes\nand does not require an internet connection", 0);
        self->progress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 0, 0, 0, 0, self->window, nullptr, windowClass.hInstance, nullptr);
        SendMessageW(self->progress, PBM_SETMARQUEE, TRUE, 0);
        self->status = label(L"Preparing game modules...", SS_ENDELLIPSIS);
        self->elapsed = label(L"", SS_ENDELLIPSIS);
        self->note = label(L"Please keep this window open", 0);
        self->Tick();
        self->Layout(Dpi(self->window), nullptr);
        SetTimer(self->window, kTimer, 250, nullptr);
        ShowWindow(self->window, SW_SHOW);
        SetEvent(self->ready);
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        for (HFONT font : self->fonts) if (font != nullptr) DeleteObject(font);
        return 0;
    }
    void Start() {
        started = GetTickCount64();
        ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread = CreateThread(nullptr, 0, Run, this, 0, nullptr);
        WaitForSingleObject(ready, 5000);
    }
    // WM_SETTEXT from this thread is sent to the UI thread and handled there.
    void Status(const wchar_t* text) { if (status != nullptr) SetWindowTextW(status, text); }
    // Destroying the window on its own thread also stops the timer and the progress animation; safe to call more than once.
    void Close() {
        if (window != nullptr) PostMessageW(window, kCloseMessage, 0, 0);
        if (thread != nullptr) { WaitForSingleObject(thread, 2000); CloseHandle(thread); }
        if (ready != nullptr) CloseHandle(ready);
        thread = ready = nullptr;
        window = status = nullptr;
    }
    ~Splash() { Close(); }
};

struct Launcher {
    static constexpr UINT kGetDpiScaledSize = 0x02E4;  // WM_GETDPISCALEDSIZE (Windows 10 1703+), not declared for the default WINVER
    // The layout stops growing at 175%; the window stays DPI aware, so above that it keeps its 175% size and text is still drawn sharply.
    static constexpr UINT kMaxLayoutDpi = 168;
    // Physical pixels kept free at the work-area edges when the window has to shrink or move to fit.
    static constexpr int kWorkAreaMargin = 24;
    // Client area at 100%. It and every control are scaled from these 96-DPI values, never from the current size, so moving between
    // monitors cannot add up rounding errors. The Quality of Life section includes the two-line Skip dialogue hint.
    static constexpr int kQualityTop = 310, kQualityRows = 4, kSkipDialogueHint = 50;
    static constexpr int kQualityHeight = 70 + 32 * (kQualityRows - 1) + kSkipDialogueHint;
    static constexpr int kCheatsTop = kQualityTop + kQualityHeight + 12, kCheatsHeight = 66, kButtonsTop = kCheatsTop + kCheatsHeight + 16;
    static constexpr int kWidth = 620, kHeight = kButtonsTop + 48;
    // Added to the width a checkbox reports for its label so rounding never clips the last character.
    static constexpr int kCheckboxPadding = 4;
    struct Placed { HWND control; RECT bounds; HFONT* face; bool checkbox = false; };
    HWND window = nullptr;
    HWND mode = nullptr;
    HWND resolution = nullptr;
    HWND frameRate = nullptr;
    int frameRateChoice = 0;
    HWND skip = nullptr;
    HWND skipDialogueBox = nullptr;
    HWND revealFullMapBox = nullptr;
    HWND showInGameFpsBox = nullptr;
    HFONT font = nullptr, heading = nullptr;
    std::vector<Placed> placed;
    int chosen = -1;
    bool borderless = false;
    bool skipIntro = false;
    bool skipDialogue = false;
    bool revealFullMap = false;
    bool showInGameFps = false;
    bool play = false;
    template <typename Function> static Function User32(const char* name) {
        return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), name)));
    }
    static UINT Dpi(HWND hwnd) {
        static const auto forWindow = User32<UINT(WINAPI*)(HWND)>("GetDpiForWindow");
        UINT dpi = forWindow != nullptr ? forWindow(hwnd) : 0;
        if (dpi == 0) {
            HDC screen = GetDC(nullptr);
            dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSY));
            ReleaseDC(nullptr, screen);
        }
        return dpi != 0 ? dpi : USER_DEFAULT_SCREEN_DPI;
    }
    static int LayoutDpi(UINT dpi) { return static_cast<int>(min(dpi, kMaxLayoutDpi)); }
    // The layout goes below the capped DPI only as far as needed to fit the work area in the current state, and never below 100%.
    int LayoutDpi(UINT dpi, const RECT& work) const {
        int layoutDpi = LayoutDpi(dpi);
        while (layoutDpi > USER_DEFAULT_SCREEN_DPI && !Fits(WindowSize(dpi, layoutDpi), work)) --layoutDpi;
        return layoutDpi;
    }
    static bool Fits(SIZE size, const RECT& work) {
        return size.cx <= work.right - work.left - 2 * kWorkAreaMargin && size.cy <= work.bottom - work.top - 2 * kWorkAreaMargin;
    }
    static RECT WorkArea(HMONITOR monitor) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(monitor, &info);
        return info.rcWork;
    }
    // Grows a client rectangle to the window rectangle; the caption and borders follow the real DPI.
    void Frame(RECT* rect, UINT dpi) const {
        const DWORD style = static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)), exStyle = static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE));
        static const auto adjust = User32<BOOL(WINAPI*)(RECT*, DWORD, BOOL, DWORD, UINT)>("AdjustWindowRectExForDpi");
        if (adjust == nullptr || !adjust(rect, style, FALSE, exStyle, dpi)) AdjustWindowRectEx(rect, style, FALSE, exStyle);
    }
    SIZE WindowSize(UINT dpi, int layoutDpi) const {
        RECT frame{0, 0, MulDiv(kWidth, layoutDpi, USER_DEFAULT_SCREEN_DPI), MulDiv(kHeight, layoutDpi, USER_DEFAULT_SCREEN_DPI)};
        Frame(&frame, dpi);
        return SIZE{frame.right - frame.left, frame.bottom - frame.top};
    }
    // Because of the cap the size is not proportional to the DPI, so Windows is told the size the window will get on the new monitor.
    SIZE ScaledSize(UINT dpi, const RECT& work) const { return WindowSize(dpi, LayoutDpi(dpi, work)); }
    // A checkbox is laid out as wide as its column, so clicks and the focus rectangle would reach far past its label. The button
    // reports what its indicator, gap and label need in the font it was just given; the column width stays the limit.
    static int LabelWidth(HWND box, int columnWidth, int padding) {
        SIZE ideal{};
        if (!SendMessageW(box, BCM_GETIDEALSIZE, 0, reinterpret_cast<LPARAM>(&ideal)) || ideal.cx <= 0) return columnWidth;
        return min(columnWidth, static_cast<int>(ideal.cx) + padding);
    }
    // Fonts, controls and the client area use the capped layout DPI; edges are scaled rather than sizes, so edges that line up at 100% still do.
    void Layout(UINT dpi, const RECT* suggested) {
        const RECT work = WorkArea(suggested != nullptr ? MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST) : MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY));
        const int layoutDpi = LayoutDpi(dpi, work);
        auto scale = [layoutDpi](int value) { return MulDiv(value, layoutDpi, USER_DEFAULT_SCREEN_DPI); };
        // Font sizes are rounded down, so text never grows faster than the boxes it is placed in.
        auto fontHeight = [layoutDpi](int pixels) { return -(pixels * layoutDpi / USER_DEFAULT_SCREEN_DPI); };
        HFONT oldFont = font, oldHeading = heading;
        font = CreateFontW(fontHeight(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        heading = CreateFontW(fontHeight(24), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        for (const auto& item : placed) {
            SendMessageW(item.control, WM_SETFONT, reinterpret_cast<WPARAM>(*item.face), FALSE);
            const RECT& b = item.bounds;
            int width = scale(b.right) - scale(b.left);
            if (item.checkbox) width = LabelWidth(item.control, width, scale(kCheckboxPadding));
            MoveWindow(item.control, scale(b.left), scale(b.top), width, scale(b.bottom) - scale(b.top), FALSE);
        }
        if (oldFont != nullptr) DeleteObject(oldFont);
        if (oldHeading != nullptr) DeleteObject(oldHeading);
        const SIZE size = WindowSize(dpi, layoutDpi);
        const int clientWidth = scale(kWidth), clientHeight = scale(kHeight);
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (IsIconic(window) && GetWindowPlacement(window, &placement)) {
            // A minimized window keeps its place and only gets the size it is restored to.
            placement.rcNormalPosition.right = placement.rcNormalPosition.left + size.cx;
            placement.rcNormalPosition.bottom = placement.rcNormalPosition.top + size.cy;
            SetWindowPlacement(window, &placement);
        } else {
            // Windows suggests the place after a DPI change; at first the window is centered in the work area of its monitor.
            const int x = suggested != nullptr ? suggested->left : work.left + max(0L, work.right - work.left - size.cx) / 2;
            const int y = suggested != nullptr ? suggested->top : work.top + max(0L, work.bottom - work.top - size.cy) / 2;
            SetWindowPos(window, nullptr, x, y, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
            // If the frame the system draws differs from the computed one, correct the size once so the client area fits the layout exactly.
            RECT client{};
            GetClientRect(window, &client);
            if (client.right != clientWidth || client.bottom != clientHeight)
                SetWindowPos(window, nullptr, 0, 0, size.cx + clientWidth - client.right, size.cy + clientHeight - client.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        }
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<Launcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == kGetDpiScaledSize && self != nullptr) {
            *reinterpret_cast<SIZE*>(lParam) = self->ScaledSize(static_cast<UINT>(wParam), WorkArea(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)));
            return TRUE;
        }
        if (message == WM_DPICHANGED) {
            if (self != nullptr) self->Layout(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
            return 0;
        }
        if (message == WM_COMMAND && self != nullptr && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)) {
            self->play = LOWORD(wParam) == IDOK;
            self->borderless = SendMessageW(self->mode, CB_GETCURSEL, 0, 0) == 1;
            self->frameRateChoice = static_cast<int>(SendMessageW(self->frameRate, CB_GETCURSEL, 0, 0));
            self->skipIntro = SendMessageW(self->skip, BM_GETCHECK, 0, 0) == BST_CHECKED;
            self->skipDialogue = SendMessageW(self->skipDialogueBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            self->revealFullMap = SendMessageW(self->revealFullMapBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            self->showInGameFps = SendMessageW(self->showInGameFpsBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            self->chosen = static_cast<int>(SendMessageW(self->resolution, CB_GETITEMDATA, SendMessageW(self->resolution, CB_GETCURSEL, 0, 0), 0));
            DestroyWindow(hwnd);
            return 0;
        }
        if (message == WM_DESTROY) {
            if (self != nullptr) self->window = nullptr;
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    // Shows the settings and saves them when Play is pressed; false means the player closed the launcher instead.
    bool Run(const std::wstring& directory, HICON icon) {
        borderless = ReadBorderless(directory);
        skipIntro = ReadSkipIntro(directory);
        frameRateChoice = ReadFrameRate(directory);
        skipDialogue = ReadSkipDialogue(directory);
        revealFullMap = ReadRevealFullMap(directory);
        showInGameFps = ReadShowInGameFps(directory);
        // Only while the launcher window exists is this thread per-monitor DPI aware, so the window is drawn sharply at the real scaling;
        // this runs on the main thread, so its previous DPI mode is restored afterwards. Windows before 10 1607 lack the API and keep
        // scaling the window as a bitmap, as before.
        const auto aware = User32<DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT)>("SetThreadDpiAwarenessContext");
        DPI_AWARENESS_CONTEXT previous = nullptr;
        if (aware != nullptr) {
            if ((previous = aware(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) == nullptr) previous = aware(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
        }
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = Proc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"LauncherSettings";
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassW(&windowClass);
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        // Created hidden on the primary monitor so its DPI is known before the final size and position are chosen.
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
        window = CreateWindowExW(0, windowClass.lpszClassName, LAUNCHER_TITLE, style, monitor.rcWork.left, monitor.rcWork.top, 0, 0, nullptr, nullptr, windowClass.hInstance, nullptr);
        if (window == nullptr) {
            if (previous != nullptr) aware(previous);
            return true;
        }
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        if (icon != nullptr) {
            SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
            SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        }
        // Positions and sizes are the 100% layout; Layout scales them and sets the fonts.
        auto control = [&](const wchar_t* type, const wchar_t* text, DWORD flags, int x, int y, int w, int h, int id, HFONT& face) {
            HWND handle = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | flags, x, y, w, h, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), windowClass.hInstance, nullptr);
            placed.push_back(Placed{handle, RECT{x, y, x + w, y + h}, &face});
            return handle;
        };
        // w is the column the checkbox sits in; Layout narrows the control to its label so only the box and the text are clickable.
        auto checkBox = [&](const wchar_t* text, int x, int y, int w, int id) {
            HWND handle = control(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 28, id, font);
            placed.back().checkbox = true;
            return handle;
        };
        control(L"STATIC", L"God of War: Sons of Sparta", 0, 24, 16, kWidth - 48, 34, 0, heading);
        control(L"BUTTON", L"Display", BS_GROUPBOX, 24, 62, kWidth - 48, 236, 0, font);
        control(L"STATIC", L"Display mode", SS_CENTERIMAGE, 44, 92, 104, 28, 0, font);
        mode = control(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 152, 92, 244, 200, 101, font);
        control(L"STATIC", L"Resolution", SS_CENTERIMAGE, 44, 130, 104, 28, 0, font);
        resolution = control(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 152, 130, 244, 200, 100, font);
        control(L"STATIC", L"Frame rate", SS_CENTERIMAGE, 44, 168, 104, 28, 0, font);
        frameRate = control(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 152, 168, 244, 200, 103, font);
        // The help text is drawn from the top of its box; the boxes are a little taller than the text at 100% so it is not clipped at other scales.
        control(L"STATIC", L"Lower resolutions improve GPU performance.", 0, 44, 214, 352, 24, 0, font);
        control(L"STATIC", L"F11 switches between Windowed and Borderless Fullscreen while the game is running.", 0, 44, 246, 352, 48, 0, font);
        control(L"BUTTON", L"Quality of Life", BS_GROUPBOX, 24, kQualityTop, kWidth - 48, kQualityHeight, 0, font);
        showInGameFpsBox = checkBox(L"Show in-game FPS (F9)", 44, kQualityTop + 30, kWidth - 88, 113);
        skip = checkBox(L"Skip intro", 44, kQualityTop + 30 + 32, kWidth - 88, 102);
        skipDialogueBox = checkBox(L"Skip dialogue", 44, kQualityTop + 30 + 32 * 2, kWidth - 88, 111);
        // The line break keeps both lines inside the indented box at every scale.
        control(L"STATIC", L"Press A (Xbox), Cross (PlayStation),\nEnter or Space to skip the current line.", 0, 64, kQualityTop + 30 + 32 * 2 + 28 + 2,
                kWidth - 108, 48, 0, font);
        revealFullMapBox = checkBox(L"Reveal full Map", 44, kQualityTop + 30 + 32 * 3 + kSkipDialogueHint, kWidth - 88, 115);
        control(L"BUTTON", L"Cheats", BS_GROUPBOX, 24, kCheatsTop, kWidth - 48, kCheatsHeight, 0, font);
        control(L"STATIC", L"Press F10 in game to open the cheat menu", 0, 44, kCheatsTop + 30, kWidth - 88, 24, 0, font);
        HWND playButton = control(L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, kWidth / 2 - 5 - 92, kButtonsTop, 92, 30, IDOK, font);
        control(L"BUTTON", L"Exit", BS_PUSHBUTTON | WS_TABSTOP, kWidth / 2 + 5, kButtonsTop, 92, 30, IDCANCEL, font);

        SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Windowed"));
        SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Borderless Fullscreen"));
        SendMessageW(mode, CB_SETCURSEL, borderless ? 1 : 0, 0);
        SendMessageW(frameRate, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"60 FPS"));
        SendMessageW(frameRate, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"120 FPS"));
        SendMessageW(frameRate, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Unlocked (experimental)"));
        SendMessageW(frameRate, CB_SETCURSEL, frameRateChoice, 0);
        SendMessageW(skip, BM_SETCHECK, skipIntro ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(skipDialogueBox, BM_SETCHECK, skipDialogue ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(revealFullMapBox, BM_SETCHECK, revealFullMap ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(showInGameFpsBox, BM_SETCHECK, showInGameFps ? BST_CHECKED : BST_UNCHECKED, 0);

        const int fromFile = ReadResolutionFile(directory);
        const int current = fromFile >= 0 ? fromFile : ReadIntSetting(directory, kResolutionKey);
        int selected = -1;
        for (const auto& option : kResolutions) {
            const auto item = SendMessageW(resolution, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option.label));
            SendMessageW(resolution, CB_SETITEMDATA, item, option.index);
            if (option.index == current) selected = static_cast<int>(item);
        }
        if (selected < 0) {
            selected = static_cast<int>(SendMessageW(resolution, CB_INSERTSTRING, 0, reinterpret_cast<LPARAM>(L"Keep current setting")));
            SendMessageW(resolution, CB_SETITEMDATA, selected, -1);
        }
        SendMessageW(resolution, CB_SETCURSEL, selected, 0);

        Layout(Dpi(window), nullptr);
        ShowWindow(window, SW_SHOWNORMAL);
        SetForegroundWindow(window);
        SetFocus(playButton);
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (window != nullptr && IsDialogMessageW(window, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (previous != nullptr) aware(previous);
        DeleteObject(font);
        DeleteObject(heading);
        if (play && chosen >= 0) {
            if (!WriteIntSetting(directory, kResolutionKey, chosen)) {
                MessageBoxW(nullptr, L"The resolution could not be saved. The game starts with its current setting.", kTitle, MB_OK | MB_ICONWARNING);
            } else if (fromFile >= 0) {
                MoveFileExW((directory + L"\\resolution.txt").c_str(), (directory + L"\\resolution.txt.old").c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }
        // A new section is appended where it is first written, so the writes follow the window: display, startup, quality_of_life.
        bool iniChanged = false;
        const auto saveIni = [&](const wchar_t* section, const wchar_t* key, const wchar_t* value) {
            if (!WritePrivateProfileStringW(section, key, value, (directory + kLauncherIni).c_str())) return false;
            iniChanged = true;
            return true;
        };
        if (play && NeedsDisplayModeWrite(directory, borderless) && !saveIni(kDisplaySection.name, L"mode", DisplayModeValue(borderless))) {
            MessageBoxW(nullptr, L"The display mode could not be saved. It is used for this start only.", kTitle, MB_OK | MB_ICONWARNING);
        }
        if (play && skipIntro != ReadSkipIntro(directory) && !saveIni(kStartupSection.name, L"skip_intro", skipIntro ? L"1" : L"0")) {
            MessageBoxW(nullptr, L"The Skip intro setting could not be saved. It is used for this start only.", kTitle, MB_OK | MB_ICONWARNING);
        }
        if (play && skipDialogue != ReadSkipDialogue(directory) && !saveIni(kQualitySection.name, L"skip_dialogue", skipDialogue ? L"1" : L"0")) {
            MessageBoxW(nullptr, L"The Skip dialogue setting could not be saved. It is used for this start only.", kTitle, MB_OK | MB_ICONWARNING);
        }
        if (play && revealFullMap != ReadRevealFullMap(directory) && !saveIni(kQualitySection.name, L"reveal_full_map", revealFullMap ? L"1" : L"0")) {
            MessageBoxW(nullptr, L"The Reveal full Map setting could not be saved. It is used for this start only.", kTitle, MB_OK | MB_ICONWARNING);
        }
        if (play && frameRateChoice != ReadFrameRate(directory) && !saveIni(kDisplaySection.name, L"frame_rate", frameRateChoice == 1 ? L"120" : frameRateChoice == 2 ? L"unlocked" : L"60")) {
            MessageBoxW(nullptr, L"The Frame rate setting could not be saved. It is used for this start only.", kTitle, MB_OK | MB_ICONWARNING);
        }
        if (play && showInGameFps != ReadShowInGameFps(directory) && !saveIni(kQualitySection.name, L"show_in_game_fps", showInGameFps ? L"1" : L"0")) {
            MessageBoxW(nullptr, L"The Show in-game FPS setting could not be saved. It is used for this start only.", kTitle, MB_OK | MB_ICONWARNING);
        }
        if (play && MigrateIniFile(directory)) iniChanged = true;
        if (iniChanged) SeparateIniSections(directory + kLauncherIni);
        return play;
    }
};

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, LAUNCHER_MUTEX);
    if (single != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        Fail(L"The game is already running.");
        return 0;
    }

    wchar_t modulePath[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(sizeof(modulePath) / sizeof(wchar_t)));
    std::wstring directory = modulePath;
    directory.resize(directory.find_last_of(L"\\/"));
    SetCurrentDirectoryW(directory.c_str());

    const std::wstring elf = directory + L"\\prep\\eboot.elf";
    const std::wstring bin = Exists(directory + L"\\eboot.bin") ? directory + L"\\eboot.bin" : directory + L"\\eboot.elf";
    std::wstring executable;
    if (Exists(bin)) {
        CreateDirectoryW((directory + L"\\prep").c_str(), nullptr);
        const FILETIME binTime = WriteTime(bin), elfTimeNow = WriteTime(elf);
        if (!Exists(elf) || CompareFileTime(&elfTimeNow, &binTime) < 0) {
            if (!ConvertOrCopy(bin, elf)) DeleteFileW(elf.c_str());
        }
        if (Exists(elf)) executable = elf;
    }
    if (executable.empty()) {
        Fail(L"The game files were not found.\n\nCopy your own decrypted " LAUNCHER_NAME L" files (eboot.elf or eboot.bin, sce_sys, sce_module, ...) "
             L"into the same folder as this program, then start it again.\n\nFolder: " + directory);
        return 1;
    }
    if (!IsElf(executable)) {
        Fail(L"The game executable is not decrypted. Please provide a decrypted eboot.elf next to this program.");
        return 1;
    }
    {
        const std::wstring required = LAUNCHER_REQUIRED;
        std::wstring missing;
        for (size_t start = 0; start < required.size();) {
            size_t end = required.find(L';', start);
            if (end == std::wstring::npos) end = required.size();
            const std::wstring item = required.substr(start, end - start);
            if (!item.empty() && !Exists(directory + L"\\" + item)) missing += (missing.empty() ? L"" : L", ") + item;
            start = end + 1;
        }
        if (!missing.empty()) {
            Fail(L"Some game files are missing (" + missing + L"). Copy the complete game folder next to this program.");
            return 1;
        }
    }
    if (!IsDirectory(directory + L"\\libs") || !Exists(directory + L"\\tools\\relinker.exe")) {
        Fail(L"Program files are missing (libs and tools folders). Please extract the whole download again.");
        return 1;
    }

    CreateDirectoryW((directory + L"\\logs").c_str(), nullptr);

    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    HICON gameIcon = nullptr;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) == Gdiplus::Ok) {
        const std::wstring png = directory + L"\\sce_sys\\icon0.png";
        const std::wstring ico = directory + L"\\logs\\" LAUNCHER_ICON;
        if (Exists(png)) {
            gameIcon = LoadGameIcon(png, ico);
            const std::wstring link = directory + L"\\" LAUNCHER_NAME L".lnk";
            if (!Exists(link) && SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
                CreateShortcut(link, modulePath, directory, ico);
                CoUninitialize();
            }
        }
    }

    bool borderless = false;
    bool skipIntro = false;
    bool skipDialogue = false;
    bool revealFullMap = false;
    bool showInGameFps = false;
    int frameRateChoice = 0;
    {
        Launcher launcher;
        if (!launcher.Run(directory, gameIcon)) return 0;
        borderless = launcher.borderless;
        skipIntro = launcher.skipIntro;
        skipDialogue = launcher.skipDialogue;
        revealFullMap = launcher.revealFullMap;
        showInGameFps = launcher.showInGameFps;
        frameRateChoice = launcher.frameRateChoice;
    }

    const std::wstring runtime = directory + L"\\" + kRuntime;
    const std::wstring stampFile = directory + L"\\prep\\prepared.stamp";
    const std::wstring stampNow = PreparedStamp(directory, executable);
    const std::wstring stampOld = ReadStamp(stampFile);
    if (Exists(runtime) && stampOld.empty()) WriteStamp(stampFile, stampNow);
    const bool stale = !Exists(runtime) || (!stampOld.empty() && stampOld != stampNow);
    if (stale) {
        Splash splash;
        splash.Start();
        {
            std::wstring failed;
            PrepareModules(directory, directory + L"\\prep", failed);
            if (!failed.empty()) {
                splash.Close();
                Fail(L"Some game modules could not be converted (" + failed + L").\n\nThey must be decrypted files from your own copy of the game.");
                return 1;
            }
        }
        splash.Status(L"Building the game program...");
        SetFileAttributesW(runtime.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(runtime.c_str());
        const std::wstring registry = directory + L"\\" + std::wstring(kRuntime, wcslen(kRuntime) - 4) + L".registry.json";
        SetFileAttributesW(registry.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(registry.c_str());
        const std::wstring command = L"\"" + directory + L"\\tools\\relinker.exe\" --windows --registry --windows-diagnostics "

        L"--skip-syscall-check --exclude-sce-module CommonDialog.prx --exclude-sce-module VoiceInputPlugin.prx --exclude-sce-module libSceNpCppWebApi.prx "
        L"--defer-sce-module PSN.prx --defer-sce-module SaveData.prx --defer-sce-module lib_burst_generated.prx "
        L"\"" + executable + L"\" \"" + runtime + L"\"";
        const long code = RunHidden(command, directory, directory + L"\\logs\\prepare.log");
        if (code != 0 || !Exists(runtime)) {
            splash.Close();
            Fail(L"The game could not be prepared from your files (exit code " + std::to_wstring(code) + L").\n\nSee logs\\prepare.log. "
                 L"Make sure eboot.elf is the decrypted executable of " LAUNCHER_NAME L" (" LAUNCHER_TITLE_ID L").");
            return 1;
        }

        MoveFileExW((directory + L"\\windows-diagnostics-imports.txt").c_str(), (directory + L"\\logs\\windows-diagnostics-imports.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
        SetFileAttributesW(runtime.c_str(), FILE_ATTRIBUTE_HIDDEN);
        SetFileAttributesW(registry.c_str(), FILE_ATTRIBUTE_HIDDEN);
        WriteStamp(stampFile, stampNow);
    }

    for (const wchar_t* name : {L"Media", L"sce_sys"}) LinkIntoApp0(directory, name);

    for (int attempt = 1;; ++attempt) {

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE out = CreateFileW((directory + L"\\logs\\game.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE err = CreateFileW((directory + L"\\logs\\game.err").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = out;
    startup.hStdError = err;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    const std::wstring pipelineCache = directory + L"\\pipeline_cache.bin";
    SetFileAttributesW(pipelineCache.c_str(), FILE_ATTRIBUTE_NORMAL);
    {

        FILE* debug = nullptr;
        if (_wfopen_s(&debug, (directory + L"\\debug_env.txt").c_str(), L"r") == 0 && debug != nullptr) {
            wchar_t entry[512];
            while (fgetws(entry, 512, debug) != nullptr) {
                std::wstring text = entry;
                while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
                const auto equals = text.find(L'=');
                if (equals != std::wstring::npos && equals != 0 && text[0] != L'#') SetEnvironmentVariableW(text.substr(0, equals).c_str(), text.substr(equals + 1).c_str());
            }
            fclose(debug);
        }
    }
    // The runtime reads these choices from its environment; an option that is off has no variable, so an inherited value cannot turn it on.
    SetEnvironmentVariableW(L"SOS_SKIP_INTRO", skipIntro ? L"1" : nullptr);
    SetEnvironmentVariableW(L"SOS_SKIP_DIALOGUE", skipDialogue ? L"1" : nullptr);
    SetEnvironmentVariableW(L"SOS_REVEAL_FULL_MAP", revealFullMap ? L"1" : nullptr);
    SetEnvironmentVariableW(L"APS5_VBLANK_HZ", frameRateChoice >= 1 ? L"1000" : nullptr);
    SetEnvironmentVariableW(L"APS5_FPS_CAP", frameRateChoice == 1 ? L"120" : nullptr);
    SetEnvironmentVariableW(L"SOS_SHOW_IN_GAME_FPS", showInGameFps ? L"1" : nullptr);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + runtime + L"\"";
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process)) {
        Fail(L"The game could not be started.");
        return 1;
    }
    // The launcher window had the foreground; pass it on so the game window opens in front.
    AllowSetForegroundWindow(process.dwProcessId);
    CloseHandle(out);
    CloseHandle(err);
    const DWORD started = GetTickCount();
    if (gameIcon != nullptr) {

        WindowSearch search{process.dwProcessId, gameIcon, false};
        for (int i = 0; i < 240 && !search.found && WaitForSingleObject(process.hProcess, 250) == WAIT_TIMEOUT; ++i) {
            EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&search));
        }
    }
    if (borderless) RequestFullscreen(process);

    bool hung = false;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);

    const bool crashed = (code & 0xf0000000u) == 0xc0000000u && code != 0xcfffffffu;
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    SetFileAttributesW(pipelineCache.c_str(), FILE_ATTRIBUTE_HIDDEN);
    {

        char line[160];
        const int length = std::snprintf(line, sizeof(line), "attempt %d: %s, exit code 0x%lx after %lu s\r\n", attempt, hung ? "hung during startup" : "ended", code, (GetTickCount() - started) / 1000);
        HANDLE journal = CreateFileW((directory + L"\\logs\\launcher.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (journal != INVALID_HANDLE_VALUE) { DWORD put = 0; WriteFile(journal, line, static_cast<DWORD>(length), &put, nullptr); CloseHandle(journal); }
        if (hung || code != 0) CopyFileW((directory + L"\\logs\\game.err").c_str(), (directory + L"\\logs\\game.err." + std::to_wstring(attempt)).c_str(), FALSE);
    }

    if (crashed && GetTickCount() - started < 60000) {
        Fail(L"The game stopped unexpectedly (code " + std::to_wstring(code) + L").\n\nDetails are in logs\\game.err.");
    }
    return static_cast<int>(code);
    }
}
