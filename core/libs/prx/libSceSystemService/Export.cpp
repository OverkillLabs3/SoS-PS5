#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "prx/libc/include/Shutdown.hpp"
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libSceSystemService/SystemService.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
namespace {

// Sons of Sparta Skip intro (SOS_SKIP_INTRO=1): UiScripts.SplashScreen.Start (<Start>d__20.MoveNext in Il2cppUserAssemblies)
// goes from its first state straight to its own ending, so the Sony screen, the startup video and its music never start. The ending
// still marks the game as booted, saves, loads the FMOD banks and KS_World; it now first waits for the save system and the settings
// defaults, which the presentation used to outlast. Runs when this library loads, before any game code. Offsets are module RVAs.
bool SkipStartupPresentation() {
    const char* option = std::getenv("SOS_SKIP_INTRO");
    if (option == nullptr || std::strcmp(option, "1") != 0) return false;

    constexpr std::uint32_t coroutine = 0xc61f40, coroutineEnd = 0xc62d00;
    constexpr std::uint64_t coroutineHash = 0x9f43bcbea9a0a0d9;
    constexpr std::uint32_t firstState = 0xc6205a;  // case 0, after <>1__state = -1
    constexpr std::uint32_t ending = 0xc624f7;      // after the video loop
    constexpr std::uint32_t saveCheck = 0xc6252f;   // if (saveSystem == null || !saveSystem.isInitialized) skip the booted mark
    constexpr std::uint32_t saveReady = 0xc6253a;
    constexpr std::uint32_t nextFrame = 0xc62b5b;   // yield null as state 4, which resumes into the ending while the video is not playing
    constexpr std::uint32_t userSkip = 0xc62479;    // the video loop's skip branch, unreachable without the video

    auto* game = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(L"Il2cppUserAssemblies.prx.guest.prx"));
    const auto* headers = game != nullptr ? reinterpret_cast<const IMAGE_NT_HEADERS*>(game + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew) : nullptr;

    // Only the analysed game build is patched (FNV-1a of the whole coroutine, which has no relocations); anything else plays the intro.
    std::uint64_t hash = 0xcbf29ce484222325;
    if (headers != nullptr && headers->OptionalHeader.SizeOfImage >= coroutineEnd) {
        for (std::uint32_t offset = coroutine; offset < coroutineEnd; ++offset) hash = (hash ^ game[offset]) * 0x100000001b3;
    }
    if (hash != coroutineHash) {
        std::fprintf(stderr, "Skip intro: the startup screen code was not recognised, the intro plays\n");
        return false;
    }

    const auto jump = [game](std::uint32_t from, std::uint32_t to) {
        const auto distance = static_cast<std::int32_t>(to - (from + 5));
        game[from] = 0xe9;
        std::memcpy(game + from + 1, &distance, sizeof(distance));
    };
    // rax = save system: wait unless isInitialized (+0x11) and currentGlobalData (+0x20)->previouslyInitializedToDefaults (+0x1c),
    // followed by jmp saveReady and wait: jmp nextFrame.
    constexpr std::uint8_t waitForSaveSystem[] = {0x48, 0x85, 0xc0, 0x74, 0x1a, 0x80, 0x78, 0x11, 0x01, 0x75, 0x14, 0x48, 0x8b, 0x48,
                                                  0x20, 0x48, 0x85, 0xc9, 0x74, 0x0b, 0x80, 0x79, 0x1c, 0x00, 0x74, 0x05};

    DWORD protection = 0;
    if (!VirtualProtect(game + coroutine, coroutineEnd - coroutine, PAGE_EXECUTE_READWRITE, &protection)) return false;

    std::memcpy(game + userSkip, waitForSaveSystem, sizeof(waitForSaveSystem));
    jump(userSkip + sizeof(waitForSaveSystem), saveReady);
    jump(userSkip + sizeof(waitForSaveSystem) + 5, nextFrame);
    jump(saveCheck, userSkip);
    jump(firstState, ending);

    VirtualProtect(game + coroutine, coroutineEnd - coroutine, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), game + coroutine, coroutineEnd - coroutine);
    return true;
}

[[maybe_unused]] const bool startupPresentationSkipped = SkipStartupPresentation();

}
#endif

