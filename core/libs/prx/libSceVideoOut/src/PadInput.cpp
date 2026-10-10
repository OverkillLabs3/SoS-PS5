#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include "SDL.h"
#include "prx/libSceVideoOut/include/PadInput.hpp"
#include "prx/libSceKeyboard/include/KeyboardState.hpp"
#include "prx/libSceVideoOut/include/DisplayWindow.hpp"
#include "prx/libScePad/include/PadState.hpp"
#include "prx/libScePad/include/PadInputTypes.hpp"
#include "prx/libc/include/General.hpp"

PadInput::PadInput()
    : bindings(Pad::LoadInputMapping()), pressed(bindings.size()), wheelReleaseTimes(bindings.size()) {
    openFirstAvailableController();
    loadScript();
}

PadInput::~PadInput() {
    closeController();
}

void PadInput::SetGameInputBlocked(bool blocked) {
    if (blocked == gameInputBlocked) return;
    gameInputBlocked = blocked;
    std::fill(pressed.begin(), pressed.end(), false);
    std::fill(wheelReleaseTimes.begin(), wheelReleaseTimes.end(), std::chrono::steady_clock::time_point{});
    if (blocked) {
        mouseModeBeforeBlock = mouseEnabled;
        if (mouseEnabled) setMouseMode(false);
    } else if (mouseModeBeforeBlock) {
        mouseModeBeforeBlock = false;
        setMouseMode(true);
    }
    publish();
}

void PadInput::openFirstAvailableController() {
    if (controller != nullptr) return;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    if ((SDL_WasInit(SDL_INIT_GAMECONTROLLER) & SDL_INIT_GAMECONTROLLER) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
            APS5_LOG_ERR("Pad: SDL game controller init failed: %s", SDL_GetError());
            return;
        }
    }
    for (int deviceIndex = 0; deviceIndex < SDL_NumJoysticks(); ++deviceIndex) {
        if (!SDL_IsGameController(deviceIndex)) continue;
        openController(deviceIndex);
        if (controller != nullptr) return;
    }
}

void PadInput::openController(int deviceIndex) {
    if (controller != nullptr || !SDL_IsGameController(deviceIndex)) return;
    controller = SDL_GameControllerOpen(deviceIndex);
    if (controller == nullptr) {
        APS5_LOG_ERR("Pad: could not open game controller %d: %s", deviceIndex, SDL_GetError());
        return;
    }
    const char* name = SDL_GameControllerName(controller);
    APS5_LOG_OUT("Pad: connected game controller: %s (type %d, sensors accel=%d gyro=%d, touchpads=%d, led=%d, trigger rumble=%d)",
        name != nullptr ? name : "unknown", static_cast<int>(SDL_GameControllerGetType(controller)),
        SDL_GameControllerHasSensor(controller, SDL_SENSOR_ACCEL) == SDL_TRUE, SDL_GameControllerHasSensor(controller, SDL_SENSOR_GYRO) == SDL_TRUE,
        SDL_GameControllerGetNumTouchpads(controller), SDL_GameControllerHasLED(controller) == SDL_TRUE, SDL_GameControllerHasRumbleTriggers(controller) == SDL_TRUE);
    enableSensors();
    outputPending = true;
}

void PadInput::enableSensors() {
    if (controller == nullptr) return;
    const SDL_bool wanted = outputState.motionEnabled ? SDL_TRUE : SDL_FALSE;
    if (SDL_GameControllerHasSensor(controller, SDL_SENSOR_ACCEL) == SDL_TRUE) SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_ACCEL, wanted);
    if (SDL_GameControllerHasSensor(controller, SDL_SENSOR_GYRO) == SDL_TRUE) SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_GYRO, wanted);
}

void PadInput::closeController() {
    if (controller == nullptr) return;
    SDL_GameControllerClose(controller);
    controller = nullptr;
    controllerState = {};
}

