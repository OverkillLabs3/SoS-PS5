#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_VULKANDEVICE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_VULKANDEVICE_HPP

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "Recompiler.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceAgcDriver/Execution/include/DisplayBuffer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Recipe.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <functional>
#include <memory>
#include <optional>

namespace AgcDriver {

namespace Graphics {
class StorageTexture;
}

struct PreparedDispatch;

struct RecipeHit;

struct RecordedDispatch;

class VulkanDevice {
public:
    explicit VulkanDevice(const PresentationWindow* window = nullptr);
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;
    ShaderRecompiler::SpirvTarget Target() const;
    ShaderRecompiler::SpirvTarget ComputeTarget(std::uint32_t waveSize) const;

    std::uint64_t Serial() const { return serial; }

    VkDevice Device() const;

    void WaitIdle();
    void PrepareForReplacement();

    void SubmitRecorded(bool reapFirst = true);

    bool CanWaitUnlocked() const;
    std::uint64_t SubmitAndEpoch();
    void WaitRecorded(std::uint64_t serial);
    void ReapRecorded(std::uint64_t serial);

    void ReapRecorded();

    int WriteLabelOnGpu(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue, bool reapFirst = true);

    std::optional<Graphics::Recorder::LabelHit> PendingLabel(std::uint64_t address, std::size_t bytes, std::uint64_t afterStamp, Graphics::Recorder::LabelRefusal* refusal = nullptr) const;
    bool OpenWriteOverlaps(std::uint64_t address, std::size_t bytes) const;

    bool FillBuffer(std::uint64_t address, std::size_t bytes, std::span<const std::uint32_t, 4> pattern);

    struct CopyOutcome {

        int path;
        bool synced;
        double syncMs;

        enum Refusal { None = 0, Size, Image, SourcePending, SourceUnsettled, DestinationPending, DestinationUnsettled, Label, Reader, Shadow, Refusals };
        int reason;

        bool sourceSettledBySignal;
        bool waitedForSource;
        double waitMs;

        std::uint64_t readerSerial;
        std::uint32_t readerQueue;
        int readerKind;
        bool readerOpen;
    };
    using CopyWriterNote = std::function<void(std::span<const std::byte> value, std::uint64_t generation)>;
    CopyOutcome CopyBuffer(std::uint64_t destination, std::uint64_t source, std::size_t bytes, std::size_t cpuMax, std::size_t gpuMax, std::size_t knownMax, std::uint64_t programAddress, std::uint32_t queue, const CopyWriterNote& noteWriter);

    struct CopyVerification {
        std::uint64_t verified, sourceChanged, destinationChanged, readerMissed;
    };
    static CopyVerification CopyVerifyCounts();

    struct CopyAliasing {
        std::uint64_t aliased, created, noSource, shape, refused;
    };
    static CopyAliasing CopyAliasCounts();
    void ResolveMemory(std::uint64_t address, std::size_t bytes, bool writable);
    void* Window() const;
    void Resize(std::uint32_t width, std::uint32_t height);
    bool Presentable() const;
    void SetPresentationOverlay(PresentationOverlay* overlay);
    bool PrimitiveListRestart() const;

    static std::size_t FlipInFlight();
    double RetirePresents(std::size_t keepInFlight);

    bool PresentWaitsForSlots(const DisplayBuffer* buffer) const;
    bool AcquireImage();
    bool PresentClear(std::uint32_t width, std::uint32_t height, bool opaque);
    void PresentPixels(std::uint32_t width, std::uint32_t height, std::span<const std::byte> pixels);
    bool PresentDisplayBuffer(const DisplayBuffer& buffer);
    double FinishPresent();
    void QueuePresent();

    void FlipBatches(std::uint64_t& submissions, std::uint64_t& unsignaled) const;

    struct PresentStatistics {
        std::uint64_t presents, gpuUnread, batchesAheadOfBlit, batchesAfterFlip, readsChecked, readsOverwritten;
        double gpuBusyMs, gpuGapMs, lastSubmitAfterFlipMs, blitSubmitAfterFlipMs;
    };
    static PresentStatistics PresentCounts();

