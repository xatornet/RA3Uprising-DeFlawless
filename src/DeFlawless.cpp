#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

struct Settings {
    bool enabled = true;
    bool logging = true;
    bool debugPassthrough = false;
    float additionalFov = 0.0f;
    float defaultAspect = 1.777777791f;
    float fovMax = 110.0f;
};

Settings g_settings;
HMODULE g_game = nullptr;
std::vector<void*> g_stubs;
std::mutex g_logMutex;
volatile LONG g_hookCalls = 0;
PVOID g_exceptionHandler = nullptr;

void Log(const char* format, ...);

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* exception) {
    if (exception && exception->ExceptionRecord && exception->ContextRecord) {
        Log("CRASH: code=0x%08lX address=%p EIP=0x%08lX ESP=0x%08lX",
            static_cast<unsigned long>(exception->ExceptionRecord->ExceptionCode),
            exception->ExceptionRecord->ExceptionAddress,
            static_cast<unsigned long>(exception->ContextRecord->Eip),
            static_cast<unsigned long>(exception->ContextRecord->Esp));
    } else {
        Log("CRASH: exception data unavailable");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI VectoredCrashHandler(EXCEPTION_POINTERS* exception) {
    if (!exception || !exception->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    const DWORD code = exception->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_FLT_STACK_CHECK) {
        CrashHandler(exception);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

std::string ModuleDirectory() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
    std::string result(path);
    const auto slash = result.find_last_of("\\/");
    return result.substr(0, slash + 1);
}

void Log(const char* format, ...) {
    if (!g_settings.logging)
        return;

    char message[1024]{};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME now{};
    GetLocalTime(&now);
    char line[1200]{};
    std::snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] %s\n",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, message);
    OutputDebugStringA(line);

    std::lock_guard<std::mutex> lock(g_logMutex);
    std::ofstream file(ModuleDirectory() + "DeFlawless.log", std::ios::app);
    if (file)
        file << line;
}

void LogLastError(const char* operation) {
    Log("%s failed, GetLastError=%lu", operation, static_cast<unsigned long>(GetLastError()));
}

std::string IniPath() {
    return ModuleDirectory() + "DeFlawless.ini";
}

void ReadSettings() {
    const std::string path = IniPath();
    g_settings.logging = GetPrivateProfileIntA("General", "Logging", 1, path.c_str()) != 0;
    g_settings.enabled = GetPrivateProfileIntA("Display", "Enabled", 1, path.c_str()) != 0;
    g_settings.debugPassthrough = GetPrivateProfileIntA("Debug", "Passthrough", 0, path.c_str()) != 0;

    char value[64]{};
    const auto readFloat = [&](const char* section, const char* key, const char* fallback, float minimum, float maximum) {
        GetPrivateProfileStringA(section, key, fallback, value, sizeof(value), path.c_str());
        const float parsed = std::strtof(value, nullptr);
        if (!std::isfinite(parsed))
            return std::strtof(fallback, nullptr);
        return std::clamp(parsed, minimum, maximum);
    };

    g_settings.additionalFov = readFloat("Display", "AdditionalFOV", "0.0", -30.0f, 30.0f);
    g_settings.defaultAspect = readFloat("Display", "DefaultAspectRatio", "1.777777791", 0.5f, 4.0f);
    g_settings.fovMax = readFloat("Display", "FOVMax", "110.0", 30.0f, 110.0f);
    Log("INI: path='%s', logging=%d, enabled=%d, passthrough=%d, additionalFOV=%.3f, defaultAspect=%.6f, fovMax=%.3f",
        path.c_str(), g_settings.logging ? 1 : 0, g_settings.enabled ? 1 : 0, g_settings.debugPassthrough ? 1 : 0,
        g_settings.additionalFov, g_settings.defaultAspect, g_settings.fovMax);
}

float CurrentAspectRatio() {
    HWND window = FindWindowW(nullptr, L"Command & Conquer Red Alert 3: Uprising");
    if (!window)
        window = GetForegroundWindow();

    RECT client{};
    if (!window || !GetClientRect(window, &client) || client.bottom <= 0)
        return g_settings.defaultAspect;

    return static_cast<float>(client.right) / static_cast<float>(client.bottom);
}

// Called by the generated x86 stubs. The value is in radians.
void __cdecl CorrectFov(float* value) {
    if (!value || !g_settings.enabled)
        return;

    const LONG call = InterlockedIncrement(&g_hookCalls);
    if (call <= 10)
        Log("HOOK entered call=%ld valuePtr=%p value=%.6f", call, static_cast<void*>(value), *value);
    const float aspect = CurrentAspectRatio();
    if (aspect <= 0.0f || g_settings.defaultAspect <= 0.0f)
        return;

    const float degrees = *value * 57.29577951308232f;
    const float corrected = 2.0f * std::atan(
        std::tan(degrees * 0.008726646259971648f) * aspect / g_settings.defaultAspect
    ) * 57.29577951308232f + g_settings.additionalFov;

    const float limited = std::clamp(corrected, 1.0f, g_settings.fovMax);
    *value = limited * 0.017453292519943295f;

    if (call <= 10 || call % 5000 == 0)
        Log("HOOK call=%ld inputRad=%.6f outputRad=%.6f aspect=%.6f window=%p",
            call, degrees * 0.017453292519943295f, *value, aspect,
            static_cast<void*>(FindWindowW(nullptr, L"Command & Conquer Red Alert 3: Uprising")));
}

bool Matches(const uint8_t* address, const std::vector<int>& pattern) {
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] >= 0 && address[i] != static_cast<uint8_t>(pattern[i]))
            return false;
    }
    return true;
}