namespace {

constexpr std::array<std::pair<const char*, std::uint32_t>, 15> ScriptButtons{{
    {"cross", static_cast<std::uint32_t>(Pad::PadButton::Cross)},
    {"circle", static_cast<std::uint32_t>(Pad::PadButton::Circle)},
    {"square", static_cast<std::uint32_t>(Pad::PadButton::Square)},
    {"triangle", static_cast<std::uint32_t>(Pad::PadButton::Triangle)},
    {"up", static_cast<std::uint32_t>(Pad::PadButton::Up)},
    {"down", static_cast<std::uint32_t>(Pad::PadButton::Down)},
    {"left", static_cast<std::uint32_t>(Pad::PadButton::Left)},
    {"right", static_cast<std::uint32_t>(Pad::PadButton::Right)},
    {"options", static_cast<std::uint32_t>(Pad::PadButton::Options)},
    {"l1", static_cast<std::uint32_t>(Pad::PadButton::L1)},
    {"r1", static_cast<std::uint32_t>(Pad::PadButton::R1)},
    {"l2", static_cast<std::uint32_t>(Pad::PadButton::L2)},
    {"r2", static_cast<std::uint32_t>(Pad::PadButton::R2)},
    {"l3", static_cast<std::uint32_t>(Pad::PadButton::L3)},
    {"r3", static_cast<std::uint32_t>(Pad::PadButton::R3)},
}};

}

void PadInput::loadScript() {
    const char* path = std::getenv("APS5_PAD_SCRIPT");
    if (path == nullptr) return;
    std::FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        APS5_LOG_ERR("Pad: script %s could not be opened: %s", path, std::strerror(errno));
        return;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        char* text = std::strchr(line, '#');
        if (text != nullptr) *text = '\0';
        long long start = 0;
        long long duration = 0;
        if (std::sscanf(line, "%lld %lld", &start, &duration) != 2) continue;
        ScriptedPress press;
        press.startMs = start;
        press.durationMs = duration;
        char* rest = line;
        for (int skip = 0; skip < 2 && rest != nullptr; ++skip) rest = std::strchr(rest, ' ');
        if (rest == nullptr) continue;
        for (char* token = std::strtok(rest, " \t\r\n"); token != nullptr; token = std::strtok(nullptr, " \t\r\n")) {
            const bool axis = std::strncmp(token, "lx=", 3) == 0 || std::strncmp(token, "ly=", 3) == 0;
            if (axis) {
                const int value = std::atoi(token + 3);
                press.stick[token[1] == 'x' ? 0 : 1] = static_cast<std::uint8_t>(std::clamp(value, 0, 255));
                press.stickSet = true;
                continue;
            }
            for (const auto& [name, bit] : ScriptButtons) {
                if (std::strcmp(token, name) == 0) press.buttons |= bit;
            }
        }
        script.push_back(press);
    }
    std::fclose(file);
    scriptReleases.resize(script.size());
    scriptStart = std::chrono::steady_clock::now();
    APS5_LOG_OUT("Pad: script %s loaded, %zu presses", path, script.size());
}

void PadInput::applyScript(std::chrono::steady_clock::time_point now) {
    scriptButtons = 0;
    scriptStick = false;
    scriptStickAxes = {128, 128};
    bool active = false;
    for (std::size_t index = 0; index < script.size(); ++index) {
        const auto& press = script[index];
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - scriptStart).count();
        if (elapsed < press.startMs) {
            active = true;
            continue;
        }
        if (elapsed >= press.startMs + press.durationMs) {
            scriptReleases[index] = {};
            continue;
        }
        active = true;
        if (scriptReleases[index] == std::chrono::steady_clock::time_point{}) {
            scriptReleases[index] = now;
            APS5_LOG_OUT("Pad: script press at %lld ms for %lld ms: 0x%05x", static_cast<long long>(press.startMs),
                static_cast<long long>(press.durationMs), press.buttons);
        }
        scriptButtons |= press.buttons;
        if (press.stickSet) {
            scriptStick = true;
            scriptStickAxes = press.stick;
        }
    }
    scriptActive = active;
}

