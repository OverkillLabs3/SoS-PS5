#include <cstdint>
#include "prx/common/StderrLog.hpp"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <ucontext.h>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <pthread.h>
#include <signal.h>
#include <string>
#include "SceTypes.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include "prx/libc/include/General.hpp"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

extern "C" Pthread APS5_VABI scePthreadSelf();

namespace {
using GuestHandler = void (APS5_VABI *)(int, void*);
constexpr int MaxSignals = 128;
std::atomic<GuestHandler> handlers[MaxSignals];

#ifdef _WIN32
constexpr std::size_t XStateBytes = 1024;
struct alignas(64) ExceptionFrame {
    alignas(64) char xstate[XStateBytes];
    CONTEXT context;
    int signum;
    void* mcontext;
    char mcontextStorage[0x400];
};
struct NtAlert { using Fn = LONG (NTAPI *)(HANDLE); };
bool IsGuestAddress(DWORD64 address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module) || !module) return false;
    if (module == GetModuleHandleW(nullptr)) return true;
    wchar_t name[MAX_PATH] = {};
    GetModuleFileNameW(module, name, MAX_PATH);
    const std::wstring path(name);
    return path.find(L".guest.prx") != std::wstring::npos;
}
void NudgeThread(Pthread thread) {
    static const auto alertById = reinterpret_cast<LONG (NTAPI *)(ULONG_PTR)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAlertThreadByThreadId"));
    if (alertById) alertById(GetThreadId(static_cast<HANDLE>(thread->nativeHandle)));
}

unsigned XMaskLow() {
    unsigned a = 1, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if ((c & (1u << 27)) == 0) return 0;
    unsigned low = 0, high = 0;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return low & 7u;
}
#endif
}

#ifdef _WIN32
extern "C" unsigned DeliverXMaskLow;
extern "C" unsigned DeliverXMaskHigh;
unsigned DeliverXMaskLow = XMaskLow();
unsigned DeliverXMaskHigh = 0;
namespace {
void DeliverExceptionImpl(ExceptionFrame* frame) {
    auto handler = handlers[frame->signum].load();
    if (handler) handler(frame->signum, frame->mcontext);
    if (DeliverXMaskLow != 0) {
        const unsigned low = DeliverXMaskLow, high = DeliverXMaskHigh;
        __asm__ volatile("xrstor64 (%0)" : : "r"(frame->xstate), "a"(low), "d"(high) : "memory");
    }
    RtlRestoreContext(&frame->context, nullptr);
}

}

extern "C" void DeliverExceptionThunk();
extern "C" void* DeliverExceptionTarget;
void* DeliverExceptionTarget = reinterpret_cast<void*>(&DeliverExceptionImpl);
__asm__(".text\n"
        ".globl DeliverExceptionThunk\n"
        "DeliverExceptionThunk:\n"
        "movl DeliverXMaskLow(%rip), %eax\n"
        "testl %eax, %eax\n"
        "jz 1f\n"
        "movl DeliverXMaskHigh(%rip), %edx\n"
        "xsave64 (%rbx)\n"
        "1:\n"
         "movq %rbx, %rcx\n"
         "jmp *DeliverExceptionTarget(%rip)\n");
#endif


static std::atomic<unsigned> gcedHanded, gcedInjected, gcedSkipped, gcedRun;

static unsigned gcWaitMillis() {
    static const unsigned value = [] { const char* text = std::getenv("APS5_GC_WAIT_MS"); return text ? static_cast<unsigned>(std::strtoul(text, nullptr, 10)) : 1000u; }();
    return value;
}

// PS5/FreeBSD `mcontext_t` as the titles' signal handlers see it. Offsets verified against the
// reference implementation's own static asserts (mc_rip at 0xa0, mc_rsp at 0xf8) - and 0xf8 is the
// slot the earlier zero-filled buffer wrote a self-pointer into, which is why the title's collector
// computed a stack range inside our scratch buffer and walked stack words as objects.
struct GuestSignalMcontext {
    std::uint64_t mc_onstack, mc_rdi, mc_rsi, mc_rdx, mc_rcx, mc_r8, mc_r9, mc_rax, mc_rbx, mc_rbp,
        mc_r10, mc_r11, mc_r12, mc_r13, mc_r14, mc_r15;
    int mc_trapno;
    std::uint16_t mc_fs, mc_gs;
    std::uint64_t mc_addr;
    int mc_flags;
    std::uint16_t mc_es, mc_ds;
    std::uint64_t mc_err, mc_rip, mc_cs, mc_rflags;
    std::uint64_t mc_reserved[8];
    std::uint64_t mc_rsp, mc_ss, mc_len, mc_fpformat, mc_ownedfp, mc_lbrfrom, mc_lbrto, mc_aux1,
        mc_aux2;
    std::uint64_t mc_fpstate[104];
    std::uint64_t mc_fsbase, mc_gsbase, mc_spare[6];
};
static_assert(offsetof(GuestSignalMcontext, mc_rip) == 0xa0);
static_assert(offsetof(GuestSignalMcontext, mc_rsp) == 0xf8);

