#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/AsyncDraw.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include <chrono>
#include <cstdlib>
#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"

namespace AgcDriver::DriverDetail {

std::uint64_t& Driver::epochSequence() {
    static thread_local std::uint64_t sequence = 0;
    return sequence;
}

bool Driver::asyncDraws() {
    static const bool enabled = std::getenv("APS5_SYNC_DRAWS") == nullptr;
    return enabled;
}

static std::unique_ptr<DrawBackend>& drawBackendSlot() {
    static thread_local std::unique_ptr<DrawBackend> backend;
    return backend;
}

DrawBackend& Driver::drawBackend(std::uint32_t queue) {
    auto& slot = drawBackendSlot();
    if (slot == nullptr) {
        slot = std::make_unique<DrawBackend>([queue] {
            char role[40];
            std::snprintf(role, sizeof role, "draw backend 0x%x", queue);
            PinWorkerThread(role);

            if (queue == 0 && std::getenv("APS5_SAMPLE_BACKEND") != nullptr) StartWorkerSampler();
            GuestMemory::TagGpuLockThread(queue);
        });
    }
    return *slot;
}

std::atomic<std::uint64_t> g_backendBusyNs{0};
std::uint64_t g_drainWaitNs = 0;
std::uint64_t g_drains = 0;

void Driver::drainDraws() {
    auto& slot = drawBackendSlot();
    if (slot == nullptr) return;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) {
        slot->Drain();
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    slot->Drain();
    g_drainWaitNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    ++g_drains;
}

void Driver::releaseDrawBackend() {
    drawBackendSlot().reset();
}

bool Driver::drawsPending() {
    auto& slot = drawBackendSlot();
    return slot != nullptr && !slot->Idle();
}

bool Driver::ordersNothing(std::uint32_t opcode) {
    switch (opcode) {

        case 0x42: case 0x46:
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x26: case 0x2a: case 0x2f:
        case 0x63: case 0x64: case 0x9f: case 0x69: case 0x76: case 0x79: case 0x7a: return true;
        default: return false;
    }
}

void Driver::recordAsyncDraw(AsyncDraw& task) {
    struct Done {
        Driver& driver;
        ~Done() {
            --driver.packetsInFlight;
            ++driver.packetsDone;
        }
    } done{*this};
    struct BusyTimer {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~BusyTimer() { g_backendBusyNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()); }
    } busyTimer;
    static thread_local std::uint64_t seenEpoch = 0;
    if (task.epochSeq != seenEpoch) {
        seenEpoch = task.epochSeq;
        GuestMemory::BumpCollectEpoch();
    }
    try {
        std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::defer_lock);
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        gpuLock.lock();
        auto localDevice = task.device;

        if (auto current = device.Load(); current != nullptr && current != localDevice) localDevice = std::move(current);
        recordLabelsForPacket(localDevice.get(), task.queue);
        noteDrawWriters(task.stages, task.queue);
        const auto& graphics = task.decode->state;
        GuestMemory::SetTraceFrame(task.frame);
        if (task.recipe != nullptr) {
            if (localDevice->DrawFromRecipe(graphics, task.drawParameters, task.stages, task.snapshots, task.recipe) == RecipeOutcome::Recorded) return;
            VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, VulkanDevice::RecipeKind::Draw);
        }
        std::shared_ptr<const DrawRecipe> built;
        localDevice->Draw(graphics, task.drawParameters, task.stages, task.snapshots, task.recipeStages.empty() ? nullptr : &built);
        if (built != nullptr) attachDrawRecipe(task.drawKey, task.recipeStages, std::move(built));
    } catch (const std::exception& error) {
        Graphics::CountDrawSkipReason(error.what());
        char suffix[80];
        std::snprintf(suffix, sizeof(suffix), " [recorded draw, color target 0x%llx]", static_cast<unsigned long long>(task.color));
        reportSkip("draw", std::string(error.what()) + suffix);
    }
}

}
