#include "bridge.hpp"
#include <type_traits>

template<bool Wide> struct InputTypes;
template<> struct InputTypes<false> {
    using Input = IDirectInput8A;
    using Device = IDirectInputDevice8A;
    using Instance = DIDEVICEINSTANCEA;
    using Object = DIDEVICEOBJECTINSTANCEA;
    using EffectInfo = DIEFFECTINFOA;
    using EnumDevices = LPDIENUMDEVICESCALLBACKA;
    using EnumObjects = LPDIENUMDEVICEOBJECTSCALLBACKA;
    using EnumEffects = LPDIENUMEFFECTSCALLBACKA;
    using String = LPCSTR;
    using Actions = LPDIACTIONFORMATA;
    using Semantics = LPDIENUMDEVICESBYSEMANTICSCBA;
    using Configure = LPDICONFIGUREDEVICESPARAMSA;
    using Images = LPDIDEVICEIMAGEINFOHEADERA;
    static const GUID& input_iid() { return IID_IDirectInput8A; }
    static const GUID& device_iid() { return IID_IDirectInputDevice8A; }
};
template<> struct InputTypes<true> {
    using Input = IDirectInput8W;
    using Device = IDirectInputDevice8W;
    using Instance = DIDEVICEINSTANCEW;
    using Object = DIDEVICEOBJECTINSTANCEW;
    using EffectInfo = DIEFFECTINFOW;
    using EnumDevices = LPDIENUMDEVICESCALLBACKW;
    using EnumObjects = LPDIENUMDEVICEOBJECTSCALLBACKW;
    using EnumEffects = LPDIENUMEFFECTSCALLBACKW;
    using String = LPCWSTR;
    using Actions = LPDIACTIONFORMATW;
    using Semantics = LPDIENUMDEVICESBYSEMANTICSCBW;
    using Configure = LPDICONFIGUREDEVICESPARAMSW;
    using Images = LPDIDEVICEIMAGEINFOHEADERW;
    static const GUID& input_iid() { return IID_IDirectInput8W; }
    static const GUID& device_iid() { return IID_IDirectInputDevice8W; }
};

template<class Char, size_t Size>
void copy_name(Char (&output)[Size], const char* input)
{
    if constexpr (std::is_same_v<Char, wchar_t>)
        MultiByteToWideChar(CP_UTF8, 0, input, -1, output, static_cast<int>(Size));
    else
        strncpy_s(output, input, _TRUNCATE);
}

template<bool Wide>
typename InputTypes<Wide>::Instance wheel_info()
{
    typename InputTypes<Wide>::Instance result{};
    result.dwSize = sizeof(result);
    result.guidInstance = wheel_instance;
    result.guidProduct = wheel_product;
    result.dwDevType = DI8DEVTYPE_DRIVING | (DI8DEVTYPEDRIVING_LIMITED << 8);
    copy_name(result.tszInstanceName, "Logitech Driving Force GT USB");
    copy_name(result.tszProductName, "Logitech Driving Force GT USB");
    result.guidFFDriver = wheel_instance;
    result.wUsagePage = 1;
    result.wUsage = 4;
    return result;
}

struct ObjectDescription {
    GUID guid;
    DWORD type;
    DWORD offset;
    DWORD usage;
    const char* name;
    int axis;
    int button;
};

static std::vector<ObjectDescription> object_descriptions()
{
    std::vector<ObjectDescription> result{
        {GUID_XAxis,DIDFT_ABSAXIS,DIJOFS_X,0x30,"Steering",0,-1},
        {GUID_YAxis,DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(1),DIJOFS_Y,0x31,"Accelerator",1,-1},
        {GUID_RzAxis,DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(5),DIJOFS_RZ,0x35,"Brake",2,-1},
        {GUID_POV,DIDFT_POV,DIJOFS_POV(0),0x39,"D-pad",-1,-1}
    };
    for (unsigned index = 0; index < 21; ++index)
        result.push_back({GUID_Button,static_cast<DWORD>(DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(index)),static_cast<DWORD>(DIJOFS_BUTTON(index)),
            index + 1,"Button",-1,static_cast<int>(index)});
    return result;
}

