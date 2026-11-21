#include "bridge.hpp"
#include <cwctype>

static std::vector<std::wstring> wheel_paths()
{
    std::vector<std::wstring> paths;
    HDEVINFO devices = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devices == INVALID_HANDLE_VALUE)
        return paths;
    SP_DEVINFO_DATA device{sizeof(device)};
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &device); ++index) {
        wchar_t id[512]{};
        if (!SetupDiGetDeviceInstanceIdW(devices, &device, id, static_cast<DWORD>(std::size(id)), nullptr))
            continue;
        std::wstring lower(id);
        std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
        if (lower.find(L"vid_0eb7&pid_0e04") == std::wstring::npos)
            continue;
        if (lower.find(L"&mi_") != std::wstring::npos && lower.find(L"&mi_00") == std::wstring::npos)
            continue;
        HKEY key = SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE)
            continue;
        wchar_t values[4096]{};
        DWORD bytes = sizeof(values);
        DWORD type = 0;
        LONG status = RegQueryValueExW(key, L"DeviceInterfaceGUIDs", nullptr, &type,
            reinterpret_cast<BYTE*>(values), &bytes);
        RegCloseKey(key);
        if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_MULTI_SZ))
            continue;
        for (const wchar_t* value = values; *value; value += wcslen(value) + 1) {
            GUID guid{};
            if (FAILED(CLSIDFromString(value, &guid)))
                continue;
            HDEVINFO interfaces = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (interfaces == INVALID_HANDLE_VALUE)
                continue;
            SP_DEVICE_INTERFACE_DATA data{sizeof(data)};
            for (DWORD number = 0; SetupDiEnumDeviceInterfaces(interfaces, nullptr, &guid, number, &data); ++number) {
                DWORD required = 0;
                SetupDiGetDeviceInterfaceDetailW(interfaces, &data, nullptr, 0, &required, nullptr);
                if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
                    continue;
                std::vector<BYTE> buffer(required);
                auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
                detail->cbSize = sizeof(*detail);
                SP_DEVINFO_DATA owner{sizeof(owner)};
                if (!SetupDiGetDeviceInterfaceDetailW(interfaces, &data, detail, required, nullptr, &owner))
                    continue;
                wchar_t owner_id[512]{};
                if (SetupDiGetDeviceInstanceIdW(interfaces, &owner, owner_id, static_cast<DWORD>(std::size(owner_id)), nullptr)
                    && _wcsicmp(owner_id, id) == 0)
                    paths.emplace_back(detail->DevicePath);
            }
            SetupDiDestroyDeviceInfoList(interfaces);
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return paths;
}

std::shared_ptr<WheelEngine> wheel_engine()
{
    static std::mutex mutex;
    static std::weak_ptr<WheelEngine> instance;
    std::lock_guard lock(mutex);
    auto result = instance.lock();
    if (!result) {
        result = std::make_shared<WheelEngine>();
        instance = result;
    }
    return result;
}

WheelEngine::WheelEngine()
{
    max_force_percent = std::clamp(setting(L"MaxForcePercent", 20), 0, 100);
    force_sign = setting(L"InvertForce", 0) ? -1 : 1;
    log_message("USB backend starting; output cap=%d%% invert=%d", max_force_percent, force_sign < 0);
    worker = std::thread(&WheelEngine::run, this);
}

WheelEngine::~WheelEngine()
{
    stopping = true;
    worker.join();
}

WheelSample WheelEngine::sample()
{
    std::lock_guard lock(sample_mutex);
    return current;
}

std::vector<WheelSample> WheelEngine::samples_after(uint64_t sequence)
{
    std::lock_guard lock(sample_mutex);
    std::vector<WheelSample> result;
    for (const auto& sample : history)
        if (sample.sequence > sequence) result.push_back(sample);
    return result;
}

std::shared_ptr<InputNotification> WheelEngine::subscribe()
{
    auto result = std::make_shared<InputNotification>();
    std::lock_guard lock(sample_mutex);
    notifications.emplace_back(result);
    return result;
}

void WheelEngine::notify_input_locked()
{
    for (auto it = notifications.begin(); it != notifications.end();) {
        if (auto notification = it->lock()) {
            if (HANDLE event = notification->event.load()) SetEvent(event);
            ++it;
        } else {
            it = notifications.erase(it);
        }
    }
}

