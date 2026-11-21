# Fanatec DFGT input and Shift 2 telemetry bridge

Two 32-bit Windows DLLs provide Fanatec WinUSB input/force feedback and native Shift 2 telemetry. No Logitech DFGT driver is used.

- `dinput8.dll`: presents a game-local Logitech Driving Force GT compatible DirectInput device; steering, separate accelerator/brake, POV/buttons and force feedback. Other devices forward to system DirectInput.
- `shift2-telemetry.dll`: optional, build-specific native physics capture; engine/gear/body telemetry through versioned shared memory. The input DLL loads it when TelemetryEnabled=1.
- Native physics capture measured 360 Hz (2.77778 ms step), about 355 samples/sec over a 40-second drive, zero capture drops. MFC integration publishes the latest completed state at 60 Hz without smoothing.

## Build

Install Visual Studio C++ Build Tools (MSVC x86/x64 tools and Windows SDK). No Visual Studio solution or IDE is needed. Both DLLs are **x86**, including on 64-bit Windows. Telemetry uses MSVC x86 inline assembly and SEH; this is not currently a Linux/MinGW build.

From a normal Windows command prompt:

```bat
build.cmd all check
```

The wrapper discovers Visual Studio with vswhere, selects x86 and invokes NMAKE. Or use an **x86 Developer Command Prompt**:

```bat
nmake /nologo /f Makefile.msvc all check
```

GNU Make on Windows can use the root Makefile wrapper, from the same compiler environment:

```bat
make all
make check
```

The MSVC/NMAKE backend is tested. The GNU Make wrapper delegates to it; GNU Make was not installed on the development host, so that entry point has not been exercised here.

Outputs: `build/make/dinput8.dll` and `build/make/shift2-telemetry.dll`. build.cmd also refreshes legacy development artifact locations build/dinput8.dll and telemetry/build/shift2-telemetry.dll. MSVC/Windows SDK and vendored MinHook are the only native build dependencies; Python is only needed for readout tools.

Targets: all, input, telemetry, check, test-effects, test-telemetry, check-hardware. `check` runs offline numerical effect and hook/ABI tests, including the completed-step detour and ret8 cleanup. `check-hardware` opens WinUSB; close other wheel users first. Build outputs are functionally equivalent builds from the same source, not a promise of byte-identical DLLs across compiler versions/timestamps.

## Wheel setup

Tested with Fanatec ClubSport DD+ in **purple PS4 Compatibility mode**, USB VID:PID0EB7:0E04, interface 0. Bind that wheel interface to **WinUSB** with Zadig. The bridge uses input endpoint0x84, output0x03 and Fanatec report0x30. It does not install a system-wide virtual joystick.

Copy both built DLLs and fanatec-dfgt.ini beside the 32-bit Shift 2 executable. In game choose Driving Force GT with separate pedals or Custom Wheel. The base range and game wheel preset must agree; the tested setup uses 540 degrees. The bridge does not change the base's physical range setting. MaxForcePercent=100 matches the tested baseline; this is a scaling limit in the bridge.

Telemetry is optional: set TelemetryEnabled=0 to use only input/FFB. The telemetry DLL only accepts the known 1.02 executable SHA256 **BF1F9FCFF587D55BA190707EFE53E58B41DD6A3B26FD59CFEED8A53A8F16D157**. It also validates hook signatures and fails closed for other executables while preserving the input bridge. Game files, UCP assets, PhysX libraries and physicstweaker.mrdf are not included or required to build these DLLs. The telemetry hook does not require the modified LiveTrace MRDF.

## Telemetry output

The native DLL hooks the completed local-body integrator, preserving CPU flags, registers, x87 and SSE state. It publishes coherent position, velocity, angular velocity, rotation matrix, drivetrain and step timing into `Local\Shift2Telemetry-v1-PID`. It also intercepts LiveTrace diagnostics; native LiveTrace file writing is replaced while enabled.

Read a running game with `python telemetry/monitor.py --pid PID --seconds 60 --output capture.jsonl`. `telemetry/decode.py` decodes correlated body records from a saved JSONL capture. The native ring includes completed-step timing metadata.

The MFC client/bridge integration is maintained in a separate repository and is not bundled here. Shared-memory consumers can use the published ring without depending on MFC. Pause/lost focus stops simulation updates; consumers should release motion when samples stop advancing.

Validated: solo forward gear, speed/RPM against dashboard, body position/velocity and rotation/angular velocity consistency. Neutral/reverse, other wheel models, opponents/replays and physical motion signs require further validation.

## Publishing and rollback

This folder is the source-only repository candidate. Put DLLs in GitHub Releases rather than committing build outputs. The companion runtime ZIP contains the two DLLs, config and third-party notices. No game payload, captures, logs, personal device serials or development backups are bundled. Copy aside existing game-local DLLs/config before installation; restore those files to roll back.

## License

Project-owned code is licensed under the [MIT License](LICENSE), copyright (c) 2026 lmirel. MinHook and its HDE components retain their own BSD-style terms and copyright notices. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), telemetry/vendor/minhook/LICENSE.txt, and [PROVENANCE.md](PROVENANCE.md). The MFC integration is maintained separately and is excluded from this repository and both release archives.
