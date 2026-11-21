#include "bridge.hpp"
#include <limits>

DWORD effect_kind(REFGUID type)
{
    if (type == GUID_ConstantForce) return DIEFT_CONSTANTFORCE;
    if (type == GUID_RampForce) return DIEFT_RAMPFORCE;
    if (type == GUID_Spring || type == GUID_Damper || type == GUID_Friction || type == GUID_Inertia)
        return DIEFT_CONDITION;
    if (type == GUID_Sine || type == GUID_Square || type == GUID_Triangle ||
        type == GUID_SawtoothUp || type == GUID_SawtoothDown)
        return DIEFT_PERIODIC;
    return 0;
}

double evaluate_effect(const EffectState& effect, const WheelSample& sample, ULONGLONG now)
{
    if (!effect.playing || !effect.downloaded || !effect.started || now < effect.started)
        return 0;
    const double elapsed = (now - effect.started) * 1000.0;
    if (elapsed < effect.delay)
        return 0;
    double time = elapsed - effect.delay;
    if (effect.duration != INFINITE) {
        if (!effect.duration || (effect.iterations != INFINITE && time >= double(effect.duration) * effect.iterations))
            return 0;
        time = std::fmod(time, effect.duration);
    }
    double force = 0;
    const DWORD kind = effect_kind(effect.type);
    if (kind == DIEFT_CONSTANTFORCE && effect.parameters.size() == sizeof(DICONSTANTFORCE)) {
        DICONSTANTFORCE value{};
        memcpy(&value, effect.parameters.data(), sizeof(value));
        force = value.lMagnitude / 10000.0;
    } else if (kind == DIEFT_RAMPFORCE && effect.parameters.size() == sizeof(DIRAMPFORCE)) {
        DIRAMPFORCE value{};
        memcpy(&value, effect.parameters.data(), sizeof(value));
        const double fraction = effect.duration == INFINITE ? 0 : time / effect.duration;
        force = (value.lStart + (value.lEnd - value.lStart) * fraction) / 10000.0;
    } else if (kind == DIEFT_PERIODIC && effect.parameters.size() == sizeof(DIPERIODIC)) {
        DIPERIODIC value{};
        memcpy(&value, effect.parameters.data(), sizeof(value));
        if (!value.dwPeriod)
            return 0;
        const double phase = std::fmod(time / value.dwPeriod + value.dwPhase / 36000.0, 1.0);
        double wave = std::sin(phase * 6.283185307179586);
        if (effect.type == GUID_Square) wave = phase < 0.5 ? 1 : -1;
        if (effect.type == GUID_Triangle) wave = 1 - 4 * std::abs(phase - 0.5);
        if (effect.type == GUID_SawtoothUp) wave = 2 * phase - 1;
        if (effect.type == GUID_SawtoothDown) wave = 1 - 2 * phase;
        force = (value.lOffset + value.dwMagnitude * wave) / 10000.0;
    } else if (kind == DIEFT_CONDITION && effect.parameters.size() == sizeof(DICONDITION)) {
        DICONDITION value{};
        memcpy(&value, effect.parameters.data(), sizeof(value));
        double position = sample.axes[0] / 32767.5 - 1;
        if (effect.type == GUID_Damper) position = std::clamp(sample.velocity / 4.0, -1.0, 1.0);
        if (effect.type == GUID_Inertia) position = std::clamp(sample.acceleration / 40.0, -1.0, 1.0);
        if (effect.type == GUID_Friction) position = sample.velocity > 0.01 ? 1 : (sample.velocity < -0.01 ? -1 : 0);
        double delta = position * 10000 - value.lOffset;
        if (std::abs(delta) <= value.lDeadBand)
            return 0;
        delta += delta > 0 ? -value.lDeadBand : value.lDeadBand;
        const double coefficient = delta > 0 ? value.lPositiveCoefficient : value.lNegativeCoefficient;
        const double saturation = delta > 0 ? value.dwPositiveSaturation : value.dwNegativeSaturation;
        force = -std::clamp(delta * coefficient / 10000.0, -saturation, saturation) / 10000.0;
    }
    if (effect.has_envelope && kind != DIEFT_CONDITION) {
        double envelope = 1;
        if (effect.envelope.dwAttackTime && time < effect.envelope.dwAttackTime) {
            const double start = effect.envelope.dwAttackLevel / 10000.0;
            envelope = start + (1 - start) * time / effect.envelope.dwAttackTime;
        }
        if (effect.duration != INFINITE && effect.envelope.dwFadeTime &&
            time > double(effect.duration) - effect.envelope.dwFadeTime) {
            const double remaining = effect.duration - time;
            const double end = effect.envelope.dwFadeLevel / 10000.0;
            envelope = std::min(envelope, end + (1 - end) * remaining / effect.envelope.dwFadeTime);
        }
        force *= envelope;
    }
    if (kind != DIEFT_CONDITION) {
        if (effect.coordinate_flags & DIEFF_CARTESIAN)
            force *= effect.direction < 0 ? -1 : 1;
        else
            force *= std::cos(effect.direction / 18000.0 * 3.141592653589793);
    }
    return force * effect.gain / 10000.0;
}

