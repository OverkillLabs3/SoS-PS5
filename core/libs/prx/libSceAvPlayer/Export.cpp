// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdint>
#include <string_view>
#include "SceTypes.hpp"
#include "prx/libSceAvPlayer/include/AvPlayer.hpp"
#include "prx/libSceAvPlayer/include/SafeTransition.hpp"
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
namespace {

inline void AvpNote(const char* name, const void* handle, long long value = -1) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() % 1000000;
    std::fprintf(stderr, "[avp-call] t=%lld %s h=%p v=%lld\n", static_cast<long long>(ms), name, handle, value);
}
}
namespace {
inline void AvpTrace(const char* name, int result = 0, bool hasResult = false) {
    static const bool on = std::getenv("APS5_TRACE_AVP") != nullptr;
    if (!on) return;
    static std::atomic<unsigned> n{0};
    const auto c = n.fetch_add(1);
    if (c < 300 || (c & (c - 1)) == 0) {
        if (hasResult) std::fprintf(stderr, "[avp] #%u %s -> %d\n", c, name, result);
        else std::fprintf(stderr, "[avp] #%u %s\n", c, name);
        std::fflush(stderr);
    }
}
}

using namespace AvPlayer;

namespace {

Player* ToPlayer(AvPlayerInternal* h) {
    return static_cast<Player*>(h);
}

std::uint32_t VideoBufferCount(std::int32_t requested) {
    return static_cast<std::uint32_t>(std::clamp(requested, 2, 16));
}

}

#pragma GCC visibility push(default)
extern "C" {

int APS5_VABI sceAvPlayerAddSource(AvPlayerInternal* h, const char* filename) {
    AvpTrace("sceAvPlayerAddSource");
    if (!h || !filename) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const int r = SafeTransition::AddSource([&] { return ToPlayer(h)->AddSource(filename, SourceTypeUnknown); }); AvpTrace("  AddSource result", r, true); return r; }
}

int APS5_VABI sceAvPlayerAddSourceEx(AvPlayerInternal* h, uint32_t uri_type, const AvPlayerSourceDetails* source_details) {
    AvpTrace("sceAvPlayerAddSourceEx");
    if (!h || uri_type != 0 || !source_details || !source_details->uri.name) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { std::fprintf(stderr, "[avp] AddSourceEx uri=%.*s type=%u\n", static_cast<int>(source_details->uri.length), source_details->uri.name, source_details->source_type); const int r = SafeTransition::AddSource([&] { return ToPlayer(h)->AddSource(std::string_view(source_details->uri.name, source_details->uri.length), source_details->source_type); }); AvpTrace("  AddSourceEx result", r, true); return r; }
}

