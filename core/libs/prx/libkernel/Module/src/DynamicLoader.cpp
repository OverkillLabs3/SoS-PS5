#include "prx/libc/include/General.hpp"
#include "prx/common/StderrLog.hpp"
#include <nid/NidCompute.hpp>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
thread_local std::array<char, 512> loaderError{};
thread_local bool pendingError = false;
void Error(const char* message) {
    std::snprintf(loaderError.data(), loaderError.size(), "%s", message);
    pendingError = true;
}
struct Module {
    void* native = nullptr;
    bool owned = true;
    bool global = false;
    ~Module() {
        if (owned && native) {
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(native));
#else
            ::dlclose(native);
#endif
        }
    }
};
std::mutex modulesMutex;
std::map<std::uintptr_t, std::shared_ptr<Module>> modules;
std::uintptr_t nextHandle = 0x20000000;

bool TraceLoader() {
    static const bool value = std::getenv("APS5_TRACE_DL") != nullptr;
    return value;
}
void Log(const char* format, ...) {
    if (!TraceLoader()) return;
    va_list args;
    va_start(args, format);
    aps5::LogString(aps5::LogStdErr, "[dl] ");
    aps5::LogWrite(2, format, args);
    aps5::LogChar(aps5::LogStdErr, '\n');
    va_end(args);
}
void* Symbol(Module& module, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module.native), name));
#else
    return ::dlsym(module.native, name);
#endif
}
void* FindSymbol(Module& module, const char* name) {
    if (auto* symbol = Symbol(module, name)) return symbol;
    const auto nid = Nid::ComputeNid(name, "");
    return Symbol(module, nid.c_str());
}
#ifdef _WIN32

void RunDeferredLifecycle(void* native) {
    static std::mutex lifecycleMutex;
    static std::set<void*> started;
    {
        std::lock_guard lock(lifecycleMutex);
        if (!started.insert(native).second) return;
    }
    const auto* base = static_cast<std::uint8_t*>(native);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (auto* end = section + nt->FileHeader.NumberOfSections; section != end; ++section) {
        if (std::memcmp(section->Name, ".ginit", 6) != 0) continue;
        const auto init = *reinterpret_cast<const std::uint32_t*>(base + section->VirtualAddress);
        if (!init) return;
        Log("running deferred module init at %p", base + init);
        reinterpret_cast<void (*)(std::uint64_t, std::uint64_t, std::uint64_t)>(base + init)(0, 0, 0);
        return;
    }
}
#endif
}

