#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_PADINPUT_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_PADINPUT_HPP

#include "SDL_events.h"
#include "SDL_gamecontroller.h"
#include "prx/libScePad/include/InputMapping.hpp"
#include "prx/libScePad/include/PadState.hpp"
#include <array>
#include <chrono>
#include <vector>

class DisplayWindow;

struct ScriptedPress {
    std::int64_t startMs = 0;
    std::int64_t durationMs = 0;
    std::uint32_t buttons = 0;
    bool stickSet = false;
    std::array<std::uint8_t, 2> stick{128, 128};
};

class PadInput {
public:
    PadInput();
    ~PadInput();
    void HandleEvent(const SDL_Event& event, DisplayWindow& window);
    void Update();
    void SetGameInputBlocked(bool blocked);

private:
    void publish();
    void setMouseMode(bool enabled);
    void openFirstAvailableController();
    void openController(int deviceIndex);
    void closeController();
    PadInputState sampleController() const;
    void loadScript();
    void applyScript(std::chrono::steady_clock::time_point now);
    void applyOutput();
    void enableSensors();

    std::vector<ScriptedPress> script;
    std::chrono::steady_clock::time_point scriptStart{};
    std::vector<std::chrono::steady_clock::time_point> scriptReleases;
    std::uint32_t scriptButtons = 0;
    bool scriptActive = false;
    bool scriptStick = false;
    std::array<std::uint8_t, 2> scriptStickAxes{128, 128};
    std::vector<Pad::InputBinding> bindings;
    std::vector<bool> pressed;
    std::vector<std::chrono::steady_clock::time_point> wheelReleaseTimes;
    std::array<std::uint8_t, 2> mouseStick{128, 128};
    std::chrono::steady_clock::time_point nextMousePoll{};
    bool mouseEnabled = false;
    bool gameInputBlocked = false;
    bool mouseModeBeforeBlock = false;
    SDL_GameController* controller = nullptr;
    PadInputState controllerState{};
    std::uint32_t outputSequence = 0;
    PadOutputState outputState{};
    bool outputPending = false;
    std::chrono::steady_clock::time_point nextRumbleRefresh{};
};

#endif