int APS5_VABI sceAvPlayerChangeStream(AvPlayerInternal* h, uint32_t old_stream_id, uint32_t new_stream_id) {
    AvpTrace("sceAvPlayerChangeStream");
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->ChangeStream(old_stream_id, new_stream_id); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerClose(AvPlayerInternal* h) {
    AvpTrace("sceAvPlayerClose");
    AvpNote("Close", h);
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    delete ToPlayer(h);
    return SCE_OK;
}

uint64_t APS5_VABI sceAvPlayerCurrentTime(AvPlayerInternal* h) {
    AvpTrace("sceAvPlayerCurrentTime");
    if (!h) return static_cast<uint64_t>(static_cast<int64_t>(SCE_AVPLAYER_ERROR_INVALID_PARAMS));
    { const auto r_ = ToPlayer(h)->CurrentTime(); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerDisableStream(AvPlayerInternal* h, uint32_t stream_id) {
    AvpTrace("sceAvPlayerDisableStream");
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->DisableStream(stream_id); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerEnableStream(AvPlayerInternal* h, uint32_t stream_id) {
    AvpTrace("sceAvPlayerEnableStream");
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->EnableStream(stream_id); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

Bool APS5_VABI sceAvPlayerGetAudioData(AvPlayerInternal* h, AvPlayerFrameInfo* audio_info) {
    AvpTrace("sceAvPlayerGetAudioData");
    if (!h || !audio_info) return false;
    { const auto r_ = ToPlayer(h)->GetAudioData(*audio_info); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerGetStreamInfo(AvPlayerInternal* h, uint32_t stream_id, AvPlayerStreamInfo* info) {
    AvpTrace("sceAvPlayerGetStreamInfo");
    if (!h || !info) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->GetStreamInfo(stream_id, *info); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerGetStreamInfoEx(AvPlayerInternal* h, uint32_t stream_id, AvPlayerStreamInfoEx* info) {
    AvpTrace("sceAvPlayerGetStreamInfoEx");
    if (!h || !info) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->GetStreamInfoEx(stream_id, *info); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

Bool APS5_VABI sceAvPlayerGetVideoData(AvPlayerInternal* h, AvPlayerFrameInfo* video_info) {
    AvpTrace("sceAvPlayerGetVideoData");
    if (!h || !video_info) return false;
    { const auto r_ = ToPlayer(h)->GetVideoData(*video_info); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

Bool APS5_VABI sceAvPlayerGetVideoDataEx(AvPlayerInternal* h, AvPlayerFrameInfoEx* video_info) {
    AvpTrace("sceAvPlayerGetVideoDataEx");
    if (!h || !video_info) return false;
    { const auto r_ = ToPlayer(h)->GetVideoData(*video_info); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

AvPlayerInternal* APS5_VABI sceAvPlayerInit(AvPlayerInitData* init) {
    AvpTrace("sceAvPlayerInit");
    if (!init) return nullptr;
    return new Player(*init, VideoBufferCount(init->num_output_video_framebuffers));
}

int APS5_VABI sceAvPlayerInitEx(const AvPlayerInitDataEx* init_ex, AvPlayerInternal** handle) {
    AvpTrace("sceAvPlayerInitEx");
    if (!init_ex || !handle) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    AvPlayerInitData init{};
    init.memory_replacement = init_ex->memory_replacement;
    init.file_replacement = init_ex->file_replacement;
    init.event_replacement = init_ex->event_replacement;
    init.debug_level = init_ex->debug_level;
    init.num_output_video_framebuffers = init_ex->num_output_video_framebuffers;
    init.auto_start = init_ex->auto_start;
    init.default_language = init_ex->default_language;
    *handle = new Player(init, VideoBufferCount(init.num_output_video_framebuffers));
    return SCE_OK;
}

Bool APS5_VABI sceAvPlayerIsActive(AvPlayerInternal* h) {
    AvpTrace("sceAvPlayerIsActive");
    if (!h) return false;
    { const auto r_ = ToPlayer(h)->IsActive(); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerJumpToTime(AvPlayerInternal* h, uint64_t time_ms) {
    AvpNote("JumpToTime", h, static_cast<long long>(time_ms));
    AvpTrace("sceAvPlayerJumpToTime"); if (std::getenv("APS5_TRACE_AVP")) { std::fprintf(stderr, "[avp]   jump to %llu ms\n", (unsigned long long)time_ms); std::fflush(stderr); }
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->JumpToTime(time_ms); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerPause(AvPlayerInternal* h) {
    AvpTrace("sceAvPlayerPause");
    AvpNote("Pause", h);
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->Pause(); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerPostInit(AvPlayerInternal* h, const AvPlayerPostInitData* post_init) {
    AvpTrace("sceAvPlayerPostInit");
    if (!h || !post_init) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->PostInit(*post_init); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerResume(AvPlayerInternal* h) {
    AvpTrace("sceAvPlayerResume");
    AvpNote("Resume", h);
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->Resume(); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerSetAvSyncMode(AvPlayerInternal* h, uint32_t sync_mode) {
    AvpTrace("sceAvPlayerSetAvSyncMode");
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->SetAvSyncMode(sync_mode); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerSetAvailableBandwidth(AvPlayerInternal* h, uint32_t start_bandwidth, uint32_t minimum_bandwidth, uint32_t maximum_bandwidth) {
    AvpTrace("sceAvPlayerSetAvailableBandwidth");
    (void)start_bandwidth;
    (void)minimum_bandwidth;
    (void)maximum_bandwidth;
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    return SCE_OK;
}

int APS5_VABI sceAvPlayerSetLogCallback(void* callback, void* user_data) {
    AvpTrace("sceAvPlayerSetLogCallback");
    (void)callback;
    (void)user_data;
    return SCE_OK;
}

int APS5_VABI sceAvPlayerSetLooping(AvPlayerInternal* h, Bool loop) {
    AvpNote("SetLooping", h, loop ? 1 : 0);
    AvpTrace("sceAvPlayerSetLooping"); if (std::getenv("APS5_TRACE_AVP")) { std::fprintf(stderr, "[avp]   loop=%d\n", (int)loop); std::fflush(stderr); }
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->SetLooping(loop != 0); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerSetTrickSpeed(AvPlayerInternal* h, int32_t trick_speed) {
    AvpTrace("sceAvPlayerSetTrickSpeed");
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->SetTrickSpeed(trick_speed); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerStart(AvPlayerInternal* h) {
    AvpTrace("sceAvPlayerStart");
    AvpNote("Start", h);
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->Start(); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerStartEx(AvPlayerInternal* h, const void* start_info_ex) {
    AvpTrace("sceAvPlayerStartEx");
    (void)start_info_ex;
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->Start(); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerStop(AvPlayerInternal* h) {
    AvpTrace("sceAvPlayerStop");
    AvpNote("Stop", h);
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->Stop(); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

int APS5_VABI sceAvPlayerStreamCount(AvPlayerInternal* h) {
    AvpTrace("sceAvPlayerStreamCount");
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    { const auto r_ = ToPlayer(h)->StreamCount(); AvpTrace("    ret", static_cast<int>(r_), true); return r_; }
}

}
#pragma GCC visibility pop