void PadInput::applyOutput() {
    PadOutputState fetched;
    if (PadFetchOutput_nid_postfix(&outputSequence, &fetched)) {
        const bool motionChanged = fetched.motionEnabled != outputState.motionEnabled;
        outputState = fetched;
        outputPending = true;
        if (motionChanged) enableSensors();
    }
    if (controller == nullptr) return;
    const auto now = std::chrono::steady_clock::now();
    const bool rumbling = outputState.vibrationLarge != 0 || outputState.vibrationSmall != 0;
    const bool triggerRumble = outputState.trigger[0].fallback != 0 || outputState.trigger[1].fallback != 0;
    const bool isPs5 = SDL_GameControllerGetType(controller) == SDL_CONTROLLER_TYPE_PS5;
    if (!outputPending) {
        if ((rumbling || (triggerRumble && !isPs5)) && now >= nextRumbleRefresh) outputPending = true;
        else return;
    }
    outputPending = false;
    nextRumbleRefresh = now + std::chrono::milliseconds(700);
    constexpr Uint32 rumbleMs = 2000;
    SDL_GameControllerRumble(controller, static_cast<Uint16>(outputState.vibrationLarge * 257), static_cast<Uint16>(outputState.vibrationSmall * 257), rumbling ? rumbleMs : 0);
    if (SDL_GameControllerHasLED(controller) == SDL_TRUE) {
        if (outputState.lightBarValid) SDL_GameControllerSetLED(controller, outputState.lightBar[0], outputState.lightBar[1], outputState.lightBar[2]);
        else SDL_GameControllerSetLED(controller, 0, 64, 255);
    }
    if (outputState.triggerTouched) {
        if (isPs5) {
            Uint8 effect[47] = {};
            effect[0] = 0x04 | 0x08;
            std::memcpy(effect + 10, outputState.trigger[1].effect, 11);
            std::memcpy(effect + 21, outputState.trigger[0].effect, 11);
            SDL_GameControllerSendEffect(controller, effect, sizeof(effect));
        } else if (SDL_GameControllerHasRumbleTriggers(controller) == SDL_TRUE) {
            SDL_GameControllerRumbleTriggers(controller, static_cast<Uint16>(outputState.trigger[0].fallback * 257), static_cast<Uint16>(outputState.trigger[1].fallback * 257), triggerRumble ? rumbleMs : 0);
        }
    }
}

void PadInput::setMouseMode(bool enabled) {
    if (SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE) != 0) throw std::runtime_error(std::string("Pad: relative mouse mode failed: ") + SDL_GetError());
    int deltaX = 0;
    int deltaY = 0;
    SDL_GetRelativeMouseState(&deltaX, &deltaY);
    mouseEnabled = enabled;
    mouseStick = {128, 128};
    nextMousePoll = std::chrono::steady_clock::now() + std::chrono::milliseconds(Pad::MousePollIntervalMs);
}