std::vector<uint8_t*> FindMatches() {
    auto* base = reinterpret_cast<uint8_t*>(g_game);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    const std::vector<int> pattern = {
        0x74, -1, 0xF3, 0x0F, 0x10, 0x05, -1, -1, -1, -1,
        0xEB, -1, 0xF3, 0x0F, 0x10, 0x05, -1, -1, -1, -1,
        0xD9, 0x05
    };

    std::vector<uint8_t*> result;
    Log("SCAN: imageBase=%p, sections=%u", static_cast<void*>(base), nt->FileHeader.NumberOfSections);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::strncmp(reinterpret_cast<const char*>(section[i].Name), ".text", 5) != 0)
            continue;
        auto* start = base + section[i].VirtualAddress;
        const size_t size = section[i].Misc.VirtualSize;
        for (size_t offset = 0; offset + pattern.size() <= size; ++offset) {
            if (Matches(start + offset, pattern))
                result.push_back(start + offset);
        }
    }
    Log("SCAN: matches=%zu", result.size());
    for (auto* match : result)
        Log("SCAN: match=%p, patch=%p", static_cast<void*>(match), static_cast<void*>(match + 20));
    return result;
}

void* MakeStub(uint8_t* patchAddress, const uint8_t original[6]) {
    if (g_settings.debugPassthrough) {
        std::vector<uint8_t> code = {
            original[0], original[1], original[2], original[3], original[4], original[5],
            0xE9, 0, 0, 0, 0
        };
        auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!stub) {
            LogLastError("VirtualAlloc(passthrough stub)");
            return nullptr;
        }
        std::memcpy(stub, code.data(), code.size());
        const auto jumpOffset = code.size() - 4;
        const auto jumpTarget = reinterpret_cast<uintptr_t>(patchAddress + 6);
        *reinterpret_cast<int32_t*>(stub + jumpOffset) = static_cast<int32_t>(jumpTarget - (reinterpret_cast<uintptr_t>(stub + jumpOffset + 4)));
        FlushInstructionCache(g_game, stub, code.size());
        Log("STUB: passthrough stub=%p patch=%p", static_cast<void*>(stub), static_cast<void*>(patchAddress));
        return stub;
    }

    // Execute the original fld before changing XMM0, matching the Lua
    // plugin's originalcode -> FOV calculation order. The x87 state is then
    // saved with that original value already on the stack.
    std::vector<uint8_t> code = {
        0x9C, 0x60, 0x89, 0xE5, 0x83, 0xE4, 0xF0,
        original[0], original[1], original[2], original[3], original[4], original[5],
        0x81, 0xEC, 0x20, 0x02, 0x00, 0x00,
        0x0F, 0xAE, 0x04, 0x24,
        0xDB, 0xE3,
        0x83, 0xEC, 0x08,
        0xF3, 0x0F, 0x11, 0x84, 0x24, 0x08, 0x02, 0x00, 0x00,
        0x8D, 0x84, 0x24, 0x08, 0x02, 0x00, 0x00, 0x50,
        0xE8, 0, 0, 0, 0,
        0x83, 0xC4, 0x04,
        0x0F, 0xAE, 0x4C, 0x24, 0x08,
        0xF3, 0x0F, 0x10, 0x84, 0x24, 0x08, 0x02, 0x00, 0x00,
        0x89, 0xEC, 0x61, 0x9D,
        0xE9, 0, 0, 0, 0
    };

    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) {
        LogLastError("VirtualAlloc(stub)");
        return nullptr;
    }

    std::memcpy(stub, code.data(), code.size());
    // callOffset points to the four-byte displacement after opcode E8.
    const auto callOffset = static_cast<size_t>(46);
    const auto jumpOffset = code.size() - 4;
    const auto callTarget = reinterpret_cast<uintptr_t>(&CorrectFov);
    const auto jumpTarget = reinterpret_cast<uintptr_t>(patchAddress + 6);
    *reinterpret_cast<int32_t*>(stub + callOffset) = static_cast<int32_t>(callTarget - (reinterpret_cast<uintptr_t>(stub + callOffset + 4)));
    *reinterpret_cast<int32_t*>(stub + jumpOffset) = static_cast<int32_t>(jumpTarget - (reinterpret_cast<uintptr_t>(stub + jumpOffset + 4)));
    FlushInstructionCache(g_game, stub, code.size());
    return stub;
}