BridgeEffect::BridgeEffect(std::shared_ptr<WheelEngine> owner, REFGUID type, uintptr_t device)
    : state(std::make_shared<EffectState>()), engine(std::move(owner))
{
    state->type = type;
    state->owner = device;
    state->effect_interface = this;
    engine->attach_effect(state);
}

HRESULT STDMETHODCALLTYPE BridgeEffect::QueryInterface(REFIID iid, void** out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDirectInputEffect) return E_NOINTERFACE;
    *out = static_cast<IDirectInputEffect*>(this);
    AddRef();
    return S_OK;
}
ULONG STDMETHODCALLTYPE BridgeEffect::AddRef() { return ++references; }
ULONG STDMETHODCALLTYPE BridgeEffect::Release()
{
    ULONG count = 0;
    {
        std::lock_guard lock(engine->effect_mutex);
        count = --references;
        if (!count) {
            state->playing = false;
            state->effect_interface = nullptr;
        }
    }
    if (!count) delete this;
    return count;
}
HRESULT STDMETHODCALLTYPE BridgeEffect::Initialize(HINSTANCE, DWORD, REFGUID) { return DI_OK; }
HRESULT STDMETHODCALLTYPE BridgeEffect::GetEffectGuid(LPGUID out)
{
    if (!out) return E_POINTER;
    *out = state->type;
    return DI_OK;
}

HRESULT STDMETHODCALLTYPE BridgeEffect::SetParameters(LPCDIEFFECT input, DWORD flags)
{
    if (!input || input->dwSize != sizeof(DIEFFECT)) return DIERR_INVALIDPARAM;
    std::lock_guard lock(engine->effect_mutex);
    EffectState next = *state;
    if (flags & DIEP_AXES) {
        if (input->cAxes != 1 || !input->rgdwAxes) return DIERR_INVALIDPARAM;
        const DWORD axis = input->rgdwAxes[0];
        if ((input->dwFlags & DIEFF_OBJECTOFFSETS) ? axis != DIJOFS_X :
            (DIDFT_GETINSTANCE(axis) != 0 || !(axis & DIDFT_AXIS)))
            return DIERR_UNSUPPORTED;
        next.axis = axis;
        next.coordinate_flags = input->dwFlags;
    }
    if (flags & DIEP_DIRECTION) {
        if (input->cAxes != 1 || !input->rglDirection) return DIERR_INVALIDPARAM;
        next.direction = input->rglDirection[0];
        next.coordinate_flags = input->dwFlags;
    }
    if (flags & DIEP_TYPESPECIFICPARAMS) {
        const DWORD kind = effect_kind(next.type);
        const size_t bytes = kind == DIEFT_CONSTANTFORCE ? sizeof(DICONSTANTFORCE) :
            kind == DIEFT_RAMPFORCE ? sizeof(DIRAMPFORCE) :
            kind == DIEFT_PERIODIC ? sizeof(DIPERIODIC) : sizeof(DICONDITION);
        if (!input->lpvTypeSpecificParams || input->cbTypeSpecificParams != bytes)
            return DIERR_INVALIDPARAM;
        const auto data = static_cast<const BYTE*>(input->lpvTypeSpecificParams);
        next.parameters.assign(data, data + bytes);
    }
    if (flags & DIEP_DURATION) next.duration = input->dwDuration;
    if (flags & DIEP_SAMPLEPERIOD) next.sample_period = input->dwSamplePeriod;
    if (flags & DIEP_GAIN) next.gain = std::min(input->dwGain, DWORD(10000));
    if (flags & DIEP_TRIGGERBUTTON) {
        next.trigger = input->dwTriggerButton;
        if (next.trigger != DIEB_NOTRIGGER) {
            if (input->dwFlags & DIEFF_OBJECTOFFSETS) {
                if (next.trigger < DIJOFS_BUTTON0 || next.trigger > DIJOFS_BUTTON(20)) return DIERR_INVALIDPARAM;
                next.trigger -= DIJOFS_BUTTON0;
            } else {
                next.trigger = DIDFT_GETINSTANCE(next.trigger);
                if (next.trigger >= 21) return DIERR_INVALIDPARAM;
            }
        }
    }
    if (flags & DIEP_TRIGGERREPEATINTERVAL) next.repeat = input->dwTriggerRepeatInterval;
    if (flags & DIEP_STARTDELAY) next.delay = input->dwStartDelay;
    if (flags & DIEP_ENVELOPE) {
        next.has_envelope = input->lpEnvelope != nullptr;
        if (next.has_envelope) {
            if (input->lpEnvelope->dwSize != sizeof(DIENVELOPE)) return DIERR_INVALIDPARAM;
            next.envelope = *input->lpEnvelope;
        }
    }
    if (!(flags & DIEP_NODOWNLOAD)) next.downloaded = true;
    if (flags & DIEP_START) {
        next.playing = true;
        next.started = next.trigger == DIEB_NOTRIGGER ? GetTickCount64() : 0;
        next.iterations = 1;
    }
    *state = std::move(next);
    return DI_OK;
}