PadInputState PadInput::sampleController() const {
    PadInputState result;
    if (controller == nullptr) return result;
    const auto readButton = [this](SDL_GameControllerButton button) {
        return SDL_GameControllerGetButton(controller, button) != 0;
    };
    const auto addButton = [&result, &readButton](SDL_GameControllerButton source, Pad::PadButton button) {
        if (readButton(source)) result.buttons |= static_cast<std::uint32_t>(button);
    };
    addButton(SDL_CONTROLLER_BUTTON_A, Pad::PadButton::Cross);
    addButton(SDL_CONTROLLER_BUTTON_B, Pad::PadButton::Circle);
    addButton(SDL_CONTROLLER_BUTTON_X, Pad::PadButton::Square);
    addButton(SDL_CONTROLLER_BUTTON_Y, Pad::PadButton::Triangle);
    addButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, Pad::PadButton::L1);
    addButton(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, Pad::PadButton::R1);
    // PS4/PS5 pads report Share/Create as BACK and have a real touchpad; elsewhere BACK is View/Select.
    const auto type = SDL_GameControllerGetType(controller);
    if (type != SDL_CONTROLLER_TYPE_PS4 && type != SDL_CONTROLLER_TYPE_PS5) addButton(SDL_CONTROLLER_BUTTON_BACK, Pad::PadButton::TouchPad);
    addButton(SDL_CONTROLLER_BUTTON_START, Pad::PadButton::Options);
    addButton(SDL_CONTROLLER_BUTTON_LEFTSTICK, Pad::PadButton::L3);
    addButton(SDL_CONTROLLER_BUTTON_RIGHTSTICK, Pad::PadButton::R3);
    addButton(SDL_CONTROLLER_BUTTON_DPAD_UP, Pad::PadButton::Up);
    addButton(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, Pad::PadButton::Right);
    addButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, Pad::PadButton::Down);
    addButton(SDL_CONTROLLER_BUTTON_DPAD_LEFT, Pad::PadButton::Left);
    addButton(SDL_CONTROLLER_BUTTON_TOUCHPAD, Pad::PadButton::TouchPad);

    const auto triggerValue = [this](SDL_GameControllerAxis axis) {
        const auto value = std::clamp<int>(SDL_GameControllerGetAxis(controller, axis), 0, 32767);
        return static_cast<std::uint8_t>((value * 255 + 16383) / 32767);
    };
    result.analogButtonsL2 = triggerValue(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    result.analogButtonsR2 = triggerValue(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    if (result.analogButtonsL2 != 0) result.buttons |= static_cast<std::uint32_t>(Pad::PadButton::L2);
    if (result.analogButtonsR2 != 0) result.buttons |= static_cast<std::uint32_t>(Pad::PadButton::R2);

    const auto stickValue = [this](SDL_GameControllerAxis axis) {
        const auto value = static_cast<std::int32_t>(SDL_GameControllerGetAxis(controller, axis)) + 32768;
        return static_cast<std::uint8_t>((value * 255 + 32767) / 65535);
    };
    result.sticks = {
        stickValue(SDL_CONTROLLER_AXIS_LEFTX),
        stickValue(SDL_CONTROLLER_AXIS_LEFTY),
        stickValue(SDL_CONTROLLER_AXIS_RIGHTX),
        stickValue(SDL_CONTROLLER_AXIS_RIGHTY)
    };
    switch (SDL_GameControllerGetType(controller)) {
        case SDL_CONTROLLER_TYPE_PS5: result.deviceKind = 1; break;
        case SDL_CONTROLLER_TYPE_PS4: result.deviceKind = 2; break;
        default: result.deviceKind = 3; break;
    }
    if (SDL_GameControllerIsSensorEnabled(controller, SDL_SENSOR_ACCEL) == SDL_TRUE && SDL_GameControllerIsSensorEnabled(controller, SDL_SENSOR_GYRO) == SDL_TRUE) {
        float accel[3];
        float gyro[3];
        if (SDL_GameControllerGetSensorData(controller, SDL_SENSOR_ACCEL, accel, 3) == 0 && SDL_GameControllerGetSensorData(controller, SDL_SENSOR_GYRO, gyro, 3) == 0) {
            result.hasMotion = true;
            for (int i = 0; i < 3; ++i) { result.accel[i] = accel[i]; result.gyro[i] = gyro[i]; }
        }
    }
    if (SDL_GameControllerGetNumTouchpads(controller) > 0) {
        for (int finger = 0; finger < 2; ++finger) {
            Uint8 down = 0;
            float x = 0.0f;
            float y = 0.0f;
            float pressure = 0.0f;
            if (SDL_GameControllerGetTouchpadFinger(controller, 0, finger, &down, &x, &y, &pressure) != 0 || down == 0) continue;
            result.touch[finger].active = true;
            result.touch[finger].x = static_cast<std::uint16_t>(std::clamp(x, 0.0f, 1.0f) * 1919.0f);
            result.touch[finger].y = static_cast<std::uint16_t>(std::clamp(y, 0.0f, 1.0f) * 942.0f);
        }
    }
    return result;
}

void PadInput::HandleEvent(const SDL_Event& event, DisplayWindow& window) {
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
        openController(event.cdevice.which);
        return;
    }
    if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller != nullptr) {
        const auto instanceId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
        if (instanceId == event.cdevice.which) {
            closeController();
            openFirstAvailableController();
            publish();
        }
        return;
    }
    if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP ||
        event.type == SDL_CONTROLLERAXISMOTION || event.type == SDL_CONTROLLERDEVICEREMAPPED) {
        publish();
        return;
    }
    if (event.type == SDL_WINDOWEVENT && (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST || event.window.event == SDL_WINDOWEVENT_CLOSE)) {
        std::fill(pressed.begin(), pressed.end(), false);
        std::fill(wheelReleaseTimes.begin(), wheelReleaseTimes.end(), std::chrono::steady_clock::time_point{});
        if (mouseEnabled) setMouseMode(false);
        publish();
        return;
    }

    static const bool ignoreInput = std::getenv("APS5_NO_PAD_INPUT") != nullptr;
    if (ignoreInput && event.type == SDL_MOUSEWHEEL) return;
    if (event.type == SDL_MOUSEWHEEL) {
        if (gameInputBlocked) return;
        int direction = (event.wheel.y > 0) - (event.wheel.y < 0);
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) direction = -direction;
        if (direction == 0) return;
        const auto releaseTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(Pad::WheelPressDurationMs);
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            const auto& binding = bindings[index];
            if (binding.wheelDirection == 0) continue;
            pressed[index] = binding.wheelDirection == direction;
            wheelReleaseTimes[index] = pressed[index] ? releaseTime : std::chrono::steady_clock::time_point{};
        }
        publish();
        return;
    }
    const bool keyboard = event.type == SDL_KEYDOWN || event.type == SDL_KEYUP;
    const bool mouse = event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP;
    if (!keyboard && !mouse) return;
    if (ignoreInput) return;
    if (keyboard && event.key.repeat != 0) return;
    const bool down = event.type == SDL_KEYDOWN || event.type == SDL_MOUSEBUTTONDOWN;
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        const auto& binding = bindings[index];
        const bool matches = keyboard
            ? binding.key != SDL_SCANCODE_UNKNOWN && binding.key == event.key.keysym.scancode
            : binding.mouseButton != Pad::MouseButton::None && binding.mouseButton == static_cast<Pad::MouseButton>(event.button.button);

        if (!matches) continue;
        if (binding.control == Pad::InputControl::ToggleFullscreen) {
            if (keyboard && down && !pressed[index] && window.Handle() != nullptr && event.key.windowID == SDL_GetWindowID(window.Handle())) window.ToggleFullscreen();
        } else if (gameInputBlocked) {
            continue;
        }
        if (binding.control == Pad::InputControl::ToggleMouse && down && !pressed[index]) setMouseMode(!mouseEnabled);
        pressed[index] = down;
    }
    publish();
}