bool Install() {
    if (!g_settings.enabled) {
        Log("INSTALL: disabled by INI");
        return true;
    }

    const auto matches = FindMatches();
    if (matches.empty()) {
        Log("INSTALL: no signature matches");
        return false;
    }

    for (auto* match : matches) {
        auto* patch = match + 20;
        if (patch[0] != 0xD9 || patch[1] != 0x05) {
            Log("INSTALL: unexpected opcode at patch=%p: %02X %02X",
                static_cast<void*>(patch), patch[0], patch[1]);
            continue;
        }

        uint8_t original[6]{};
        std::memcpy(original, patch, sizeof(original));
        auto* stub = static_cast<uint8_t*>(MakeStub(patch, original));
        if (!stub) {
            Log("INSTALL: could not create stub for patch=%p", static_cast<void*>(patch));
            return false;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(patch, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LogLastError("VirtualProtect(patch)");
            return false;
        }
        patch[0] = 0xE9;
        *reinterpret_cast<int32_t*>(patch + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) - reinterpret_cast<uintptr_t>(patch + 5));
        patch[5] = 0x90;
        VirtualProtect(patch, 6, oldProtect, &oldProtect);
        FlushInstructionCache(g_game, patch, 6);
        g_stubs.push_back(stub);
        Log("INSTALL: patched=%p, stub=%p, return=%p",
            static_cast<void*>(patch), static_cast<void*>(stub), static_cast<void*>(patch + 6));
    }
    Log("INSTALL: installed=%zu/%zu", g_stubs.size(), matches.size());
    return !g_stubs.empty();
}

DWORD WINAPI Startup(void*) {
    Sleep(5000);
    ReadSettings();
    Log("STARTUP: DeFlawless loaded, game=%p", static_cast<void*>(g_game));
    char gamePath[MAX_PATH]{};
    GetModuleFileNameA(g_game, gamePath, MAX_PATH);
    Log("STARTUP: gamePath='%s'", gamePath);
    Log("STARTUP: delay complete");
    const bool installed = Install();
    Log("STARTUP: result=%s", installed ? "success" : "failure");
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        g_exceptionHandler = AddVectoredExceptionHandler(1, VectoredCrashHandler);
        SetUnhandledExceptionFilter(CrashHandler);
        g_game = GetModuleHandleW(nullptr);
        HANDLE thread = CreateThread(nullptr, 0, Startup, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