HRESULT STDMETHODCALLTYPE BridgeEffect::GetParameters(LPDIEFFECT output, DWORD flags)
{
    if (!output || output->dwSize != sizeof(DIEFFECT)) return DIERR_INVALIDPARAM;
    std::lock_guard lock(engine->effect_mutex);
    if (flags & (DIEP_AXES | DIEP_DIRECTION)) {
        if (output->cAxes < 1) {
            output->cAxes = 1;
            return DIERR_MOREDATA;
        }
        const bool by_offset = output->dwFlags & DIEFF_OBJECTOFFSETS;
        if ((flags & DIEP_AXES) && !output->rgdwAxes) return DIERR_INVALIDPARAM;
        if ((flags & DIEP_DIRECTION) && !output->rglDirection) return DIERR_INVALIDPARAM;
        if (flags & DIEP_AXES) output->rgdwAxes[0] = by_offset ? DIJOFS_X : DIDFT_ABSAXIS;
        if (flags & DIEP_DIRECTION) output->rglDirection[0] = state->direction;
        output->cAxes = 1;
    }
    if (flags & DIEP_TYPESPECIFICPARAMS) {
        if (output->cbTypeSpecificParams < state->parameters.size()) {
            output->cbTypeSpecificParams = static_cast<DWORD>(state->parameters.size());
            return DIERR_MOREDATA;
        }
        if (!state->parameters.empty() && !output->lpvTypeSpecificParams) return DIERR_INVALIDPARAM;
        memcpy(output->lpvTypeSpecificParams, state->parameters.data(), state->parameters.size());
        output->cbTypeSpecificParams = static_cast<DWORD>(state->parameters.size());
    }
    if (flags & DIEP_DURATION) output->dwDuration = state->duration;
    if (flags & DIEP_SAMPLEPERIOD) output->dwSamplePeriod = state->sample_period;
    if (flags & DIEP_GAIN) output->dwGain = state->gain;
    if (flags & DIEP_TRIGGERBUTTON) output->dwTriggerButton = state->trigger == DIEB_NOTRIGGER ? DIEB_NOTRIGGER :
        ((output->dwFlags & DIEFF_OBJECTOFFSETS) ? DIJOFS_BUTTON(state->trigger) : DIDFT_BUTTON | DIDFT_MAKEINSTANCE(state->trigger));
    if (flags & DIEP_TRIGGERREPEATINTERVAL) output->dwTriggerRepeatInterval = state->repeat;
    if (flags & DIEP_STARTDELAY) output->dwStartDelay = state->delay;
    if (flags & DIEP_ENVELOPE) {
        if (state->has_envelope && output->lpEnvelope) *output->lpEnvelope = state->envelope;
        else output->lpEnvelope = nullptr;
    }
    return DI_OK;
}

HRESULT STDMETHODCALLTYPE BridgeEffect::Start(DWORD iterations, DWORD flags)
{
    if (!iterations) return DIERR_INVALIDPARAM;
    if (flags & DIES_SOLO) engine->stop_effects();
    std::lock_guard lock(engine->effect_mutex);
    if (!state->downloaded && (flags & DIES_NODOWNLOAD)) return DIERR_NOTDOWNLOADED;
    state->downloaded = true;
    state->iterations = iterations;
    state->started = state->trigger == DIEB_NOTRIGGER ? GetTickCount64() : 0;
    state->triggered_down = false;
    state->playing = true;
    log_message("FFB Start kind=%lu iterations=%lu", effect_kind(state->type), iterations);
    return DI_OK;
}
HRESULT STDMETHODCALLTYPE BridgeEffect::Stop()
{
    std::lock_guard lock(engine->effect_mutex);
    state->playing = false;
    return DI_OK;
}
HRESULT STDMETHODCALLTYPE BridgeEffect::GetEffectStatus(LPDWORD output)
{
    if (!output) return E_POINTER;
    std::lock_guard lock(engine->effect_mutex);
    const auto now = engine->paused ? engine->paused_at.load() : GetTickCount64();
    bool playing = state->playing;
    if (state->duration != INFINITE && state->iterations != INFINITE && state->started)
        playing = playing && (now - state->started) * 1000.0 < state->delay + double(state->duration) * state->iterations;
    *output = playing ? DIEGES_PLAYING : 0;
    return DI_OK;
}
HRESULT STDMETHODCALLTYPE BridgeEffect::Download()
{
    std::lock_guard lock(engine->effect_mutex);
    state->downloaded = true;
    return DI_OK;
}
HRESULT STDMETHODCALLTYPE BridgeEffect::Unload()
{
    std::lock_guard lock(engine->effect_mutex);
    state->playing = false;
    state->downloaded = false;
    return DI_OK;
}
HRESULT STDMETHODCALLTYPE BridgeEffect::Escape(LPDIEFFESCAPE) { return DIERR_UNSUPPORTED; }
