#ifndef LAUNCHER_XBOX_PROMPT_ASSETS_HPP
#define LAUNCHER_XBOX_PROMPT_ASSETS_HPP

#include <string>

enum class XboxPromptAssetsResult { Patched, AlreadyPatched, Failed };

// Writes a patched copy of the game's Addressables icon bundle to outputBundle when its Xbox prompt set lacks the left-stick glyphs.
// gameFolder is only read. report receives one line per step, for the log.
XboxPromptAssetsResult PatchXboxPromptAssets(const std::wstring& gameFolder, const std::wstring& outputBundle, std::string& report);

#endif
