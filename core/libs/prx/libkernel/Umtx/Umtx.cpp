#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/common/StderrLog.hpp"
#include "SceTypes.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

extern "C" int* APS5_VABI __error_nid_postfix();
extern "C" Pthread APS5_VABI scePthreadSelf();
void RunExceptionHandlerInline(int signum);

namespace {

constexpr int OpWait = 2, OpWake = 3, OpWaitUint = 11, OpWaitUintPrivate = 15, OpWakePrivate = 16, OpNWakePrivate = 21;
constexpr int GuestTimedOut = 60, GuestInvalid = 22, GuestNoSys = 78;
struct GuestTimespec { std::int64_t sec; std::int64_t nsec; };

struct Waiter { void* address; HANDLE event; Waiter* next; };

constexpr std::size_t BucketCount = 64;
struct alignas(64) Bucket {
    SRWLOCK lock = SRWLOCK_INIT;
    Waiter* head = nullptr;
};
Bucket buckets[BucketCount];

Bucket& BucketOf(const void* address) {
    const auto hashed = (reinterpret_cast<std::uintptr_t>(address) >> 2) * 0x9e3779b97f4a7c15ull;
    return buckets[hashed >> 58];
}
void Lock(Bucket& bucket) { AcquireSRWLockExclusive(&bucket.lock); }
void Unlock(Bucket& bucket) { ReleaseSRWLockExclusive(&bucket.lock); }
void Unlink(Bucket& bucket, Waiter* waiter) {
    for (Waiter** link = &bucket.head; *link; link = &(*link)->next)
        if (*link == waiter) { *link = waiter->next; return; }
}
}

bool Trace() { static const bool on = std::getenv("APS5_UMTX_TRACE") != nullptr; return on; }

extern "C" {

int APS5_VABI _umtx_op_nid_postfix(void* object, int op, unsigned long value, void* uaddr, void* uaddr2) {
    const auto fail = [](int error) { *__error_nid_postfix() = error; return -1; };
    switch (op) {
    case OpWait:
    case OpWaitUint:
    case OpWaitUintPrivate: {
        if (!object) return fail(GuestInvalid);
        Pthread self = scePthreadSelf();
        if (!self->wakeEvent) self->wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        DWORD timeout = INFINITE;
        if (uaddr2) {
            const auto* span = static_cast<const GuestTimespec*>(uaddr2);
            const auto total = span->sec * 1000 + span->nsec / 1000000 + (span->nsec % 1000000 ? 1 : 0);
            timeout = total < 0 ? 0 : total > 0x7ffffff0 ? 0x7ffffff0 : static_cast<DWORD>(total);
        }
        const bool wide = op == OpWait;
        const std::uint64_t expected = wide ? value : static_cast<std::uint32_t>(value);
        if (Trace()) aps5::LogErr( "[umtx] wait tid=%lu addr=%p val=%llx cur=%llx timeout=%lu\n", GetCurrentThreadId(), object, static_cast<unsigned long long>(expected), static_cast<unsigned long long>(wide ? *static_cast<volatile std::uint64_t*>(object) : *static_cast<volatile std::uint32_t*>(object)), static_cast<unsigned long>(timeout));
        Waiter waiter{object, static_cast<HANDLE>(self->wakeEvent), nullptr};
        ResetEvent(waiter.event);
        self->inWait.store(true);
        Bucket& bucket = BucketOf(object);
        Lock(bucket);
        const std::uint64_t current = wide ? *static_cast<volatile std::uint64_t*>(object) : *static_cast<volatile std::uint32_t*>(object);
        bool queued = false;
        if (current == expected && self->pendingException.load() == 0) {
            waiter.next = bucket.head; bucket.head = &waiter; queued = true;
        }
        Unlock(bucket);
        bool timedOut = false;
        if (queued) {
            char target[40];
            std::snprintf(target, sizeof(target), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(object)));
            KernelParkEnter_nid_postfix("umtx", target, __builtin_return_address(0));
            timedOut = WaitForSingleObject(waiter.event, timeout) == WAIT_TIMEOUT;
            KernelParkLeave_nid_postfix();
            Lock(bucket); Unlink(bucket, &waiter); Unlock(bucket);
        }
        self->inWait.store(false);
        if (const int pending = self->pendingException.exchange(0)) RunExceptionHandlerInline(pending);
        if (timedOut) return fail(GuestTimedOut);
        return 0;
    }
    case OpWake:
    case OpWakePrivate: {
        if (!object) return fail(GuestInvalid);
        if (Trace()) aps5::LogErr( "[umtx] wake tid=%lu addr=%p n=%lu\n", GetCurrentThreadId(), object, value);
        unsigned long remaining = value;
        Bucket& bucket = BucketOf(object);
        Lock(bucket);
        for (Waiter** link = &bucket.head; *link && remaining;) {
            Waiter* waiter = *link;
            if (waiter->address == object) { *link = waiter->next; SetEvent(waiter->event); --remaining; }
            else link = &waiter->next;
        }
        Unlock(bucket);
        return 0;
    }
    case OpNWakePrivate: {

        auto* addresses = static_cast<void* const*>(object);
        for (unsigned long i = 0; i < value; ++i) {
            Bucket& bucket = BucketOf(addresses[i]);
            Lock(bucket);
            for (Waiter** link = &bucket.head; *link;) {
                Waiter* waiter = *link;
                if (waiter->address == addresses[i]) { *link = waiter->next; SetEvent(waiter->event); break; }
                link = &waiter->next;
            }
            Unlock(bucket);
        }
        return 0;
    }
    default: {
        static std::atomic<int> reported{0};
        if (reported.fetch_add(1) < 20) aps5::LogErr( "[umtx] unsupported op %d obj=%p val=%lu\n", op, object, value);
        return fail(GuestNoSys);
    }
    }
}

}