void PadInput::Update() {
    if (controller != nullptr) SDL_GameControllerUpdate();
    applyOutput();
    const auto now = std::chrono::steady_clock::now();
    if (!script.empty()) applyScript(now);
    if (controller != nullptr || scriptActive) {
        if (controller != nullptr) controllerState = sampleController();
        publish();
    }
    bool released = false;
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        if (bindings[index].wheelDirection == 0 || !pressed[index] || now < wheelReleaseTimes[index]) continue;
        pressed[index] = false;
        wheelReleaseTimes[index] = {};
        released = true;
    }
    if (released) publish();
    if (!mouseEnabled) return;
    if (SDL_GetKeyboardFocus() == nullptr) {
        std::fill(pressed.begin(), pressed.end(), false);
        std::fill(wheelReleaseTimes.begin(), wheelReleaseTimes.end(), std::chrono::steady_clock::time_point{});
        setMouseMode(false);
        publish();
        return;
    }
    if (now < nextMousePoll) return;
    nextMousePoll = now + std::chrono::milliseconds(Pad::MousePollIntervalMs);
    int deltaX = 0;
    int deltaY = 0;
    SDL_GetRelativeMouseState(&deltaX, &deltaY);
    mouseStick = {128, 128};
    if (deltaX != 0 || deltaY != 0) {
        const double distance = std::hypot(deltaX, deltaY);
        const double scale = std::clamp(distance * Pad::MouseSensitivity + 16.0, 64.0, 128.0) / distance;
        const auto mapAxis = [scale](int delta) { return static_cast<std::uint8_t>(std::clamp(128L + std::lround(delta * scale), 0L, 255L)); };
        mouseStick = {mapAxis(deltaX), mapAxis(deltaY)};
    }
    publish();
}