void FillFromHost(GuestSignalMcontext& out, const ucontext_t& host) {
    const auto* g = host.uc_mcontext.gregs;
    out.mc_rdi = g[REG_RDI]; out.mc_rsi = g[REG_RSI]; out.mc_rdx = g[REG_RDX]; out.mc_rcx = g[REG_RCX];
    out.mc_r8 = g[REG_R8]; out.mc_r9 = g[REG_R9]; out.mc_r10 = g[REG_R10]; out.mc_r11 = g[REG_R11];
    out.mc_r12 = g[REG_R12]; out.mc_r13 = g[REG_R13]; out.mc_r14 = g[REG_R14]; out.mc_r15 = g[REG_R15];
    out.mc_rax = g[REG_RAX]; out.mc_rbx = g[REG_RBX]; out.mc_rbp = g[REG_RBP];
    out.mc_rip = g[REG_RIP]; out.mc_rsp = g[REG_RSP]; out.mc_rflags = g[REG_EFL];
    out.mc_len = sizeof(out);
}

// Registers as they are *right now*, for the self-targeted case: the volatile half is meaningless
// across the raise, so it is left zero like the reference does, while the frame the collector must
// scan (rsp/rbp/rbx/r12..r15 and the return address) is real.
void CaptureSelf(GuestSignalMcontext& out, std::uint64_t rip) {
    out = {};
    asm volatile("movq %%rsp, %0\n\tmovq %%rbp, %1\n\tmovq %%rbx, %2\n\t"
                 "movq %%r12, %3\n\tmovq %%r13, %4\n\tmovq %%r14, %5\n\tmovq %%r15, %6\n\t"
                 : "=r"(out.mc_rsp), "=r"(out.mc_rbp), "=r"(out.mc_rbx), "=r"(out.mc_r12),
                   "=r"(out.mc_r13), "=r"(out.mc_r14), "=r"(out.mc_r15)
                 : : "memory");
    out.mc_rip = rip;
    out.mc_len = sizeof(out);
}

void RunExceptionHandlerInline(int signum, const void* hostContext) {
    const auto handler = handlers[signum].load();
    if (!handler) return;
    aps5::LogErr("[gced] ran signum=%d run=%u\n", signum, gcedRun.fetch_add(1) + 1);
    GuestSignalMcontext ctx;
    if (hostContext != nullptr) {
        FillFromHost(ctx, *static_cast<const ucontext_t*>(hostContext));
    } else {
        CaptureSelf(ctx, reinterpret_cast<std::uint64_t>(__builtin_return_address(0)));
    }
    handler(signum, &ctx);
}

// the shared signal number lives with the thread plumbing (Pthread.hpp)

// Titles may hold a thread handle that is not the record AnyPS5's TLS resolves to for the same
// host thread (an adopted thread gets its own), so completion has to be keyed by host id: waiting
// on the handle's counter would time out even though the handler ran.
std::mutex servedMutex;
std::map<std::uintptr_t, unsigned> servedByHost;

unsigned ServedOn(std::uintptr_t host) {
    std::lock_guard lock(servedMutex);
    return servedByHost.count(host) ? servedByHost[host] : 0u;
}

void MarkServed(std::uintptr_t host) {
    std::lock_guard lock(servedMutex);
    ++servedByHost[host];
}

void GuestSignalHandler(int, siginfo_t*, void* hostContext) {
    const auto host = static_cast<std::uintptr_t>(pthread_self());
    if (const Pthread self = scePthreadSelf(); self != nullptr) {
        if (const int pending = self->pendingException.exchange(0)) RunExceptionHandlerInline(pending, hostContext);
        self->exceptionServed.fetch_add(1, std::memory_order_release);
    }
    MarkServed(host);
}

