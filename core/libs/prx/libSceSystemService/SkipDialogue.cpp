#ifdef _WIN32
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <windows.h>
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libSceSystemService/SkipDialogue.hpp"

namespace {

// Skip dialogue (SOS_SKIP_DIALOGUE=1): a Cross press during a spoken Pixel Crushers line runs the game's own continue,
// ConversationView.HandleContinueButtonClick. Hooks are patched into Il2cppUserAssemblies when this library loads; offsets are module RVAs.

std::uintptr_t game = 0;
std::uintptr_t gameSize = 0;

// The hooked method's argument registers and return address, as the trampoline pushed them.
struct Saved {
    std::uint64_t rdi, rsi, rdx, rcx, r8, r9, rax, r10, r11, ret;
};

enum class Use : std::uint8_t {
    Skip,   // Skip dialogue needs it
    Scene,  // the cutscene fast-forward needs it; patched only with Skip dialogue
};

struct Site {
    const char* name;
    std::uint32_t begin, end;  // the whole method, hashed before patching
    std::uint64_t hash;
    std::uint8_t copied;  // first whole instructions moved to the trampoline
    bool ripCompare;      // followed by cmp byte ptr [rip + disp32], imm8, moved with an absolute address
    Use use;
    void (*on)(const Saved&);
};

struct Range {
    std::uint32_t begin, end;
};

// Code that is called, not patched, hashed the same way.
struct Code {
    Range range;
    std::uint64_t hash;
};

std::uint64_t Pointer(std::uint64_t object, std::size_t offset) {
    return object == 0 ? 0 : *reinterpret_cast<const std::uint64_t*>(object + offset);
}

int Int(std::uint64_t object, std::size_t offset, int missing = -1) {
    return object == 0 ? missing : *reinterpret_cast<const std::int32_t*>(object + offset);
}

// System.String: int32 length at +0x10, UTF-16 at +0x14.
bool Contains(std::uint64_t string, const char* text) {
    const int length = Int(string, 0x10);
    if (length < 0) return false;
    const auto* chars = reinterpret_cast<const char16_t*>(string + 0x14);
    const std::size_t size = std::strlen(text), count = static_cast<std::size_t>(length);
    for (std::size_t start = 0; start + size <= count; ++start) {
        std::size_t matched = 0;
        while (matched < size && chars[start + matched] == static_cast<char16_t>(text[matched])) ++matched;
        if (matched == size) return true;
    }
    return false;
}

constexpr std::size_t kSubtitleSequence = 0x28, kSubtitleEntry = 0x38;
constexpr std::size_t kEntryConversation = 0x20;
constexpr std::size_t kViewSequencer = 0x38;
constexpr std::size_t kIteratorState = 0x10, kIteratorThis = 0x20;
constexpr std::size_t kCommandSequencer = 0x28;

std::uint64_t hookFrame = 0;  // the game's rbp when the current hook was entered

constexpr Range kConversationMixerProcessFrame{0x5516f0, 0x551e10};
constexpr Range kContinueMixerProcessFrame{0x550dc0, 0x5514d0};

// Walks the game's rbp chain from the hooked call outwards, only while it stays on this thread's stack, until visit returns true.
template <typename Visit> bool WalkCallers(std::uint64_t ret, int depth, Visit&& visit) {
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const auto low = reinterpret_cast<std::uint64_t>(tib->StackLimit), high = reinterpret_cast<std::uint64_t>(tib->StackBase);
    std::uint64_t frame = hookFrame;
    for (int index = 0; index < depth; ++index) {
        if (visit(ret, frame)) return true;
        if (frame < low || frame + 16 > high || (frame & 7) != 0) return false;
        const auto* slots = reinterpret_cast<const std::uint64_t*>(frame);
        ret = slots[1];
        if (slots[0] <= frame) {
            return index + 1 < depth && visit(ret, 0);
        }
        frame = slots[0];
    }
    return false;
}

bool InRange(std::uint64_t address, Range range) { return address >= game + range.begin && address < game + range.end; }

// True when the hooked call runs inside the invocation of the method in range whose frame pointer is invocationFrame.
bool InsideInvocation(std::uint64_t ret, Range range, std::uint64_t invocationFrame) {
    bool inside = false;
    WalkCallers(ret, 16, [&](std::uint64_t address, std::uint64_t frame) {
        if (!InRange(address, range)) return false;
        inside = invocationFrame != 0 && frame == invocationFrame;
        return true;
    });
    return inside;
}

// Methods that start or continue conversations for scripted content; a conversation started from none of them is gameplay dialogue.
constexpr Range kScriptedCallers[] = {
    kConversationMixerProcessFrame,  // ConversationMixerBehaviour.ProcessFrame
    kContinueMixerProcessFrame,      // ContinueConversationMixerBehaviour.ProcessFrame
    {0xe22380, 0xe22420},            // EventModule_StartConversation.ApplyModule
    {0xe00440, 0xe00770},            // EventModule_InvokeBark, BarkRandomEntryData.PlayRandomBark
    {0x7762f0, 0x776390},            // LivingIchorBossScript.PlayBark
    {0x596500, 0x5970c0},            // DialogueSystemTrigger.DoConversationAction
    {0x59e600, 0x59e990},            // ConversationStarter.StartConversation
    {0x5a7a00, 0x5a7c60},            // StartConversationOnDialogueEvent.DoAction
    {0x53de30, 0x53e200},            // Sequencer.HandleContinueInternally
    {0x8dd300, 0x8de1a0},            // <TeleportToPosition>d__197.MoveNext
    {0x569b30, 0x56a6b0},            // <StartSavedConversation>d__6.MoveNext
};

bool StartedByGameplay(std::uint64_t ret) {
    return !WalkCallers(ret, 8, [](std::uint64_t address, std::uint64_t) {
        for (const Range range : kScriptedCallers) {
            if (InRange(address, range)) return true;
        }
        return false;
    });
}

constexpr Code kHandleContinueButtonClick{{0x54d7b0, 0x54d8b0}, 0xa75befa12e27b024};
using HandleContinueButtonClick = void(APS5_VABI*)(std::uint64_t view, const void* method);

bool skipAvailable = false;
// The spoken line Cross may skip; only the game thread touches these.
std::uint64_t skipView = 0;
std::uint64_t skipSequencer = 0;

void ArmLine(std::uint64_t view, std::uint64_t subtitle) {
    const bool spoken = Contains(Pointer(subtitle, kSubtitleSequence), "FMODWait(");
    skipView = spoken ? view : 0;
    skipSequencer = spoken ? Pointer(view, kViewSequencer) : 0;
    SkipDialogue::SetLineActive(spoken);
}

void DisarmLine() {
    skipView = 0;
    skipSequencer = 0;
    SkipDialogue::SetLineActive(false);
}

// Cutscene fast-forward: when the skipped line was started by a long single-root timeline, that timeline's root playable runs at
// kSceneFfSpeed until its ContinueConversation track reaches the continue meant for the skipped line, which is dropped because Cross already
// continued. Everything here runs on the game thread.

struct Handle {  // UnityEngine.Playables.PlayableHandle or PlayableGraph
    std::uint64_t pointer;
    std::uint32_t version, padding;
};

bool Same(const Handle& a, const Handle& b) { return a.pointer == b.pointer && a.version == b.version; }

struct Playables {
    bool(APS5_VABI* handleValid)(const Handle*);
    void(APS5_VABI* handleGraph)(const Handle*, Handle*);
    bool(APS5_VABI* graphValid)(const Handle*);
    bool(APS5_VABI* graphPlaying)(const Handle*);
    int(APS5_VABI* rootCount)(const Handle*);
    void(APS5_VABI* root)(const Handle*, int, Handle*);
    std::uint64_t(APS5_VABI* resolver)(const Handle*);
    double(APS5_VABI* getSpeed)(const Handle*);
    void(APS5_VABI* setSpeed)(const Handle*, double);
    double(APS5_VABI* getDuration)(const Handle*);
};
Playables playables{};
int playablesState = 0;  // 0 unresolved, 1 ready, -1 missing

constexpr std::uint32_t kResolveIcall = 0x35fc90;
using ResolveIcall = std::uint64_t(APS5_VABI*)(const char* name);

template <typename Function> bool Resolve(Function& function, const char* name) {
    function = reinterpret_cast<Function>(reinterpret_cast<ResolveIcall>(game + kResolveIcall)(name));
    return function != nullptr;
}

// Resolved on first use, from the game thread, once the engine has registered its internal calls.
bool PlayablesReady() {
    if (playablesState != 0) return playablesState == 1;
    bool found = Resolve(playables.handleValid, "UnityEngine.Playables.PlayableHandle::IsValid_Injected(UnityEngine.Playables.PlayableHandle&)");
    found &= Resolve(playables.handleGraph,
        "UnityEngine.Playables.PlayableHandle::GetGraph_Injected(UnityEngine.Playables.PlayableHandle&,UnityEngine.Playables.PlayableGraph&)");
    found &= Resolve(playables.graphValid, "UnityEngine.Playables.PlayableGraph::IsValid_Injected(UnityEngine.Playables.PlayableGraph&)");
    found &= Resolve(playables.graphPlaying, "UnityEngine.Playables.PlayableGraph::IsPlaying_Injected(UnityEngine.Playables.PlayableGraph&)");
    found &= Resolve(playables.rootCount, "UnityEngine.Playables.PlayableGraph::GetRootPlayableCount_Injected(UnityEngine.Playables.PlayableGraph&)");
    found &= Resolve(playables.root, "UnityEngine.Playables.PlayableGraph::GetRootPlayableInternal_Injected(UnityEngine.Playables.PlayableGraph&,"
                                     "System.Int32,UnityEngine.Playables.PlayableHandle&)");
    found &= Resolve(playables.resolver, "UnityEngine.Playables.PlayableGraph::GetResolver_Injected(UnityEngine.Playables.PlayableGraph&)");
    found &= Resolve(playables.getSpeed, "UnityEngine.Playables.PlayableHandle::GetSpeed_Injected(UnityEngine.Playables.PlayableHandle&)");
    found &= Resolve(playables.setSpeed, "UnityEngine.Playables.PlayableHandle::SetSpeed_Injected(UnityEngine.Playables.PlayableHandle&,System.Double)");
    found &= Resolve(playables.getDuration, "UnityEngine.Playables.PlayableHandle::GetDuration_Injected(UnityEngine.Playables.PlayableHandle&)");
    playablesState = found ? 1 : -1;
    if (!found) std::fprintf(stderr, "Skip dialogue: the cutscene playback calls were not found, cutscenes keep their pace\n");
    return found;
}

bool sceneAvailable = false;

// A conversation the timeline's ConversationMixerBehaviour started, recognised by that ProcessFrame invocation being on the stack.
struct Owner {
    std::uint64_t view, controller, mixer;
    Handle playable;
    int conversation;
    bool open;
};
Owner owners[4] = {};
std::size_t nextOwner = 0;

struct Invocation {
    std::uint64_t frame, mixer;
    Handle playable;
};
Invocation conversationMixerInvocation{};

struct SeenMixer {
    std::uint64_t mixer;
    Handle playable;
    std::uint64_t seenMs;
};
SeenMixer continueMixers[8] = {};

void RememberOwner(std::uint64_t view, std::uint64_t controller, const Invocation* invocation) {
    const Owner owner{view, controller, invocation != nullptr ? invocation->mixer : 0, invocation != nullptr ? invocation->playable : Handle{}, -1, true};
    for (Owner& entry : owners) {
        if (entry.view == view) {
            entry = owner;
            return;
        }
    }
    owners[nextOwner++ % std::size(owners)] = owner;
}

Owner* OwnerOf(std::uint64_t view) {
    for (Owner& entry : owners) {
        if (view != 0 && entry.view == view) return &entry;
    }
    return nullptr;
}

bool ControllerOpen(std::uint64_t controller) {
    for (const Owner& owner : owners) {
        if (owner.controller == controller && owner.open) return true;
    }
    return false;
}

void NoteContinueMixer(std::uint64_t mixer, const Handle& playable) {
    const std::uint64_t now = GetTickCount64();
    SeenMixer* oldest = &continueMixers[0];
    for (SeenMixer& entry : continueMixers) {
        if (entry.mixer == mixer) {
            entry.playable = playable;
            entry.seenMs = now;
            return;
        }
        if (entry.seenMs < oldest->seenMs) oldest = &entry;
    }
    *oldest = {mixer, playable, now};
}

constexpr double kSceneFfMinTimelineDurationSeconds = 15.0;

struct SceneOwner {
    bool cinematic;
    Handle graph, root, continuePlayable;
    std::uint64_t director, continueMixer, controller;
};

// Read-only. The line is cinematic only when its timeline graph has one valid root and a director, has exactly one ContinueConversation
// mixer running, plays, and lasts at least kSceneFfMinTimelineDurationSeconds; every other case fails closed.
SceneOwner ResolveOwner(std::uint64_t view) {
    SceneOwner scene{};
    if (!sceneAvailable || !PlayablesReady()) return scene;
    const Owner* owner = OwnerOf(view);
    if (owner == nullptr || owner->mixer == 0) return scene;
    for (const Owner& other : owners) {
        if (other.open && other.view != view && other.view != 0 && other.conversation >= 0 && other.conversation == owner->conversation) return scene;
    }
    if (!playables.handleValid(&owner->playable)) return scene;
    playables.handleGraph(&owner->playable, &scene.graph);
    if (!playables.graphValid(&scene.graph)) return scene;
    scene.director = playables.resolver(&scene.graph);
    if (playables.rootCount(&scene.graph) != 1) return scene;
    playables.root(&scene.graph, 0, &scene.root);
    if (!playables.handleValid(&scene.root) || scene.director == 0) return scene;
    int continueCandidates = 0;
    const std::uint64_t now = GetTickCount64();
    for (const SeenMixer& entry : continueMixers) {
        if (entry.mixer == 0 || now - entry.seenMs > 1000 || !playables.handleValid(&entry.playable)) continue;
        Handle graph{};
        playables.handleGraph(&entry.playable, &graph);
        if (!Same(graph, scene.graph)) continue;
        ++continueCandidates;
        scene.continueMixer = entry.mixer;
        scene.continuePlayable = entry.playable;
    }
    if (continueCandidates != 1 || !playables.graphPlaying(&scene.graph)) return scene;
    scene.controller = owner->controller;
    scene.cinematic = playables.getSpeed(&scene.root) > 0.0 && playables.getDuration(&scene.root) >= kSceneFfMinTimelineDurationSeconds;
    return scene;
}

// Slow enough that the timeline still samples short ContinueConversation clips on its way.
constexpr double kSceneFfSpeed = 10.0;
constexpr std::uint64_t kSceneFfTimeoutMs = 3000;

struct SceneFF {
    bool active;
    Handle graph, root, continuePlayable;
    std::uint64_t continueMixer, director;
    double originalSpeed;
    std::uint64_t startMs;
    int pendingContinues;            // one per Cross press on this owner
    std::uint64_t sourceController;  // the conversation of the latest skipped line
    bool sourceConversationClosed;
};
SceneFF sceneFF{};

bool SceneFFRootValid() { return playables.graphValid(&sceneFF.graph) && playables.handleValid(&sceneFF.root); }

// Restores the original speed unless the root is gone or something else changed it meanwhile.
void EndSceneFF() {
    if (SceneFFRootValid() && playables.getSpeed(&sceneFF.root) == kSceneFfSpeed) playables.setSpeed(&sceneFF.root, sceneFF.originalSpeed);
    sceneFF.active = false;
    sceneFF.pendingContinues = 0;
}

void CheckSceneFF() {
    if (!sceneFF.active) return;
    if (!SceneFFRootValid() || !playables.graphPlaying(&sceneFF.graph) || GetTickCount64() - sceneFF.startMs > kSceneFfTimeoutMs) EndSceneFF();
}

// The continue that skipped the line may already have closed its conversation.
void SetSceneFFSource(const SceneOwner& scene) {
    sceneFF.sourceController = scene.controller;
    sceneFF.sourceConversationClosed = !ControllerOpen(scene.controller);
}

// The owner was classified before the continue ran, so it is checked again right before its speed changes.
void StartSceneFF(const SceneOwner& scene) {
    const bool valid = playables.graphValid(&scene.graph) && playables.handleValid(&scene.root) && playables.rootCount(&scene.graph) == 1;
    const double speed = valid ? playables.getSpeed(&scene.root) : 0.0;
    if (!(speed > 0.0) || speed == kSceneFfSpeed) return;
    sceneFF = {true, scene.graph, scene.root, scene.continuePlayable, scene.continueMixer, scene.director, speed, GetTickCount64(), 1, 0, false};
    playables.setSpeed(&sceneFF.root, kSceneFfSpeed);
    SetSceneFFSource(scene);
}

bool SameSceneFFOwner(const SceneOwner& scene) {
    return Same(scene.graph, sceneFF.graph) && Same(scene.root, sceneFF.root) && scene.director == sceneFF.director &&
           scene.continueMixer == sceneFF.continueMixer && Same(scene.continuePlayable, sceneFF.continuePlayable);
}

// Each Cross on the running owner adds one stale continue to drop and restarts the timeout; another owner is left alone.
void AdvanceScene(const SceneOwner& scene) {
    if (!scene.cinematic) return;
    if (!sceneFF.active) {
        StartSceneFF(scene);
    } else if (SameSceneFFOwner(scene)) {
        ++sceneFF.pendingContinues;
        sceneFF.startMs = GetTickCount64();
        SetSceneFFSource(scene);
    }
}

// ContinueConversationMixerBehaviour.ProcessFrame sends a continue for input i when its weight is above 0.001 and i is not in its played
// set, then adds i; the helpers below are the ones it calls, with the method arguments it passes.
using InputCount = int(APS5_VABI*)(std::uint64_t playable, std::uint32_t version, std::uint64_t method);
using InputWeight = float(APS5_VABI*)(std::uint64_t playable, std::uint32_t version, int input, std::uint64_t method);
using PlayedSet = bool(APS5_VABI*)(std::uint64_t set, int input, std::uint64_t method);
constexpr std::uint32_t kInputCount = 0x16882e0, kInputWeight = 0x16885a0, kPlayedContains = 0x2d74f60, kPlayedAdd = 0x2d770f0;
constexpr std::uint32_t kInputCountMethod = 0x5e73410, kInputWeightMethod = 0x5e73428, kPlayedContainsMethod = 0x5e511b8, kPlayedClass = 0x5e511a8;
constexpr std::uint32_t kContinueMixerInitialized = 0x6289697;  // set once ProcessFrame has filled the slots above
constexpr std::size_t kMixerPlayed = 0x10;

std::uint64_t Slot(std::uint32_t offset) { return *reinterpret_cast<const std::uint64_t*>(game + offset); }

int PendingContinue(std::uint64_t mixer, const Handle& playable) {
    const std::uint64_t played = Pointer(mixer, kMixerPlayed);
    if (*reinterpret_cast<const std::uint8_t*>(game + kContinueMixerInitialized) == 0 || played == 0) return -1;
    const int count = reinterpret_cast<InputCount>(game + kInputCount)(playable.pointer, playable.version, Slot(kInputCountMethod));
    for (int input = 0; input < count; ++input) {
        const float weight =
            reinterpret_cast<InputWeight>(game + kInputWeight)(playable.pointer, playable.version, input, Slot(kInputWeightMethod));
        if (weight > 0.001f && !reinterpret_cast<PlayedSet>(game + kPlayedContains)(played, input, Slot(kPlayedContainsMethod))) return input;
    }
    return -1;
}

bool MarkPlayed(std::uint64_t mixer, int input) {
    const std::uint64_t method = Pointer(Pointer(Pointer(Slot(kPlayedClass), 0x20), 0xc0), 0xa8);
    const std::uint64_t played = Pointer(mixer, kMixerPlayed);
    if (method == 0 || played == 0) return false;
    reinterpret_cast<PlayedSet>(game + kPlayedAdd)(played, input, method);
    return true;
}

void OnStartSubtitle(const Saved& r) {
    if (skipAvailable) ArmLine(r.rdi, r.rsi);
    if (Owner* owner = sceneAvailable ? OwnerOf(r.rdi) : nullptr) owner->conversation = Int(Pointer(r.rsi, kSubtitleEntry), kEntryConversation);
}

std::uint64_t finishedSubtitles = 0;

void OnFinishSubtitle(const Saved&) {
    ++finishedSubtitles;
    if (skipAvailable) DisarmLine();
}

void OnClose(const Saved& r) {
    if (sceneFF.active && r.rdi == sceneFF.sourceController) sceneFF.sourceConversationClosed = true;
    if (skipAvailable) DisarmLine();
    for (Owner& owner : owners) {
        if (owner.controller == r.rdi) owner.open = false;
    }
}

void OnInitialize(const Saved& r) {
    if (!sceneAvailable) return;
    const bool fromTimeline = InsideInvocation(r.ret, kConversationMixerProcessFrame, conversationMixerInvocation.frame);
    RememberOwner(r.rdx, r.rdi, fromTimeline ? &conversationMixerInvocation : nullptr);
    // Once the skipped line's conversation has closed, gameplay dialogue starting means the scene is over. Its remaining continues are no
    // longer stale (the last one may release the player), so the speed returns now and none is dropped.
    if (sceneFF.active && sceneFF.sourceConversationClosed && !fromTimeline && StartedByGameplay(r.ret)) EndSceneFF();
}

// ProcessFrame(this, Playable playable, FrameData info, object playerData): the Playable's handle and version arrive in rsi and edx. The
// method then pushes rbp just below its return address, so that address minus 8 identifies this invocation's frame.
Invocation EnteredMixer(const Saved& r) {
    return {reinterpret_cast<std::uint64_t>(&r.ret) - 8, r.rdi, Handle{r.rsi, static_cast<std::uint32_t>(r.rdx), 0}};
}

void OnConversationMixer(const Saved& r) {
    conversationMixerInvocation = EnteredMixer(r);
    CheckSceneFF();
}

void OnContinueMixer(const Saved& r) {
    const Invocation invocation = EnteredMixer(r);
    NoteContinueMixer(invocation.mixer, invocation.playable);
    CheckSceneFF();
    if (!sceneFF.active || invocation.mixer != sceneFF.continueMixer || !Same(invocation.playable, sceneFF.continuePlayable)) return;
    const int input = PendingContinue(invocation.mixer, invocation.playable);
    if (input < 0) return;
    if (!MarkPlayed(invocation.mixer, input)) {
        EndSceneFF();
        return;
    }
    if (--sceneFF.pendingContinues > 0) {
        sceneFF.startMs = GetTickCount64();
        return;
    }
    EndSceneFF();
}

// The voice command's Start resumes in state 1 once its voice has ended; the line then continues by itself.
void OnVoiceCommand(const Saved& r) {
    if (skipAvailable && skipSequencer != 0 && Int(r.rdi, kIteratorState) == 1 &&
        Pointer(Pointer(r.rdi, kIteratorThis), kCommandSequencer) == skipSequencer) {
        DisarmLine();
    }
}

// The continue runs from the view's own Update: on the game thread, and only on the live view being updated.
void OnViewUpdate(const Saved& r) {
    CheckSceneFF();
    if (!skipAvailable || r.rdi != skipView || !SkipDialogue::TakeRequest()) return;
    const SceneOwner scene = ResolveOwner(r.rdi);  // before the continue, which may close the conversation
    const std::uint64_t finished = finishedSubtitles;
    reinterpret_cast<HandleContinueButtonClick>(game + kHandleContinueButtonClick.range.begin)(r.rdi, nullptr);
    // HandleContinueButtonClick ignores a continue in the frame its line started; the scene only advances with the line.
    if (finishedSubtitles != finished) AdvanceScene(scene);
}

const Site kSites[] = {
    {"ConversationController.Close", 0x4e78c0, 0x4e7d40, 0xe050d415a367db99, 12, false, Use::Skip, OnClose},
    {"ConversationView.StartSubtitle", 0x54bae0, 0x54c260, 0xa70a4c4ede99a7b4, 12, false, Use::Skip, OnStartSubtitle},
    {"ConversationView.FinishSubtitle", 0x54d160, 0x54d340, 0xfc6f8dfa6195a132, 10, true, Use::Skip, OnFinishSubtitle},
    {"SequencerCommandFMODWait.<Start>d__16.MoveNext", 0xd37350, 0xd37a50, 0xd74eddee50f74239, 12, false, Use::Skip, OnVoiceCommand},
    {"ConversationView.Update", 0x54b9d0, 0x54ba20, 0xe7a4896dde6d9f9c, 13, false, Use::Skip, OnViewUpdate},
    {"ConversationController.Initialize", 0x4e7680, 0x4e78c0, 0x7830fba09508dbba, 12, false, Use::Scene, OnInitialize},
    {"ConversationMixerBehaviour.ProcessFrame", kConversationMixerProcessFrame.begin, kConversationMixerProcessFrame.end, 0x3dc96aa4acf40e6e, 12,
        false, Use::Scene, OnConversationMixer},
    {"ContinueConversationMixerBehaviour.ProcessFrame", kContinueMixerProcessFrame.begin, kContinueMixerProcessFrame.end, 0xb12fa57a4bdc9a13, 12,
        false, Use::Scene, OnContinueMixer},
};

constexpr Code kSkipCode[] = {kHandleContinueButtonClick};
constexpr Code kSceneCode[] = {
    {{kResolveIcall, 0x35ff30}, 0xcf69ecb8ca8a28fe},
    {{kInputCount, kInputCount + 32}, 0xd66869c67bf1ba90},
    {{kInputWeight, kInputWeight + 32}, 0x711e8c1c9f122c10},
    {{kPlayedContains, kPlayedContains + 32}, 0xb2f1469b9ffff2e9},
    {{kPlayedAdd, kPlayedAdd + 32}, 0xab67f6f185fe0b83},
};

void APS5_VABI OnHook(const Site* site, const Saved* saved) {
    hookFrame = *static_cast<const std::uint64_t*>(__builtin_frame_address(0));  // the trampoline keeps the game's rbp, saved by this frame
    site->on(*saved);
}

// Saves every argument register (the game calls with the System V ABI), calls OnHook(site, saved) and restores them; the moved
// instructions and a jump back follow. Entered at the method's first byte, so rsp + 8 is 16-byte aligned.
constexpr std::uint8_t kTrampolineStart[] = {
    0x41, 0x53, 0x41, 0x52, 0x50, 0x41, 0x51, 0x41, 0x50, 0x51, 0x52, 0x56, 0x57,  // push r11, r10, rax, r9, r8, rcx, rdx, rsi, rdi
    0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00,                                      // sub rsp, 0x80
    0xc5, 0xfa, 0x7f, 0x04, 0x24, 0xc5, 0xfa, 0x7f, 0x4c, 0x24, 0x10,              // vmovdqu [rsp + 16 * n], xmm0..xmm7
    0xc5, 0xfa, 0x7f, 0x54, 0x24, 0x20, 0xc5, 0xfa, 0x7f, 0x5c, 0x24, 0x30, 0xc5, 0xfa, 0x7f, 0x64, 0x24, 0x40,
    0xc5, 0xfa, 0x7f, 0x6c, 0x24, 0x50, 0xc5, 0xfa, 0x7f, 0x74, 0x24, 0x60, 0xc5, 0xfa, 0x7f, 0x7c, 0x24, 0x70,
    0x48, 0x8d, 0xb4, 0x24, 0x80, 0x00, 0x00, 0x00,                                // lea rsi, [rsp + 0x80]
    0x48, 0xbf, 0, 0, 0, 0, 0, 0, 0, 0,                                            // mov rdi, site (offset 77)
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,                                            // mov rax, OnHook (offset 87)
    0xff, 0xd0,                                                                    // call rax
    0xc5, 0xfa, 0x6f, 0x04, 0x24, 0xc5, 0xfa, 0x6f, 0x4c, 0x24, 0x10,              // vmovdqu xmm0..xmm7, [rsp + 16 * n]
    0xc5, 0xfa, 0x6f, 0x54, 0x24, 0x20, 0xc5, 0xfa, 0x6f, 0x5c, 0x24, 0x30, 0xc5, 0xfa, 0x6f, 0x64, 0x24, 0x40,
    0xc5, 0xfa, 0x6f, 0x6c, 0x24, 0x50, 0xc5, 0xfa, 0x6f, 0x74, 0x24, 0x60, 0xc5, 0xfa, 0x6f, 0x7c, 0x24, 0x70,
    0x48, 0x81, 0xc4, 0x80, 0x00, 0x00, 0x00,                                      // add rsp, 0x80
    0x5f, 0x5e, 0x5a, 0x59, 0x41, 0x58, 0x41, 0x59, 0x58, 0x41, 0x5a, 0x41, 0x5b,  // pop rdi, rsi, rdx, rcx, r8, r9, rax, r10, r11
};
static_assert(sizeof(kTrampolineStart) == 164);
constexpr std::size_t kTrampolineSite = 77, kTrampolineHandler = 87;
constexpr std::size_t kTrampolineSlot = 256;
constexpr std::size_t kEntryJump = 12;  // mov rax, trampoline; jmp rax

std::uint64_t Hash(std::uint32_t begin, std::uint32_t end) {
    std::uint64_t hash = 0xcbf29ce484222325;
    for (std::uint32_t offset = begin; offset < end; ++offset) hash = (hash ^ reinterpret_cast<const std::uint8_t*>(game)[offset]) * 0x100000001b3;
    return hash;
}

template <std::size_t count> bool Recognised(const Code (&code)[count]) {
    for (const Code& entry : code) {
        if (entry.range.end > gameSize || Hash(entry.range.begin, entry.range.end) != entry.hash) return false;
    }
    return true;
}

void BuildTrampoline(std::uint8_t* at, const Site& site) {
    auto* method = reinterpret_cast<std::uint8_t*>(game + site.begin);
    const void* const handler = reinterpret_cast<const void*>(&OnHook);
    const Site* const self = &site;
    std::memcpy(at, kTrampolineStart, sizeof(kTrampolineStart));
    std::memcpy(at + kTrampolineSite, &self, sizeof(self));
    std::memcpy(at + kTrampolineHandler, &handler, sizeof(handler));
    std::size_t used = sizeof(kTrampolineStart);
    std::memcpy(at + used, method, site.copied);
    used += site.copied;
    const std::uint8_t* resume = method + site.copied;
    if (site.ripCompare) {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, resume + 2, sizeof(displacement));
        const std::uint8_t* const operand = resume + 7 + displacement;
        const std::uint8_t compare[] = {0x50, 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x38, resume[6], 0x58};  // push rax; mov rax, operand;
        std::memcpy(at + used, compare, sizeof(compare));                                                    // cmp byte [rax], imm8; pop rax
        std::memcpy(at + used + 3, &operand, sizeof(operand));
        used += sizeof(compare);
        resume += 7;
    }
    const std::uint8_t jumpBack[] = {0xff, 0x25, 0, 0, 0, 0};  // jmp qword ptr [rip], followed by the address
    std::memcpy(at + used, jumpBack, sizeof(jumpBack));
    std::memcpy(at + used + sizeof(jumpBack), &resume, sizeof(resume));
}