    std::shared_ptr<PreparedDispatch> PrepareDispatch(const ShaderRecompiler::RecompileResult& shader, std::span<const Graphics::GuestMemorySnapshot> snapshots);

    static std::span<const double, 5> PreparePhaseMs(const PreparedDispatch& prepared);

    void Dispatch(const ShaderRecompiler::RecompileResult& shader, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::span<const Graphics::GuestMemorySnapshot> snapshots = {}, std::uint64_t programAddress = 0, std::shared_ptr<PreparedDispatch> prepared = nullptr, std::shared_ptr<const Recipe>* recipe = nullptr);

    struct IndirectOutcome {
        int cpuReason;
        double argumentReadMs;
    };
    IndirectOutcome DispatchIndirect(const ShaderRecompiler::RecompileResult& shader, std::uint64_t arguments, std::span<const Graphics::GuestMemorySnapshot> snapshots = {}, std::uint64_t programAddress = 0, std::shared_ptr<PreparedDispatch> prepared = nullptr, std::shared_ptr<const Recipe>* recipe = nullptr);

    std::shared_ptr<RecipeHit> PrepareRecipe(const std::shared_ptr<const Recipe>& recipe, bool indirect);

    RecipeOutcome DispatchRecipe(const ShaderRecompiler::RecompileResult& shader, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint64_t arguments, std::uint64_t programAddress, const std::shared_ptr<RecipeHit>& hit, IndirectOutcome& outcome, const std::shared_ptr<PreparedDispatch>& verify = nullptr, bool refreshByWords = false);

    static bool DispatchRecipes();
    static bool VerifyRecipes();

    static bool TemplateDataRefresh();

    enum class RecipeEvent { Restart, Attach };
    enum class RecipeKind : std::size_t { Dispatch, Indirect, Draw };
    static void NoteRecipe(RecipeEvent event, bool indirect);
    static void NoteRecipe(RecipeEvent event, RecipeKind kind);
    enum class DrawRecipePrecheck : std::size_t { NoRecipe, Device };
    static void NoteDrawRecipeMiss(DrawRecipePrecheck miss);

    void Draw(const Graphics::State& graphics, const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> shaders, std::span<const Graphics::GuestMemorySnapshot> snapshots = {}, std::shared_ptr<const DrawRecipe>* recipe = nullptr);
    std::optional<std::string> KnownDrawRejection(const Graphics::State& graphics, std::span<const Graphics::CompiledShader> shaders) const;
    void ColorMetadataPass(const Graphics::ColorMetadataPass& pass);

    RecipeOutcome DrawFromRecipe(const Graphics::State& graphics, const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> shaders, std::span<const Graphics::GuestMemorySnapshot> snapshots, const std::shared_ptr<const DrawRecipe>& recipe);

    struct IndirectDrawSupport {
        bool firstInstance;
        bool multi;
        bool count;
    };
    IndirectDrawSupport DrawIndirectSupport() const;
    void EnqueueDraw(const Graphics::State& graphics, const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> shaders, std::span<const Graphics::GuestMemorySnapshot> snapshots = {});

private:

    Graphics::Context graphicsContext() const;
    Graphics::Context buildContext() const;

    IndirectOutcome dispatch(const ShaderRecompiler::RecompileResult& shader, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint64_t arguments, std::span<const Graphics::GuestMemorySnapshot> snapshots, std::uint64_t programAddress, std::shared_ptr<PreparedDispatch> prepared, std::shared_ptr<const Recipe>* recipe);

    std::uint64_t presync(std::span<const std::pair<std::uint64_t, std::uint64_t>> surfaces);

    void decideIndirect(RecordedDispatch& record, IndirectOutcome& outcome, char* groupsText);

    void recordDispatch(RecordedDispatch& record);
    bool present(std::uint32_t width, std::uint32_t height, bool opaque, std::span<const std::byte> pixels, const DisplayBuffer* display = nullptr, const std::shared_ptr<Graphics::StorageTexture>& resident = nullptr, VkFilter residentFilter = VK_FILTER_LINEAR, bool dumpFrame = false, bool residentCopy = false);
    struct State;
    std::unique_ptr<State> state;
    std::uint64_t serial;
};

}

#endif