extern "C" {
char* APS5_VABI dlerror_nid_postfix() {
    if (!pendingError) return nullptr;
    pendingError = false;
    return loaderError.data();
}
void* APS5_VABI dlopen_nid_postfix_impl(const char* path, int flags);
void* APS5_VABI dlopen_nid_postfix(const char* path, int flags) {
    void* result = dlopen_nid_postfix_impl(path, flags);
    Log("dlopen(%s, 0x%x) -> %p%s", path ? path : "(null)", flags, result, result ? "" : (pendingError ? (std::string(" error: ") + loaderError.data()).c_str() : ""));
    return result;
}
void* APS5_VABI dlopen_nid_postfix_impl(const char* path, int flags) {
    if ((flags & ~0x103) || (flags & 3) == 0 || (flags & 3) == 3) {
        Error("dlopen: unsupported flags"); return nullptr;
    }
    try {
        auto module = std::make_shared<Module>();
        module->global = (flags & 0x100) != 0 || !path;
#ifdef _WIN32
        if (!path) {
            module->native = GetModuleHandleW(nullptr);
            module->owned = false;
        } else {
            if (!*path) { Error("dlopen: empty module path"); return nullptr; }
            auto resolved = ResolvePath_nid_no_patch(path);

            {
                wchar_t executable[MAX_PATH] = {};
                GetModuleFileNameW(nullptr, executable, MAX_PATH);
                const std::wstring base = std::filesystem::path(resolved).filename().wstring();
                for (const wchar_t* directory : {L"app0\\sce_module", L"sce_module"}) {
                    const auto guest = std::filesystem::path(executable).parent_path() / directory / (base + L".guest.prx");
                    std::error_code ignored;
                    if (std::filesystem::exists(guest, ignored)) { resolved = guest; break; }
                }
            }
            module->native = LoadLibraryExW(resolved.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (!module->native) {
            char message[128];
            std::snprintf(message, sizeof(message), "dlopen: Windows loader error %lu (module must be host-compatible)", GetLastError());
            Error(message); return nullptr;
        }
#else
        auto resolved = path ? ResolvePath_nid_no_patch(path).string() : std::string{};
        // On by default: this is what the Windows branch does, and without it the title never gets
        // its managed runtime resident, so every later stage is missing. APS5_GUEST_PRX_REMAP=0
        // turns it off to compare against the pre-remap behaviour.
        static const bool mapGuestPrx = std::getenv("APS5_GUEST_PRX_REMAP") == nullptr ||
            std::strcmp(std::getenv("APS5_GUEST_PRX_REMAP"), "0") != 0;
        if (path && *path && mapGuestPrx) {
            // Same rule as the Windows branch: the converted file is "<name>.prx.guest.prx" next to
            // the executable, and titles ask for modules with their own spelling of the name, so the
            // lookup has to ignore case the way Windows' filesystem does.
            const auto lower = [](std::string text) {
                for (auto& c : text)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return text;
            };
            const auto exe = [] {
                std::error_code ignored;
                auto self = std::filesystem::read_symlink("/proc/self/exe", ignored);
                return ignored ? std::filesystem::path{} : self.parent_path();
            }();
            if (!exe.empty()) {
                const auto wanted = lower(std::filesystem::path(resolved).filename().string() + ".guest.prx");
                for (const char* directory : {"app0/sce_module", "sce_module"}) {
                    const auto parent = exe / directory;
                    const auto exact = parent / (std::filesystem::path(resolved).filename().string() + ".guest.prx");
                    std::error_code ignored;
                    if (std::filesystem::exists(exact, ignored)) {
                        resolved = exact.string();
                        break;
                    }
                    if (!std::filesystem::is_directory(parent, ignored)) continue;
                    bool found = false;
                    for (const auto& entry : std::filesystem::directory_iterator(parent, ignored)) {
                        if (entry.is_regular_file() && lower(entry.path().filename().string()) == wanted) {
                            resolved = entry.path().string();
                            found = true;
                            break;
                        }
                    }
                    if (found) break;
                }
            }
        }
        const int nativeFlags = ((flags & 3) == 1 ? RTLD_LAZY : RTLD_NOW) |
            ((flags & 0x100) ? RTLD_GLOBAL : RTLD_LOCAL);
        module->native = ::dlopen(path ? resolved.c_str() : nullptr, nativeFlags);
        if (!module->native) { Error(::dlerror()); return nullptr; }
#endif
        void* native = module->native;
        std::uintptr_t handle;
        {
            std::lock_guard lock(modulesMutex);
            handle = nextHandle++;
            modules.emplace(handle, std::move(module));
        }
#ifdef _WIN32

        RunDeferredLifecycle(native);
#endif
        return reinterpret_cast<void*>(handle);
    } catch (const std::exception& error) { Error(error.what()); return nullptr; }
}
void* APS5_VABI dlsym_nid_postfix_impl(void* handle, const char* name);
void* APS5_VABI dlsym_nid_postfix(void* handle, const char* name) {
    void* result = dlsym_nid_postfix_impl(handle, name);
    static int count = 0;
    if (TraceLoader() && (!result || count++ < 40)) Log("dlsym(%p, %s) -> %p", handle, name ? name : "(null)", result);
    return result;
}
void* APS5_VABI dlsym_nid_postfix_impl(void* handle, const char* name) {
    if (!name || !*name) { Error("dlsym: empty symbol name"); return nullptr; }
    try {
        std::vector<std::shared_ptr<Module>> search;
        {
            std::lock_guard lock(modulesMutex);
            if (handle == reinterpret_cast<void*>(static_cast<std::intptr_t>(-2)) || handle == nullptr) {
                for (const auto& [key, module] : modules) if (module->global) search.push_back(module);
            } else {
                auto found = modules.find(reinterpret_cast<std::uintptr_t>(handle));
                if (found == modules.end()) {
                    // Handle 0 is the main program on PS5, and modules the dynamic loader pulled in
                    // through DT_NEEDED never appear in this table, so an unknown handle is normal:
                    // fall through to the process-wide lookup below instead of failing here.
                    if (handle != nullptr) { Error("dlsym: unsupported module handle"); return nullptr; }
                } else {
                    search.push_back(found->second);
                }
            }
        }
        for (const auto& module : search) if (auto* result = FindSymbol(*module, name)) return result;
#ifndef _WIN32
        // Titles pass handle 0 (RTLD_DEFAULT) for a process-wide lookup. Modules the dynamic loader
        // pulled in through DT_NEEDED never appear in `modules`, so ask the real loader as well - by
        // the plain name and by its NID, which is how the guest libraries export their API. glibc
        // searches the executable first, which matches "handle 0 is the main program" on PS5.
        if (handle == nullptr || handle == reinterpret_cast<void*>(static_cast<std::intptr_t>(-2))) {
            if (auto* result = ::dlsym(RTLD_DEFAULT, name)) return result;
            const auto nid = Nid::ComputeNid(name, "");
            if (auto* result = ::dlsym(RTLD_DEFAULT, nid.c_str())) return result;
        }
#endif
        // A miss on a plain name is ambiguous: the guest may have asked for something that exists
        // only under its NID, or for a runtime symbol no module exports at all. Naming the computed
        // NID and the handle separates "wrong module in scope" from "not implemented here".
        const auto missingNid = Nid::ComputeNid(name, "");
        char missing[192];
        std::snprintf(missing, sizeof(missing),
            "dlsym: symbol not found in supported module scope (asked=\"%s\" nid=%s handle=%s)",
            name, missingNid.c_str(), handle ? (std::to_string(reinterpret_cast<std::uintptr_t>(handle))).c_str() : "default");
        Error(missing);
        return nullptr;
    } catch (const std::exception& error) { Error(error.what()); return nullptr; }
}
int APS5_VABI dlclose_nid_postfix(void* handle) {
    std::shared_ptr<Module> module;
    {
        std::lock_guard lock(modulesMutex);
        auto found = modules.find(reinterpret_cast<std::uintptr_t>(handle));
        if (found == modules.end()) { Error("dlclose: invalid module handle"); return -1; }
        module = std::move(found->second);
        modules.erase(found);
    }

    module.reset();
    return 0;
}
}