bool Patch(const Site& site, const std::uint8_t* trampoline, std::uint8_t* original) {
    auto* method = reinterpret_cast<std::uint8_t*>(game + site.begin);
    std::uint8_t jump[kEntryJump] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    std::memcpy(jump + 2, &trampoline, sizeof(trampoline));
    DWORD protection = 0;
    if (!VirtualProtect(method, sizeof(jump), PAGE_EXECUTE_READWRITE, &protection)) return false;
    std::memcpy(original, method, sizeof(jump));
    std::memcpy(method, jump, sizeof(jump));
    VirtualProtect(method, sizeof(jump), protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), method, sizeof(jump));
    return true;
}

void Unpatch(const Site& site, const std::uint8_t* original) {
    auto* method = reinterpret_cast<std::uint8_t*>(game + site.begin);
    DWORD protection = 0;
    if (!VirtualProtect(method, kEntryJump, PAGE_EXECUTE_READWRITE, &protection)) return;
    std::memcpy(method, original, kEntryJump);
    VirtualProtect(method, kEntryJump, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), method, kEntryJump);
}

// Only the analysed game build is patched: Skip dialogue stays off unless every method it uses is recognised, and the cutscene
// fast-forward unless every method it uses is too.
bool StartSkipDialogue() {
    if (!SkipDialogue::Enabled()) return false;
    game = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"Il2cppUserAssemblies.prx.guest.prx"));
    if (game == 0) {
        std::fprintf(stderr, "Skip dialogue: the dialogue code was not recognised, Cross does not skip lines\n");
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
    gameSize = reinterpret_cast<const IMAGE_NT_HEADERS*>(game + static_cast<std::uintptr_t>(dos->e_lfanew))->OptionalHeader.SizeOfImage;

    constexpr std::size_t count = std::size(kSites);
    constexpr std::size_t blockSize = count * kTrampolineSlot;
    auto* block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, blockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    bool skipRecognised = block != nullptr && Recognised(kSkipCode);
    bool sceneRecognised = Recognised(kSceneCode);
    for (std::size_t index = 0; index < count && block != nullptr; ++index) {
        const Site& site = kSites[index];
        if (site.end <= gameSize && Hash(site.begin, site.end) == site.hash) {
            BuildTrampoline(block + index * kTrampolineSlot, site);
        } else if (site.use == Use::Skip) {
            skipRecognised = false;
        } else {
            sceneRecognised = false;
        }
    }
    DWORD protection = 0;
    const bool executable = block != nullptr && VirtualProtect(block, blockSize, PAGE_EXECUTE_READ, &protection);
    if (block != nullptr) FlushInstructionCache(GetCurrentProcess(), block, blockSize);
    bool skipInstalled = skipRecognised && executable;
    bool sceneInstalled = skipInstalled && sceneRecognised;
    std::uint8_t originals[count][kEntryJump] = {};
    bool applied[count] = {};
    for (std::size_t index = 0; index < count; ++index) {
        const Site& site = kSites[index];
        if (!(site.use == Use::Skip ? skipInstalled : sceneInstalled)) continue;
        if (Patch(site, block + index * kTrampolineSlot, originals[index])) {
            applied[index] = true;
            continue;
        }
        if (site.use == Use::Skip) skipInstalled = false;
        sceneInstalled = false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const Site& site = kSites[index];
        if (applied[index] && !(site.use == Use::Skip ? skipInstalled : sceneInstalled)) Unpatch(site, originals[index]);
    }
    skipAvailable = skipInstalled;
    sceneAvailable = skipInstalled && sceneInstalled;
    std::fprintf(stderr, skipAvailable ? "Skip dialogue: patch applied\n"
                                       : "Skip dialogue: the dialogue code was not recognised, Cross does not skip lines\n");
    if (skipAvailable && !sceneAvailable) std::fprintf(stderr, "Skip dialogue: the cutscene code was not recognised, cutscenes keep their pace\n");
    return true;
}

[[maybe_unused]] const bool skipDialogueStarted = StartSkipDialogue();

}
#endif
