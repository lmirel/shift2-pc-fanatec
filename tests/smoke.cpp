#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static GUID instance{};
static unsigned wheels = 0;
static BOOL CALLBACK enumerate(const DIDEVICEINSTANCEA* info, void*)
{
    if (info->guidProduct.Data1 == 0xc29a046d) {
        instance = info->guidInstance;
        ++wheels;
        printf("Found %s VID/PID=%08lx\n", info->tszProductName, info->guidProduct.Data1);
    }
    return DIENUM_CONTINUE;
}
static void require(bool success, const char* message)
{
    if (!success) {
        printf("FAIL: %s\n", message);
        fflush(stdout);
        ExitProcess(1);
    }
}
int main(int argc, char** argv)
{
    const auto library = LoadLibraryW(L"dinput8.dll");
    require(library != nullptr, "load bridge");
    using Create = HRESULT(WINAPI*)(HINSTANCE,DWORD,REFIID,LPVOID*,LPUNKNOWN);
    const auto create = reinterpret_cast<Create>(GetProcAddress(library, "DirectInput8Create"));
    require(create != nullptr, "export DirectInput8Create");
    IDirectInput8A* input = nullptr;
    require(SUCCEEDED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
        reinterpret_cast<void**>(&input), nullptr)), "create DirectInput A");
    require(SUCCEEDED(input->EnumDevices(DI8DEVCLASS_GAMECTRL, enumerate, nullptr, DIEDFL_ATTACHEDONLY)), "enumerate");
    require(wheels == 1, "exactly one virtual DFGT");
    IDirectInputDevice8A* keyboard = nullptr;
    require(SUCCEEDED(input->CreateDevice(GUID_SysKeyboard, &keyboard, nullptr)), "system keyboard forwarding");
    keyboard->Release();
    IDirectInputDevice8A* wheel = nullptr;
    require(SUCCEEDED(input->CreateDevice(instance, &wheel, nullptr)), "create wheel");
    DIDEVCAPS caps{sizeof(caps)};
    require(SUCCEEDED(wheel->GetCapabilities(&caps)) && caps.dwAxes == 3 && caps.dwButtons == 21 &&
        (caps.dwFlags & DIDC_FORCEFEEDBACK), "wheel capabilities");
    require(SUCCEEDED(wheel->SetDataFormat(&c_dfDIJoystick2)), "DIJOYSTATE2 format");
    require(SUCCEEDED(wheel->Acquire()), "acquire");
    DIJOYSTATE2 state{};
    require(SUCCEEDED(wheel->GetDeviceState(sizeof(state), &state)), "read input");
    require(state.rgdwPOV[1] == 0xffffffff, "unavailable POV stays neutral");
    require(wheel->GetDeviceState(1, &state) == DIERR_INVALIDPARAM, "reject undersized state");
    wheel->Unacquire();
    DIPROPDWORD buffer{{sizeof(buffer),sizeof(DIPROPHEADER),0,DIPH_DEVICE},32};
    require(SUCCEEDED(wheel->SetProperty(DIPROP_BUFFERSIZE, &buffer.diph)), "buffer property");
    DIPROPRANGE range{{sizeof(range),sizeof(DIPROPHEADER),0,DIPH_DEVICE},-10000,10000};
    require(SUCCEEDED(wheel->SetProperty(DIPROP_RANGE, &range.diph)), "axis range");
    wheel->Acquire();
    DWORD axis = DIJOFS_X;
    LONG direction = 10000;
    DICONSTANTFORCE constant{0};
    DIEFFECT description{};
    description.dwSize = sizeof(description);
    description.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    description.dwDuration = 100000;
    description.dwGain = 10000;
    description.dwTriggerButton = DIEB_NOTRIGGER;
    description.cAxes = 1;
    description.rgdwAxes = &axis;
    description.rglDirection = &direction;
    description.cbTypeSpecificParams = sizeof(constant);
    description.lpvTypeSpecificParams = &constant;
    IDirectInputEffect* effect = nullptr;
    require(SUCCEEDED(wheel->CreateEffect(GUID_ConstantForce, &description, &effect, nullptr)), "create zero-force effect");
    require(SUCCEEDED(effect->Start(1, 0)), "start zero-force effect");
    DWORD status = 0;
    effect->GetEffectStatus(&status);
    require(status & DIEGES_PLAYING, "effect is playing");
    Sleep(120);
    effect->GetEffectStatus(&status);
    require(!(status & DIEGES_PLAYING), "finite effect expires");
    effect->Stop();
    effect->Release();
    if (argc > 1 && strcmp(argv[1], "--watch") == 0) {
        puts("Watching wheel for 30 seconds. No nonzero force is generated.");
        for (unsigned index = 0; index < 150; ++index) {
            wheel->Poll();
            wheel->GetDeviceState(sizeof(state), &state);
            unsigned pressed = 0;
            for (unsigned button = 0; button < 21; ++button)
                if (state.rgbButtons[button]) pressed |= 1u << button;
            printf("steering=%6ld throttle=%6ld brake=%6ld pov=%08lx buttons=%06x\n",
                state.lX, state.lY, state.lRz, state.rgdwPOV[0], pressed);
            Sleep(200);
        }
    }
    wheel->Unacquire();
    wheel->Release();
    input->Release();
    IDirectInput8W* wide = nullptr;
    require(SUCCEEDED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
        reinterpret_cast<void**>(&wide), nullptr)), "create DirectInput W");
    IDirectInputDevice8W* wide_wheel = nullptr;
    require(SUCCEEDED(wide->CreateDevice(instance, &wide_wheel, nullptr)), "create Unicode wheel");
    wide_wheel->Release();
    wide->Release();
    puts("PASS: virtual DFGT, keyboard forwarding, formats, properties, effect lifecycle, ANSI and Unicode.");
    FreeLibrary(library);
}