extern "C" {

int APS5_VABI sceSystemServiceLoadExec(const char* path, const char* const* arguments) {
    if (!path || !*path) return SYSTEM_SERVICE_ERROR_PARAMETER;
    if (std::strcmp(path, "exit") != 0) {
        NotImplemented_nid_no_patch("sceSystemServiceLoadExec: executable replacement");
    }
    (void)arguments;
    LibcRunShutdown_nid_postfix();
    std::exit(0);
}

int APS5_VABI sceSystemServiceDisableNoticeScreenSkipFlagAutoSet(void) {
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceGetDisplaySafeAreaInfo(SystemServiceDisplaySafeAreaInfo* info) {
 if (info == nullptr) {
  return SYSTEM_SERVICE_ERROR_PARAMETER;
 }
 *info = SystemServiceDisplaySafeAreaInfo{};
 info->ratio = 1.0f;
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceGetHdrToneMapLuminance(SystemServiceHdrToneMapLuminance* luminance) {
 if (luminance == nullptr) {
  return SYSTEM_SERVICE_ERROR_PARAMETER;
 }
 constexpr float SdrReferenceWhiteNits = 100.0f;
 luminance->max_full_frame_tone_map_luminance = SdrReferenceWhiteNits;
 luminance->max_tone_map_luminance = SdrReferenceWhiteNits;
 luminance->min_tone_map_luminance = 0.0f;
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceGetNoticeScreenSkipFlag(bool* value) {
 if (value == nullptr) {
  return SYSTEM_SERVICE_ERROR_PARAMETER;
 }
 *value = false;
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceGetStatus(SystemServiceStatus* status) {
 if (status == nullptr) {
  return SYSTEM_SERVICE_ERROR_PARAMETER;
 }
 *status = SystemServiceStatus{};
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceHideSplashScreen(void) {
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceParamGetInt(int paramId, int* value) {
 if (value == nullptr) {
  return SYSTEM_SERVICE_ERROR_PARAMETER;
 }
 switch (paramId) {
  case SYSTEM_SERVICE_PARAM_ID_LANG: *value = SYSTEM_SERVICE_PARAM_LANG_ENGLISH_US; break;
  case SYSTEM_SERVICE_PARAM_ID_DATE_FORMAT: *value = SYSTEM_SERVICE_PARAM_DATE_FORMAT_DDMMYYYY; break;
  case SYSTEM_SERVICE_PARAM_ID_TIME_FORMAT: *value = SYSTEM_SERVICE_PARAM_TIME_FORMAT_24HOUR; break;
  case SYSTEM_SERVICE_PARAM_ID_TIME_ZONE: *value = 0; break;
  case SYSTEM_SERVICE_PARAM_ID_SUMMERTIME: *value = 0; break;
  case SYSTEM_SERVICE_PARAM_ID_GAME_PARENTAL_LEVEL: *value = SYSTEM_SERVICE_PARAM_GAME_PARENTAL_OFF; break;
  case SYSTEM_SERVICE_PARAM_ID_ENTER_BUTTON_ASSIGN: *value = SYSTEM_SERVICE_PARAM_ENTER_BUTTON_CROSS; break;
  default: *value = 0; break;
 }
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceParamGetString(int param_id, char* buf, size_t buf_size) {
 (void)param_id;
 (void)buf;
 (void)buf_size;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceSystemServicePowerTick(void) {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceSystemServiceReceiveEvent(SystemServiceEvent* event) {
 if (event == nullptr) {
  return SYSTEM_SERVICE_ERROR_PARAMETER;
 }

 event->event_type = -1;
 return SYSTEM_SERVICE_ERROR_NO_EVENT;
}

int APS5_VABI sceSystemServiceReportAbnormalTermination(const void* info) {
 (void)info;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceSystemServiceSetNoticeScreenSkipFlag(void) {
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceInitializePlayerDialogParam(void* param) {
 if (param == nullptr) return SYSTEM_SERVICE_ERROR_PARAMETER;
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceLaunchPlayerDialog(const void* param) {
 if (param == nullptr) return SYSTEM_SERVICE_ERROR_PARAMETER;
 return SYSTEM_SERVICE_OK;
}

}
