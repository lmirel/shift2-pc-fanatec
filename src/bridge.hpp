#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <winusb.h>
#include <setupapi.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern HMODULE bridge_module;
void log_message(const char* format, ...);
std::wstring module_directory();
int setting(const wchar_t* name, int fallback);

inline const GUID wheel_instance = {0x9a7ae124,0x4483,0x4d4a,{0x88,0x7a,0x55,0xf1,0x23,0x01,0x42,0x61}};
inline const GUID wheel_product = {0xc29a046d,0,0,{0,0,0x50,0x49,0x44,0x56,0x49,0x44}};

struct InputNotification {
    std::atomic<HANDLE> event{nullptr};
};

struct WheelSample {
    std::array<LONG, 3> axes{32768,65535,65535};
    DWORD pov = 0xffffffff;
    std::array<BYTE, 21> buttons{};
    bool connected = false;
    uint64_t sequence = 0;
    double velocity = 0;
    double acceleration = 0;
    ULONGLONG timestamp = 0;
};

struct EffectState {
    GUID type{};
    IDirectInputEffect* effect_interface = nullptr;
    uintptr_t owner = 0;
    DWORD duration = INFINITE;
    DWORD gain = 10000;
    DWORD delay = 0;
    DWORD trigger = DIEB_NOTRIGGER;
    DWORD repeat = INFINITE;
    DWORD sample_period = 0;
    DWORD iterations = 1;
    DWORD coordinate_flags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    DWORD axis = DIJOFS_X;
    LONG direction = 10000;
    std::vector<BYTE> parameters;
    DIENVELOPE envelope{};
    bool has_envelope = false;
    bool downloaded = true;
    bool playing = false;
    ULONGLONG started = 0;
    bool triggered_down = false;
    ULONGLONG last_trigger = 0;
};

DWORD effect_kind(REFGUID type);
double evaluate_effect(const EffectState& effect, const WheelSample& sample, ULONGLONG now);

class WheelEngine {
public:
    WheelEngine();
    ~WheelEngine();
    WheelSample sample();
    std::vector<WheelSample> samples_after(uint64_t sequence);
    std::shared_ptr<InputNotification> subscribe();
    void attach_effect(const std::shared_ptr<EffectState>& effect);
    void stop_effects();
    std::mutex effect_mutex;
    std::vector<std::weak_ptr<EffectState>> effects;
    std::atomic<unsigned> acquisitions{0};
    std::atomic<DWORD> gain{10000};
    std::atomic<bool> actuators{true};
    std::atomic<bool> paused{false};
    std::atomic<ULONGLONG> paused_at{0};
    int max_force_percent = 20;
    int force_sign = 1;
private:
    void run();
    bool open_wheel();
    void close_wheel();
    bool send_command(const std::array<BYTE,7>& command);
    void publish_report(const std::array<BYTE,64>& report);
    void update_force();
    std::atomic<bool> stopping{false};
    std::thread worker;
    HANDLE usb_file = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    std::mutex sample_mutex;
    WheelSample current;
    std::deque<WheelSample> history;
    std::vector<std::weak_ptr<InputNotification>> notifications;
    void notify_input_locked();
    bool io_failed = false;
    int last_force = -1;
    ULONGLONG last_output = 0;
};
std::shared_ptr<WheelEngine> wheel_engine();

class BridgeEffect final : public IDirectInputEffect {
public:
    BridgeEffect(std::shared_ptr<WheelEngine> engine, REFGUID type, uintptr_t owner);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override;
    HRESULT STDMETHODCALLTYPE GetEffectGuid(LPGUID) override;
    HRESULT STDMETHODCALLTYPE GetParameters(LPDIEFFECT, DWORD) override;
    HRESULT STDMETHODCALLTYPE SetParameters(LPCDIEFFECT, DWORD) override;
    HRESULT STDMETHODCALLTYPE Start(DWORD, DWORD) override;
    HRESULT STDMETHODCALLTYPE Stop() override;
    HRESULT STDMETHODCALLTYPE GetEffectStatus(LPDWORD) override;
    HRESULT STDMETHODCALLTYPE Download() override;
    HRESULT STDMETHODCALLTYPE Unload() override;
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override;
    std::shared_ptr<EffectState> state;
private:
    std::atomic<ULONG> references{1};
    std::shared_ptr<WheelEngine> engine;
};