void PadInput::publish() {
    PadInputState state;
    state.buttons = controllerState.buttons;
    state.sticks = controllerState.sticks;
    state.analogButtonsL2 = controllerState.analogButtonsL2;
    state.analogButtonsR2 = controllerState.analogButtonsR2;
    state.hasMotion = controllerState.hasMotion;
    state.accel = controllerState.accel;
    state.gyro = controllerState.gyro;
    state.touch = controllerState.touch;
    state.deviceKind = controllerState.deviceKind;

    std::array<bool, 4> negative{};
    std::array<bool, 4> positive{};

    static const bool keysAsPad = std::getenv("APS5_KEYS_AS_PAD") != nullptr;
    static const bool pureKeyboard = std::getenv("APS5_IME_PURE") != nullptr;
    const bool keyboardVisible = !keysAsPad && KeyboardIsOpen_nid_postfix();
    const bool keyboardOwned = keyboardVisible && pureKeyboard;
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        if (!pressed[index]) continue;
        const auto& binding = bindings[index];
        if (keyboardOwned && binding.control != Pad::InputControl::ToggleMouse && binding.control != Pad::InputControl::ToggleFullscreen) continue;
        switch (binding.control) {
            case Pad::InputControl::Button:
                state.buttons |= static_cast<std::uint32_t>(binding.button);
                if (binding.button == Pad::PadButton::L2) state.analogButtonsL2 = 255;
                if (binding.button == Pad::PadButton::R2) state.analogButtonsR2 = 255;
                break;
            case Pad::InputControl::LeftStickLeft: negative[0] = true; break;
            case Pad::InputControl::LeftStickRight: positive[0] = true; break;
            case Pad::InputControl::LeftStickUp: negative[1] = true; break;
            case Pad::InputControl::LeftStickDown: positive[1] = true; break;
            case Pad::InputControl::RightStickLeft: negative[2] = true; break;
            case Pad::InputControl::RightStickRight: positive[2] = true; break;
            case Pad::InputControl::RightStickUp: negative[3] = true; break;
            case Pad::InputControl::RightStickDown: positive[3] = true; break;
            case Pad::InputControl::TouchLeft: state.touchLeft = true; break;
            case Pad::InputControl::TouchRight: state.touchRight = true; break;
            case Pad::InputControl::ToggleMouse: break;
            case Pad::InputControl::ToggleFullscreen: break;
        }
    }
    for (std::size_t axis = 0; axis < state.sticks.size(); ++axis) {
        if (negative[axis] || positive[axis]) state.sticks[axis] = negative[axis] == positive[axis] ? 128 : negative[axis] ? 0 : 255;
    }

    if (keyboardVisible) {
        state.hasMotion = false;
        state.accel = {};
        state.gyro = {};
    }
    if (mouseEnabled) {
        state.sticks[2] = mouseStick[0];
        state.sticks[3] = mouseStick[1];
    }
    if (state.analogButtonsL2 != 0) state.buttons |= static_cast<std::uint32_t>(Pad::PadButton::L2);
    if (state.analogButtonsR2 != 0) state.buttons |= static_cast<std::uint32_t>(Pad::PadButton::R2);
    state.buttons |= scriptButtons;
    if ((scriptButtons & static_cast<std::uint32_t>(Pad::PadButton::L2)) != 0) state.analogButtonsL2 = 255;
    if ((scriptButtons & static_cast<std::uint32_t>(Pad::PadButton::R2)) != 0) state.analogButtonsR2 = 255;
    if (scriptStick) {
        state.sticks[0] = scriptStickAxes[0];
        state.sticks[1] = scriptStickAxes[1];
    }
    if (gameInputBlocked) {
        const auto deviceKind = state.deviceKind;
        state = PadInputState{};
        state.deviceKind = deviceKind;
    }
    PadPublishInput_nid_postfix(state);
}