void EnsureGuestSignal() {
    static const bool installed = [] {
        struct sigaction action{};
        action.sa_sigaction = &GuestSignalHandler;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        if (sigaction(GuestExceptionSignal(), &action, nullptr) != 0) return false;
        return true;
    }();
    (void)installed;
}

extern "C" {

int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler) {
    if (signum < 0 || signum >= MaxSignals) return static_cast<int>(0x80020016);
    aps5::LogErr( "[EXC] install handler signum=%d handler=%p\n", signum, handler);
    handlers[signum].store(reinterpret_cast<GuestHandler>(handler));
    return 0;
}

int APS5_VABI sceKernelRemoveExceptionHandler(int signum) {
    if (signum < 0 || signum >= MaxSignals) return static_cast<int>(0x80020016);
    handlers[signum].store(nullptr);
    return 0;
}

int APS5_VABI sceKernelRaiseException(Pthread thread, int signum) {
    if (signum < 0 || signum >= MaxSignals) return static_cast<int>(0x80020016);
    const auto handler = handlers[signum].load();
    if (!thread || thread->threadId == std::this_thread::get_id()) {
        if (handler) { alignas(16) char mcontext[0x400] = {}; *reinterpret_cast<void**>(mcontext + 0xf8) = mcontext; handler(signum, mcontext); }
        return 0;
    }
    if (!handler) return 0;
#ifdef _WIN32
    HANDLE native = static_cast<HANDLE>(thread->nativeHandle);

    CONTEXT context{};
    bool interrupted = false;
    bool handed = false;

    const auto giveUpAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(gcWaitMillis());
    for (int attempt = 0;; ++attempt) {
        if (attempt >= 4096 && std::chrono::steady_clock::now() >= giveUpAt) break;
        if (SuspendThread(native) == static_cast<DWORD>(-1)) return static_cast<int>(0x80020003);
        context = {};
        context.ContextFlags = CONTEXT_ALL;
        const bool readable = GetThreadContext(native, &context) != 0;

        thread->pendingException.store(signum);
        if (thread->inWait.load()) {
            handed = true;
            break;
        }
        thread->pendingException.store(0);
        if (readable && IsGuestAddress(context.Rip)) {
            interrupted = true;
            break;
        }
        ResumeThread(native);
        if (!readable) return static_cast<int>(0x80020003);
        if (attempt < 64) SwitchToThread(); else if (attempt < 4096) Sleep(0); else Sleep(1);
    }

    if (handed) {
        ResumeThread(native);
        if (thread->wakeEvent) SetEvent(static_cast<HANDLE>(thread->wakeEvent));
        aps5::LogErr("[gced] hand tid=%u h=%u inj=%u skip=%u\n", GetThreadId(native),
                     gcedHanded.fetch_add(1) + 1, gcedInjected.load(), gcedSkipped.load());
        return 0;
    }
    if (!interrupted) {

        {
            char where[400] = {};
            int used = 0;
            DWORD64 candidates[8] = {context.Rip};
            int found = 1;
            const auto* stack = reinterpret_cast<const DWORD64*>(context.Rsp);
            for (int slot = 0; slot < 128 && found < 8; ++slot) {
                DWORD64 value = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), stack + slot, &value, sizeof(value), nullptr)) break;
                HMODULE owner = nullptr;
                if (value > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(value), &owner) && owner != nullptr) candidates[found++] = value;
            }
            for (int index = 0; index < found && used < static_cast<int>(sizeof(where)) - 80; ++index) {
                HMODULE owner = nullptr;
                wchar_t name[MAX_PATH] = {};
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(candidates[index]), &owner) && owner != nullptr) GetModuleFileNameW(owner, name, MAX_PATH);
                const wchar_t* base = wcsrchr(name, L'\\');
                used += std::snprintf(where + used, sizeof(where) - used, " %ls+0x%llx", base ? base + 1 : name, static_cast<unsigned long long>(candidates[index] - reinterpret_cast<DWORD64>(owner)));
            }
            aps5::LogErr("[gced] skipped tid=%u at%s\n", GetThreadId(native), where);
        }
        aps5::LogErr("[gced] skip tid=%u h=%u inj=%u skip=%u\n", GetThreadId(native),
                     gcedHanded.load(), gcedInjected.load(), gcedSkipped.fetch_add(1) + 1);
        return static_cast<int>(0x80020003);
    }

    auto top = (context.Rsp - 0x200 - sizeof(ExceptionFrame)) & ~static_cast<DWORD64>(0x3f);
    auto* frame = reinterpret_cast<ExceptionFrame*>(top);
    std::memset(&frame->context, 0, sizeof(*frame) - offsetof(ExceptionFrame, context));
    std::memset(frame->xstate, 0, 576);
    frame->context = context;
    frame->signum = signum;
    frame->mcontext = frame->mcontextStorage;

    *reinterpret_cast<void**>(frame->mcontextStorage + 0xf8) = frame;
    CONTEXT redirected = context;
    redirected.Rsp = top - 0x28;
    redirected.Rip = reinterpret_cast<DWORD64>(&DeliverExceptionThunk);
    redirected.Rbx = reinterpret_cast<DWORD64>(frame);
    const bool ok = SetThreadContext(native, &redirected) != 0;
    ResumeThread(native);

    if (ok) NudgeThread(thread);
    const unsigned injections = gcedInjected.fetch_add(1) + 1;
    if ((injections & 63) == 1)
        aps5::LogErr("[gced] inj n=%u h=%u skip=%u\n", injections, gcedHanded.load(), gcedSkipped.load());
    return ok ? 0 : static_cast<int>(0x80020003);
