#include "bridge.hpp"
#include <cstdarg>

HMODULE bridge_module = nullptr;

std::wstring module_directory()
{
    wchar_t path[32768]{};
    GetModuleFileNameW(bridge_module, path, static_cast<DWORD>(std::size(path)));
    std::wstring result(path);
    return result.substr(0, result.find_last_of(L"\\/"));
}

int setting(const wchar_t* name, int fallback)
{
    const auto path = module_directory() + L"\\fanatec-dfgt.ini";
    return GetPrivateProfileIntW(L"bridge", name, fallback, path.c_str());
}

void log_message(const char* format, ...)
{
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    const auto config = module_directory() + L"\\fanatec-dfgt.ini";
    const auto fallback = module_directory() + L"\\fanatec-dfgt.log";
    wchar_t configured[32768]{};
    GetPrivateProfileStringW(L"bridge", L"LogFile", fallback.c_str(), configured,
        static_cast<DWORD>(std::size(configured)), config.c_str());
    const std::wstring path(configured);
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"ab") || !file)
        return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    fprintf(file, "%02u:%02u:%02u.%03u [%lu] ", time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds, GetCurrentThreadId());
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        bridge_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

void start_telemetry_once()
{
    static const bool attempted = [] {
        if (!setting(L"TelemetryEnabled", 0)) return false;
        const auto path = module_directory() + L"\\shift2-telemetry.dll";
        const auto dll = LoadLibraryW(path.c_str());
        if (!dll) { log_message("Telemetry load failed: %lu", GetLastError()); return false; }
        using Start = DWORD(WINAPI*)();
        const auto start = reinterpret_cast<Start>(GetProcAddress(dll, "Shift2TelemetryStart"));
        const DWORD result = start ? start() : ERROR_PROC_NOT_FOUND;
        log_message("Telemetry start result=%lu (0=active)", result);
        return result == ERROR_SUCCESS;
    }();
    (void)attempted;
}