static const std::vector<ObjectDescription> objects = object_descriptions();
static const std::array<GUID,10> effect_types{
    GUID_ConstantForce,GUID_RampForce,GUID_Sine,GUID_Square,GUID_Triangle,
    GUID_SawtoothUp,GUID_SawtoothDown,GUID_Spring,GUID_Damper,GUID_Friction
};

struct FormatEntry {
    unsigned object;
    DWORD offset;
    DWORD width;
    ULONG_PTR app_data = 0;
};

template<bool Wide>
class WheelDevice final : public InputTypes<Wide>::Device {
    using Types = InputTypes<Wide>;
    using Instance = typename Types::Instance;
    using Object = typename Types::Object;
    using EffectInfo = typename Types::EffectInfo;
public:
    WheelDevice() : engine(wheel_engine()), notification(engine->subscribe()) { log_message("Created virtual DFGT %s", Wide ? "W" : "A"); }
    ~WheelDevice()
    {
        Unacquire();
        std::lock_guard lock(engine->effect_mutex);
        for (auto& weak : engine->effects)
            if (auto effect = weak.lock(); effect && effect->owner == reinterpret_cast<uintptr_t>(this))
                effect->playing = false;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != Types::device_iid()) {
            log_message("Device QueryInterface unsupported %08lx", iid.Data1);
            return E_NOINTERFACE;
        }
        *output = static_cast<typename Types::Device*>(this);
        AddRef();
        return DI_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto count = --references;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS output) override
    {
        std::lock_guard state_lock(state_mutex);
        if (!output || output->dwSize < sizeof(DIDEVCAPS_DX3) || output->dwSize > sizeof(DIDEVCAPS))
            return DIERR_INVALIDPARAM;
        DIDEVCAPS caps{};
        caps.dwSize = output->dwSize;
        caps.dwFlags = DIDC_ATTACHED | DIDC_FORCEFEEDBACK | DIDC_POLLEDDATAFORMAT | DIDC_POLLEDDEVICE;
        caps.dwDevType = wheel_info<Wide>().dwDevType;
        caps.dwAxes = 3;
        caps.dwButtons = 21;
        caps.dwPOVs = 1;
        caps.dwFFSamplePeriod = 4000;
        caps.dwFFMinTimeResolution = 1000;
        caps.dwFirmwareRevision = 0x1350;
        caps.dwHardwareRevision = 1;
        caps.dwFFDriverVersion = 1;
        memcpy(output, &caps, output->dwSize);
        return DI_OK;
    }
    Object describe(unsigned index)
    {
        const auto& definition = objects[index];
        Object result{};
        result.dwSize = sizeof(result);
        result.guidType = definition.guid;
        result.dwOfs = definition.offset;
        for (const auto& entry : format)
            if (entry.object == index) result.dwOfs = entry.offset;
        result.dwType = definition.type;
        result.dwFlags = definition.axis >= 0 ? DIDOI_ASPECTPOSITION : 0;
        if (definition.axis == 0) result.dwFlags |= DIDOI_FFACTUATOR;
        copy_name(result.tszName, definition.name);
        result.wUsagePage = definition.button >= 0 ? 9 : 1;
        result.wUsage = static_cast<WORD>(definition.usage);
        result.dwFFMaxForce = definition.axis == 0 ? 10000 : 0;
        result.dwFFForceResolution = definition.axis == 0 ? 128 : 0;
        return result;
    }
    HRESULT STDMETHODCALLTYPE EnumObjects(typename Types::EnumObjects callback, LPVOID context, DWORD flags) override
    {
        std::lock_guard state_lock(state_mutex);
        if (!callback) return DIERR_INVALIDPARAM;
        log_message("EnumObjects flags=%08lx", flags);
        for (unsigned index = 0; index < objects.size(); ++index) {
            const auto& object = objects[index];
            const DWORD category = flags & 0xffu;
            if (category && !(category & object.type & 0xff)) continue;
            if ((flags & DIDFT_FFACTUATOR) && object.axis != 0) continue;
            auto info = describe(index);
            if (callback(&info, context) == DIENUM_STOP) break;
        }
        return DI_OK;
    }
    int find_object(DWORD value, DWORD how)
    {
        if (how == DIPH_BYOFFSET) {
            for (const auto& entry : format)
                if (entry.offset == value) return static_cast<int>(entry.object);
            if (format.empty()) {
                for (unsigned index = 0; index < objects.size(); ++index)
                    if (objects[index].offset == value) return static_cast<int>(index);
            }
        }
        if (how == DIPH_BYID) {
            for (unsigned index = 0; index < objects.size(); ++index)
                if ((objects[index].type & ~DIDFT_FFACTUATOR) == (value & ~DIDFT_FFACTUATOR))
                    return static_cast<int>(index);
        }
        if (how == DIPH_BYUSAGE) {
            for (unsigned index = 0; index < objects.size(); ++index)
                if (objects[index].usage == HIWORD(value) &&
                    (objects[index].button >= 0 ? 9u : 1u) == LOWORD(value))
                    return static_cast<int>(index);
        }
        return -1;
    }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID property, LPDIPROPHEADER header) override
    {
        std::lock_guard state_lock(state_mutex);
        if (!header || header->dwHeaderSize != sizeof(DIPROPHEADER)) return DIERR_INVALIDPARAM;
        const auto id = reinterpret_cast<ULONG_PTR>(&property);
        const int object = find_object(header->dwObj, header->dwHow);
        const int axis = object >= 0 ? objects[object].axis : 0;
        log_message("GetProperty id=%llu how=%lu object=%lu", static_cast<unsigned long long>(id), header->dwHow, header->dwObj);
        if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_RANGE)) {
            if (header->dwSize != sizeof(DIPROPRANGE) || axis < 0) return DIERR_INVALIDPARAM;
            auto output = reinterpret_cast<DIPROPRANGE*>(header);
            output->lMin = minimum[axis];
            output->lMax = maximum[axis];
            return DI_OK;
        }
        if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_GUIDANDPATH)) {
            if (header->dwSize != sizeof(DIPROPGUIDANDPATH)) return DIERR_INVALIDPARAM;
            auto output = reinterpret_cast<DIPROPGUIDANDPATH*>(header);
            output->guidClass = wheel_instance;
            wcscpy_s(output->wszPath, L"\\\\?\\fanatec-dfgt#vid_046d&pid_c29a");
            return DI_OK;
        }
        if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_INSTANCENAME) || id == reinterpret_cast<ULONG_PTR>(&DIPROP_PRODUCTNAME)) {
            if (header->dwSize != sizeof(DIPROPSTRING)) return DIERR_INVALIDPARAM;
            wcscpy_s(reinterpret_cast<DIPROPSTRING*>(header)->wsz, L"Logitech Driving Force GT USB");
            return DI_OK;
        }
        if (header->dwSize != sizeof(DIPROPDWORD)) return DIERR_UNSUPPORTED;
        auto output = reinterpret_cast<DIPROPDWORD*>(header);
        if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_BUFFERSIZE)) output->dwData = buffer_size;
        else if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_AXISMODE)) output->dwData = DIPROPAXISMODE_ABS;
        else if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_GRANULARITY)) output->dwData = 1;
        else if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_DEADZONE) && axis >= 0) output->dwData = deadzone[axis];
        else if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_SATURATION) && axis >= 0) output->dwData = saturation[axis];
        else if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_FFGAIN)) output->dwData = engine->gain;
        else if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_FFLOAD)) output->dwData = 0;
        else if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_AUTOCENTER)) output->dwData = DIPROPAUTOCENTER_OFF;
        else if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_VIDPID)) output->dwData = 0xc29a046d;
        else if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_JOYSTICKID)) output->dwData = 0;
        else return DIERR_UNSUPPORTED;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID property, LPCDIPROPHEADER header) override
    {
        std::lock_guard state_lock(state_mutex);
        if (!header || header->dwHeaderSize != sizeof(DIPROPHEADER)) return DIERR_INVALIDPARAM;
        const auto id = reinterpret_cast<ULONG_PTR>(&property);
        log_message("SetProperty id=%llu how=%lu object=%lu", static_cast<unsigned long long>(id), header->dwHow, header->dwObj);
        if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_RANGE)) {
            if (header->dwSize != sizeof(DIPROPRANGE)) return DIERR_INVALIDPARAM;
            const auto range = reinterpret_cast<const DIPROPRANGE*>(header);
            if (range->lMin >= range->lMax) return DIERR_INVALIDPARAM;
            const int object = find_object(header->dwObj, header->dwHow);
            if (header->dwHow != DIPH_DEVICE && (object < 0 || objects[object].axis < 0)) return DIERR_OBJECTNOTFOUND;
            for (unsigned axis = 0; axis < 3; ++axis) {
                if (header->dwHow == DIPH_DEVICE || objects[object].axis == static_cast<int>(axis)) {
                    minimum[axis] = range->lMin;
                    maximum[axis] = range->lMax;
                }
            }
            return DI_OK;
        }
        if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_APPDATA)) {
            if (header->dwSize != sizeof(DIPROPPOINTER)) return DIERR_INVALIDPARAM;
            for (auto& entry : format) {
                if (static_cast<int>(entry.object) == find_object(header->dwObj, header->dwHow)) {
                    entry.app_data = reinterpret_cast<const DIPROPPOINTER*>(header)->uData;
                    return DI_OK;
                }
            }
            return DIERR_OBJECTNOTFOUND;
        }
        if (header->dwSize != sizeof(DIPROPDWORD)) return DIERR_UNSUPPORTED;
        const DWORD value = reinterpret_cast<const DIPROPDWORD*>(header)->dwData;
        if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_BUFFERSIZE)) {
            if (acquired) return DIERR_ACQUIRED;
            buffer_size = std::min(value, DWORD(4096));
            pending.clear();
            overflow = false;
        } else if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_AXISMODE)) {
            if (value != DIPROPAXISMODE_ABS) return DIERR_UNSUPPORTED;
        } else if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_FFGAIN)) {
            if (value > 10000) return DIERR_INVALIDPARAM;
            engine->gain = value;
        } else if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_AUTOCENTER)) {
            if (value != DIPROPAUTOCENTER_OFF) return DIERR_UNSUPPORTED;
        } else if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_CALIBRATIONMODE)) {
            if (value != DIPROPCALIBRATIONMODE_COOKED) return DIERR_UNSUPPORTED;
        } else if (id == reinterpret_cast<ULONG_PTR>(&DIPROP_DEADZONE) ||
                   id == reinterpret_cast<ULONG_PTR>(&DIPROP_SATURATION)) {
            if (value > 10000) return DIERR_INVALIDPARAM;
            const int object = find_object(header->dwObj, header->dwHow);
            if (header->dwHow != DIPH_DEVICE && (object < 0 || objects[object].axis < 0)) return DIERR_OBJECTNOTFOUND;
            auto& values = id == reinterpret_cast<ULONG_PTR>(&DIPROP_DEADZONE) ? deadzone : saturation;
            for (unsigned axis = 0; axis < 3; ++axis)
                if (header->dwHow == DIPH_DEVICE || objects[object].axis == static_cast<int>(axis)) values[axis] = value;
        } else return DIERR_UNSUPPORTED;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Acquire() override
    {
        std::lock_guard state_lock(state_mutex);
        if (format.empty()) return DIERR_INVALIDPARAM;
        if (acquired) return S_FALSE;
        acquired = true;
        ++engine->acquisitions;
        const auto baseline = engine->sample();
        cursor = baseline.sequence;
        collect(baseline);
        log_message("Acquire physical_connected=%d", engine->sample().connected);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Unacquire() override
    {
        std::lock_guard state_lock(state_mutex);
        if (!acquired) return S_FALSE;
        acquired = false;
        if (--engine->acquisitions == 0) engine->stop_effects();
        pending.clear();
        return DI_OK;
    }
    DWORD object_value(const ObjectDescription& object, const WheelSample& sample)
    {
        if (object.axis >= 0) {
            const unsigned axis = object.axis;
            double normalized = sample.axes[axis] / 32767.5 - 1;
            const double dead = deadzone[axis] / 10000.0;
            const double sat = saturation[axis] / 10000.0;
            const double magnitude = std::abs(normalized);
            normalized = magnitude <= dead ? 0 :
                std::copysign(std::clamp((magnitude - dead) / std::max(0.0001, sat - dead), 0.0, 1.0), normalized);
            return static_cast<DWORD>(std::lround(minimum[axis] +
                (normalized + 1) * 0.5 * (double(maximum[axis]) - minimum[axis])));
        }
        if (object.button >= 0) return sample.buttons[object.button];
        return sample.pov;
    }
    void collect(const WheelSample& sample)
    {
        for (const auto& entry : format) {
            const DWORD value = object_value(objects[entry.object], sample);
            if (have_previous && value != previous[entry.object] && buffer_size) {
                if (pending.size() >= buffer_size) {
                    pending.pop_front();
                    overflow = true;
                }
                DIDEVICEOBJECTDATA event{};
                event.dwOfs = entry.offset;
                event.dwData = value;
                event.dwTimeStamp = static_cast<DWORD>(sample.timestamp);
                event.dwSequence = ++sequence;
                event.uAppData = entry.app_data;
                pending.push_back(event);
                if (HANDLE event_handle = notification->event.load()) SetEvent(event_handle);
            }
            previous[entry.object] = value;
        }
        have_previous = true;
    }
    void collect_input(const WheelSample& latest)
    {
        const auto samples = engine->samples_after(cursor);
        if (!samples.empty() && cursor && samples.front().sequence > cursor + 1) overflow = true;
        for (const auto& sample : samples) {
            collect(sample);
            cursor = sample.sequence;
        }
        if (samples.empty() && !have_previous) collect(latest);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD bytes, LPVOID output) override
    {
        std::lock_guard state_lock(state_mutex);
        if (!output || bytes != format_size) return DIERR_INVALIDPARAM;
        if (!acquired) return DIERR_NOTACQUIRED;
        const auto sample = engine->sample();
        collect_input(sample);
        memset(output, 0, bytes);
        const DWORD neutral_hat = 0xffffffff;
        for (DWORD offset : neutral_povs)
            memcpy(static_cast<BYTE*>(output) + offset, &neutral_hat, sizeof(neutral_hat));
        for (const auto& entry : format) {
            const DWORD value = object_value(objects[entry.object], sample);
            memcpy(static_cast<BYTE*>(output) + entry.offset, &value, entry.width);
        }
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD bytes, LPDIDEVICEOBJECTDATA output, LPDWORD count, DWORD flags) override
    {
        std::lock_guard state_lock(state_mutex);
        if (!count || (bytes != sizeof(DIDEVICEOBJECTDATA) && bytes != sizeof(DIDEVICEOBJECTDATA_DX3)))
            return DIERR_INVALIDPARAM;
        if (!acquired) return DIERR_NOTACQUIRED;
        if (!buffer_size) return DIERR_NOTBUFFERED;
        collect_input(engine->sample());
        const auto amount = std::min<size_t>(*count, pending.size());
        for (size_t index = 0; output && index < amount; ++index)
            memcpy(reinterpret_cast<BYTE*>(output) + index * bytes, &pending[index], bytes);
        *count = static_cast<DWORD>(amount);
        const bool lost = overflow;
        if (!(flags & DIGDD_PEEK)) {
            pending.erase(pending.begin(), pending.begin() + amount);
            overflow = false;
        }
        return lost ? DI_BUFFEROVERFLOW : DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT input) override
    {
        std::lock_guard state_lock(state_mutex);
        if (acquired) return DIERR_ACQUIRED;
        if (!input || input->dwSize != sizeof(DIDATAFORMAT) ||
            input->dwObjSize != sizeof(DIOBJECTDATAFORMAT) || !input->rgodf || !input->dwDataSize ||
            input->dwDataSize > 65536 || input->dwNumObjs > 256 || !(input->dwFlags & DIDF_ABSAXIS))
            return DIERR_INVALIDPARAM;
        std::vector<FormatEntry> next;
        std::vector<DWORD> next_neutral_povs;
        for (DWORD index = 0; index < input->dwNumObjs; ++index) {
            const auto& requested = input->rgodf[index];
            int match = -1;
            for (unsigned object = 0; object < objects.size(); ++object) {
                const auto& candidate = objects[object];
                const bool instance = (requested.dwType & DIDFT_INSTANCEMASK) == DIDFT_ANYINSTANCE ||
                    DIDFT_GETINSTANCE(candidate.type) == DIDFT_GETINSTANCE(requested.dwType);
                if (!instance || !(candidate.type & requested.dwType & 0xff) ||
                    (requested.pguid && *requested.pguid != candidate.guid)) continue;
                if (std::any_of(next.begin(), next.end(), [object](const auto& entry) { return entry.object == object; }))
                    continue;
                match = static_cast<int>(object);
                break;
            }
            const DWORD width = (requested.dwType & DIDFT_BUTTON) ? 1 : 4;
            if (requested.dwOfs > input->dwDataSize || width > input->dwDataSize - requested.dwOfs)
                return DIERR_INVALIDPARAM;
            if (match < 0) {
                if (!(requested.dwType & 0x80000000u)) return DIERR_OBJECTNOTFOUND;
                if (requested.dwType & DIDFT_POV) next_neutral_povs.push_back(requested.dwOfs);
                continue;
            }
            next.push_back({static_cast<unsigned>(match), requested.dwOfs, width});
        }
        format = std::move(next);
        neutral_povs = std::move(next_neutral_povs);
        format_size = input->dwDataSize;
        pending.clear();
        have_previous = false;
        log_message("SetDataFormat size=%lu objects=%lu matched=%zu", format_size, input->dwNumObjs, format.size());
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE event) override {
        std::lock_guard state_lock(state_mutex); notification->event = event; return DI_OK; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD flags) override
    {
        std::lock_guard state_lock(state_mutex);
        log_message("SetCooperativeLevel hwnd=%p flags=%08lx", window, flags);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetObjectInfo(Object* output, DWORD value, DWORD how) override
    {
        std::lock_guard state_lock(state_mutex);
        if (!output || output->dwSize > sizeof(Object) || output->dwSize < offsetof(Object, dwFFMaxForce))
            return DIERR_INVALIDPARAM;
        const int index = find_object(value, how);
        if (index < 0) return DIERR_OBJECTNOTFOUND;
        auto info = describe(index);
        info.dwSize = output->dwSize;
        memcpy(output, &info, info.dwSize);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(Instance* output) override
    {
        std::lock_guard state_lock(state_mutex);
        if (!output || output->dwSize > sizeof(Instance) || output->dwSize < offsetof(Instance, guidFFDriver))
            return DIERR_INVALIDPARAM;
        auto info = wheel_info<Wide>();
        info.dwSize = output->dwSize;
        memcpy(output, &info, info.dwSize);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override {
        std::lock_guard state_lock(state_mutex); return DI_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override {
        std::lock_guard state_lock(state_mutex); return DI_OK; }
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID type, LPCDIEFFECT parameters, LPDIRECTINPUTEFFECT* output, LPUNKNOWN outer) override
    {
        std::lock_guard state_lock(state_mutex);
        if (!output) return E_POINTER;
        *output = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        if (!effect_kind(type)) {
            log_message("Unsupported effect GUID=%08lx", type.Data1);
            return DIERR_UNSUPPORTED;
        }
        auto effect = new BridgeEffect(engine, type, reinterpret_cast<uintptr_t>(this));
        if (parameters) {
            const auto result = effect->SetParameters(parameters, DIEP_ALLPARAMS);
            if (FAILED(result)) { effect->Release(); return result; }
        }
        *output = effect;
        log_message("CreateEffect GUID=%08lx kind=%lu", type.Data1, effect_kind(type));
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumEffects(typename Types::EnumEffects callback, LPVOID context, DWORD flags) override
    {
        std::lock_guard state_lock(state_mutex);
        if (!callback) return DIERR_INVALIDPARAM;
        for (const auto& type : effect_types) {
            if ((DIEFT_GETTYPE(flags)) && (DIEFT_GETTYPE(flags)) != effect_kind(type)) continue;
            EffectInfo info{};
            info.dwSize = sizeof(info);
            GetEffectInfo(&info, type);
            if (callback(&info, context) == DIENUM_STOP) break;
        }
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(EffectInfo* output, REFGUID type) override
    {
        std::lock_guard state_lock(state_mutex);
        if (!output || output->dwSize != sizeof(EffectInfo)) return DIERR_INVALIDPARAM;
        const auto kind = effect_kind(type);
        if (!kind) return DIERR_OBJECTNOTFOUND;
        output->guid = type;
        output->dwEffType = kind | DIEFT_STARTDELAY | DIEFT_FFATTACK | DIEFT_FFFADE;
        output->dwStaticParams = DIEP_ALLPARAMS;
        output->dwDynamicParams = DIEP_ALLPARAMS;
        copy_name(output->tszName, kind == DIEFT_CONSTANTFORCE ? "Constant force" :
            kind == DIEFT_PERIODIC ? "Periodic" : kind == DIEFT_RAMPFORCE ? "Ramp" : "Condition");
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD output) override
    {
        std::lock_guard state_lock(state_mutex);
        if (!output) return E_POINTER;
        *output = engine->actuators ? DIGFFS_ACTUATORSON : DIGFFS_ACTUATORSOFF;
        if (engine->paused) *output |= DIGFFS_PAUSED;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD command) override
    {
        std::lock_guard state_lock(state_mutex);
        log_message("SendForceFeedbackCommand %lu", command);
        if (command == DISFFC_RESET || command == DISFFC_STOPALL) engine->stop_effects();
        else if (command == DISFFC_SETACTUATORSOFF) engine->actuators = false;
        else if (command == DISFFC_SETACTUATORSON) engine->actuators = true;
        else if (command == DISFFC_PAUSE) {
            if (!engine->paused.exchange(true)) engine->paused_at = GetTickCount64();
        } else if (command == DISFFC_CONTINUE) {
            if (engine->paused) {
                std::lock_guard lock(engine->effect_mutex);
                const auto delay = GetTickCount64() - engine->paused_at;
                for (auto& weak : engine->effects)
                    if (auto effect = weak.lock(); effect && effect->started) effect->started += delay;
                engine->paused = false;
            }
        } else return DIERR_INVALIDPARAM;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK callback, LPVOID context, DWORD) override
    {
        std::lock_guard state_lock(state_mutex);
        if (!callback) return DIERR_INVALIDPARAM;
        std::vector<IDirectInputEffect*> snapshot;
        {
            std::lock_guard lock(engine->effect_mutex);
            for (auto& weak : engine->effects) {
                if (auto effect = weak.lock(); effect && effect->owner == reinterpret_cast<uintptr_t>(this)
                    && effect->effect_interface) {
                    effect->effect_interface->AddRef();
                    snapshot.push_back(effect->effect_interface);
                }
            }
        }
        bool stopped = false;
        for (auto effect : snapshot) {
            if (!stopped) stopped = callback(effect, context) == DIENUM_STOP;
            effect->Release();
        }
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override {
        std::lock_guard state_lock(state_mutex); return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Poll() override
    {
        std::lock_guard state_lock(state_mutex);
        if (!acquired) return DIERR_NOTACQUIRED;
        collect_input(engine->sample());
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override {
        std::lock_guard state_lock(state_mutex); return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(typename Types::String, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override {
        std::lock_guard state_lock(state_mutex); return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(typename Types::String, DWORD, LPDIFILEEFFECT, DWORD) override {
        std::lock_guard state_lock(state_mutex); return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE BuildActionMap(typename Types::Actions, typename Types::String, DWORD) override
    {
        std::lock_guard state_lock(state_mutex);
        log_message("BuildActionMap requested (unsupported)");
        return DIERR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE SetActionMap(typename Types::Actions, typename Types::String, DWORD) override
    {
        std::lock_guard state_lock(state_mutex);
        log_message("SetActionMap requested (unsupported)");
        return DIERR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE GetImageInfo(typename Types::Images) override {
        std::lock_guard state_lock(state_mutex); return DIERR_UNSUPPORTED; }
private:
    std::atomic<ULONG> references{1};
    std::shared_ptr<WheelEngine> engine;
    bool acquired = false;
    bool have_previous = false;
    bool overflow = false;
    DWORD format_size = 0;
    DWORD buffer_size = 0;
    DWORD sequence = 0;
    std::shared_ptr<InputNotification> notification;
    uint64_t cursor = 0;
    std::recursive_mutex state_mutex;
    std::array<LONG,3> minimum{0,0,0};
    std::array<LONG,3> maximum{65535,65535,65535};
    std::array<DWORD,3> deadzone{};
    std::array<DWORD,3> saturation{10000,10000,10000};
    std::array<DWORD,25> previous{};
    std::vector<FormatEntry> format;
    std::vector<DWORD> neutral_povs;
    std::deque<DIDEVICEOBJECTDATA> pending;
};

template<bool Wide>
class InputProxy final : public InputTypes<Wide>::Input {
    using Types = InputTypes<Wide>;
public:
    explicit InputProxy(typename Types::Input* original) : real(original), engine(wheel_engine()) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != Types::input_iid()) return E_NOINTERFACE;
        *output = static_cast<typename Types::Input*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto count = --references;
        if (!count) { real->Release(); delete this; }
        return count;
    }
    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, typename Types::Device** output, LPUNKNOWN outer) override
    {
        if (guid != wheel_instance && guid != wheel_product) return real->CreateDevice(guid, output, outer);
        if (!output) return E_POINTER;
        *output = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        *output = new WheelDevice<Wide>();
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD type, typename Types::EnumDevices callback, LPVOID context, DWORD flags) override
    {
        if (!callback) return DIERR_INVALIDPARAM;
        log_message("EnumDevices %s type=%lu flags=%08lx physical=%d", Wide ? "W" : "A", type, flags, engine->sample().connected);
        if (!type || type == DI8DEVCLASS_ALL || type == DI8DEVCLASS_GAMECTRL || type == DI8DEVTYPE_DRIVING) {
            auto info = wheel_info<Wide>();
            if (callback(&info, context) == DIENUM_STOP) return DI_OK;
        }
        return real->EnumDevices(type, callback, context, flags);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID guid) override
    {
        if (guid == wheel_instance || guid == wheel_product) return DI_OK;
        return real->GetDeviceStatus(guid);
    }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND window, DWORD flags) override { return real->RunControlPanel(window, flags); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version) override { return real->Initialize(instance, version); }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID guid, typename Types::String name, LPGUID output) override
    {
        if (guid == wheel_product || guid == wheel_instance) {
            if (!output) return E_POINTER;
            *output = wheel_instance;
            return DI_OK;
        }
        return real->FindDevice(guid, name, output);
    }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(typename Types::String user, typename Types::Actions actions,
        typename Types::Semantics callback, LPVOID context, DWORD flags) override
    {
        log_message("EnumDevicesBySemantics (system forwarding)");
        return real->EnumDevicesBySemantics(user, actions, callback, context, flags);
    }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK callback,
        typename Types::Configure parameters, DWORD flags, LPVOID context) override
    {
        return real->ConfigureDevices(callback, parameters, flags, context);
    }
private:
    std::atomic<ULONG> references{1};
    typename Types::Input* real;
    std::shared_ptr<WheelEngine> engine;
};

extern void start_telemetry_once();

static HMODULE system_dinput()
{
    static HMODULE library = [] {
        wchar_t path[MAX_PATH]{};
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dinput8.dll");
        return LoadLibraryW(path);
    }();
    return library;
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID* output, LPUNKNOWN outer)
{
    if (!output) return E_POINTER;
    start_telemetry_once();
    *output = nullptr;
    const auto library = system_dinput();
    if (!library) return HRESULT_FROM_WIN32(GetLastError());
    using Create = HRESULT(WINAPI*)(HINSTANCE,DWORD,REFIID,LPVOID*,LPUNKNOWN);
    const auto create = reinterpret_cast<Create>(GetProcAddress(library, "DirectInput8Create"));
    if (!create) return E_FAIL;
    log_message("DirectInput8Create version=%08lx iid=%08lx", version, iid.Data1);
    const auto result = create(instance, version, iid, output, outer);
    if (FAILED(result) || outer) return result;
    if (iid == IID_IDirectInput8A)
        *output = new InputProxy<false>(static_cast<IDirectInput8A*>(*output));
    else if (iid == IID_IDirectInput8W)
        *output = new InputProxy<true>(static_cast<IDirectInput8W*>(*output));
    return result;
}
extern "C" HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID, REFIID, LPVOID* output)
{
    if (output) *output = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}
extern "C" HRESULT WINAPI DllRegisterServer() { return E_NOTIMPL; }
extern "C" HRESULT WINAPI DllUnregisterServer() { return E_NOTIMPL; }