#else
    // Windows rewrites the target thread's context; Linux has no equivalent primitive, so deliver
    // a host signal instead: the handler runs on the target thread and invokes the guest handler
    // there, which is what the title's stop-the-world (il2cpp GC) waits for. Threads that were
    // already parked in an interruptible wait are served by the same pendingException flag.
    extern void UmtxWakeWaiters();
    const unsigned servedBefore = thread->exceptionServed.load(std::memory_order_acquire);
    thread->pendingException.store(signum);
    // The interrupted context is available on Linux (a real signal handler receives it), so delivery
    // happens on the target thread and the raiser waits - stop-the-world is only meaningful if the
    // thread has actually reached its handler by the time this returns.
    static const bool useSignal = std::getenv("APS5_GUEST_EXCEPTION_SIGNAL") == nullptr ||
        std::getenv("APS5_GUEST_EXCEPTION_SIGNAL")[0] != '0';
    if (useSignal) EnsureGuestSignal();
    // A title's thread handle and AnyPS5's own record for the same host thread can be different
    // objects, so identity has to be the host id: otherwise a "raise against yourself" is queued on
    // a record nobody ever services, and the collector continues believing the thread stopped.
    const auto here = static_cast<pthread_t>(pthread_self());
    if (thread == scePthreadSelf() ||
        (thread->hostThread != nullptr &&
         static_cast<pthread_t>(reinterpret_cast<std::uintptr_t>(thread->hostThread)) == here)) {
        RunExceptionHandlerInline(signum, nullptr);
        return 0;
    }
    if (useSignal && thread->hostThread != nullptr && !thread->_finished.load(std::memory_order_acquire)) {
        const auto id = static_cast<pthread_t>(reinterpret_cast<std::uintptr_t>(thread->hostThread));
        if (pthread_kill(id, GuestExceptionSignal()) == 0) {
            UmtxWakeWaiters();
            const auto hostKey = reinterpret_cast<std::uintptr_t>(thread->hostThread);
            const unsigned before = servedBefore + ServedOn(hostKey);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(gcWaitMillis());
            while (ServedOn(hostKey) + servedBefore == before &&
                   std::chrono::steady_clock::now() < deadline &&
                   !thread->_finished.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            aps5::LogErr("[gced] signal linux signum=%d h=%u served=%u\n", signum,
                         gcedInjected.fetch_add(1) + 1,
                         ServedOn(hostKey) != before);
            return 0;
        }
    }
    if (thread->inWait.load()) {
        UmtxWakeWaiters();
        aps5::LogErr("[gced] hand linux signum=%d h=%u\n", signum, gcedHanded.fetch_add(1) + 1);
        return 0;
    }
    thread->pendingException.store(0);
    aps5::LogErr("[gced] skip linux signum=%d skip=%u\n", signum, gcedSkipped.fetch_add(1) + 1);
    return static_cast<int>(0x80020003);
#endif
}

void APS5_VABI sceKernelDebugRaiseException(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseException c1=%d c2=%d", c1, c2);
}

void APS5_VABI sceKernelDebugRaiseExceptionOnReleaseMode(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseExceptionOnReleaseMode c1=%d c2=%d", c1, c2);
}

}