void WheelEngine::attach_effect(const std::shared_ptr<EffectState>& effect)
{
    std::lock_guard lock(effect_mutex);
    effects.emplace_back(effect);
}

void WheelEngine::stop_effects()
{
    std::lock_guard lock(effect_mutex);
    for (auto& weak : effects) {
        if (auto effect = weak.lock())
            effect->playing = false;
    }
}

bool WheelEngine::send_command(const std::array<BYTE,7>& command)
{
    std::array<BYTE,64> report{0x30};
    std::copy(command.begin(), command.end(), report.begin() + 1);
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent)
        return false;
    ULONG written = 0;
    BOOL success = WinUsb_WritePipe(usb, 0x03, report.data(), static_cast<ULONG>(report.size()), &written, &operation);
    if (!success && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(operation.hEvent, 100) != WAIT_OBJECT_0)
            CancelIoEx(usb_file, &operation);
        success = WinUsb_GetOverlappedResult(usb, &operation, &written, TRUE);
    }
    const DWORD error = success ? 0 : GetLastError();
    CloseHandle(operation.hEvent);
    if (!success || written != report.size()) {
        log_message("WinUSB write failed error=%lu bytes=%lu", error, written);
        return false;
    }
    return true;
}

bool WheelEngine::open_wheel()
{
    for (const auto& path : wheel_paths()) {
        usb_file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (usb_file == INVALID_HANDLE_VALUE)
            continue;
        if (!WinUsb_Initialize(usb_file, &usb)) {
            log_message("WinUsb_Initialize failed error=%lu", GetLastError());
            CloseHandle(usb_file);
            usb_file = INVALID_HANDLE_VALUE;
            continue;
        }
        USB_INTERFACE_DESCRIPTOR descriptor{};
        if (!WinUsb_QueryInterfaceSettings(usb, 0, &descriptor) || descriptor.bInterfaceNumber != 0) {
            WinUsb_Free(usb);
            usb = nullptr;
            CloseHandle(usb_file);
            usb_file = INVALID_HANDLE_VALUE;
            continue;
        }
        ULONG timeout = 100;
        WinUsb_SetPipePolicy(usb, 0x03, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
        if (!send_command({0xf3,0,0,0,0,0,0}) || !send_command({0xf5,0,0,0,0,0,0})) {
            close_wheel();
            continue;
        }
        io_failed = false;
        last_force = -1;
        last_output = 0;
        log_message("Opened Fanatec 0eb7:0e04 interface 0");
        return true;
    }
    return false;
}

void WheelEngine::close_wheel()
{
    if (usb) {
        send_command({0xf3,0,0,0,0,0,0});
        WinUsb_Free(usb);
        usb = nullptr;
    }
    if (usb_file != INVALID_HANDLE_VALUE) {
        CloseHandle(usb_file);
        usb_file = INVALID_HANDLE_VALUE;
    }
    stop_effects();
    std::lock_guard lock(sample_mutex);
    WheelSample neutral;
    neutral.sequence = current.sequence + 1;
    neutral.timestamp = GetTickCount64();
    current = neutral;
    history.push_back(current);
    if (history.size() > 512) history.pop_front();
    notify_input_locked();
}

void WheelEngine::publish_report(const std::array<BYTE,64>& raw)
{
    if (raw[0] != 1)
        return;
    WheelSample next;
    next.connected = true;
    next.timestamp = GetTickCount64();
    next.axes = {LONG(raw[43] | (unsigned(raw[44]) << 8)), LONG(raw[46]) * 257, LONG(raw[48]) * 257};
    const unsigned hat = raw[5] & 15;
    next.pov = hat < 8 ? hat * 4500 : 0xffffffff;
    const BYTE first = (raw[5] & 0xc0) | ((raw[5] & 0x10) << 1) | ((raw[5] & 0x20) >> 1);
    const BYTE second = (raw[6] & 0x30) | ((raw[6] & 0x45) << 1) | ((raw[6] & 0x8a) >> 1);
    const BYTE extra = ((raw[54] & 1) << 2) | ((raw[54] & 0x10) >> 1) |
        ((raw[54] & 4) << 2) | ((raw[54] & 2) << 4) | ((raw[54] & 8) << 3);
    const unsigned bits = (first >> 4) | (unsigned(second) << 4) | (unsigned(extra) << 12) | (unsigned(raw[7] & 1) << 20);
    for (unsigned index = 0; index < next.buttons.size(); ++index)
        next.buttons[index] = (bits & (1u << index)) ? 0x80 : 0;
    std::lock_guard lock(sample_mutex);
    if (current.connected && next.timestamp > current.timestamp) {
        const double dt = (next.timestamp - current.timestamp) / 1000.0;
        const double velocity = (next.axes[0] - current.axes[0]) / (32767.5 * dt);
        next.velocity = current.velocity * 0.75 + velocity * 0.25;
        next.acceleration = current.acceleration * 0.9 + (next.velocity - current.velocity) / dt * 0.1;
    }
    const bool changed = next.axes != current.axes || next.buttons != current.buttons ||
        next.pov != current.pov || next.connected != current.connected;
    next.sequence = current.sequence + 1;
    current = next;
    history.push_back(next);
    if (history.size() > 512) history.pop_front();
    if (changed) notify_input_locked();
}

void WheelEngine::update_force()
{
    const auto input = sample();
    const auto now = GetTickCount64();
    double force = 0;
    if (input.connected && now - input.timestamp < 100 && acquisitions && actuators && !paused) {
        std::lock_guard lock(effect_mutex);
        for (auto it = effects.begin(); it != effects.end();) {
            auto effect = it->lock();
            if (!effect) {
                it = effects.erase(it);
                continue;
            }
            ++it;
            if (effect->trigger != DIEB_NOTRIGGER) {
                const DWORD button = effect->trigger;
                const bool down = button < input.buttons.size() && input.buttons[button] != 0;
                if (down && (!effect->triggered_down || (effect->repeat != INFINITE &&
                    now - effect->last_trigger >= effect->repeat / 1000))) {
                    effect->started = now;
                    effect->last_trigger = now;
                }
                effect->triggered_down = down;
            }
            force += evaluate_effect(*effect, input, now);
        }
        force = std::clamp(force, -1.0, 1.0) * gain.load() / 10000.0 * max_force_percent / 100.0 * force_sign;
    }
    const int level = std::clamp(static_cast<int>(std::lround(128 + std::clamp(force, -1.0, 1.0) * 127)), 1, 255);
    if (level == last_force && now - last_output < 100)
        return;
    const bool success = level == 128
        ? send_command({0x13,0,0,0,0,0,0})
        : send_command({0x11,0x08,static_cast<BYTE>(level),0x80,0,0,0});
    if (!success) {
        io_failed = true;
        return;
    }
    last_force = level;
    last_output = now;
}

void WheelEngine::run()
{
    ULONGLONG next_scan = 0;
    while (!stopping) {
        if (!usb) {
            if (GetTickCount64() >= next_scan) {
                open_wheel();
                next_scan = GetTickCount64() + 1000;
            }
            if (!usb) {
                Sleep(10);
                continue;
            }
        }
        std::array<BYTE,64> report{};
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent)
            break;
        ULONG count = 0;
        BOOL success = WinUsb_ReadPipe(usb, 0x84, report.data(), static_cast<ULONG>(report.size()), &count, &operation);
        DWORD error = success ? 0 : GetLastError();
        if (!success && error == ERROR_IO_PENDING) {
            while (!stopping && !io_failed && WaitForSingleObject(operation.hEvent, 4) == WAIT_TIMEOUT) {
                update_force();
                if (io_failed)
                    break;
            }
            if (stopping || io_failed) {
                CancelIoEx(usb_file, &operation);
            }
            if (usb)
                success = WinUsb_GetOverlappedResult(usb, &operation, &count, TRUE);
            error = success ? 0 : GetLastError();
        }
        CloseHandle(operation.hEvent);
        if (!usb)
            continue;
        if (io_failed) {
            close_wheel();
            continue;
        }
        if (success && count == report.size())
            publish_report(report);
        else if ((!success || io_failed) && !stopping) {
            log_message("WinUSB read failed error=%lu", error);
            close_wheel();
            continue;
        }
        update_force();
    }
    close_wheel();
}
