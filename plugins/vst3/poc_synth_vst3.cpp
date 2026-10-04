// PoC Synth (VST3): the same tiny saw synth as the VST2 test plugin, as a
// 32-bit Windows .vst3 module. It is written against Steinberg's MIT-licensed
// pluginterfaces headers only (no SDK helper classes), so the test plugin and
// the host share nothing but the VST3 ABI. One object implements IComponent,
// IAudioProcessor and IEditController (a "single component" plugin).

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <atomic>
#include <cmath>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// {6F1C2A4B-2D8E-4B7A-9C11-50434F533301}
const TUID kSynthCid = INLINE_UID(0x6F1C2A4B, 0x2D8E4B7A, 0x9C115043, 0x4F533301);

constexpr int kVoices = 8;
constexpr int kNumParams = 3;
const char *const kParamNames[kNumParams] = {"Gain", "Cutoff", "Release"};
const double kParamDefaults[kNumParams] = {0.8, 0.6, 0.2};

bool sameIid(const TUID a, const TUID b) { return std::memcmp(a, b, sizeof(TUID)) == 0; }

void toString128(String128 out, const char *text) {
    int i = 0;
    for (; text[i] && i < 127; ++i) out[i] = static_cast<char16>(text[i]);
    out[i] = 0;
}

struct Voice {
    int note = -1;
    bool held = false;
    float phase = 0, inc = 0, level = 0;
};

class PocSynth final : public IComponent, public IAudioProcessor, public IEditController {
  public:
    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        if (sameIid(iid, FUnknown_iid) || sameIid(iid, IPluginBase_iid) || sameIid(iid, IComponent_iid)) {
            *obj = static_cast<IComponent *>(this);
        } else if (sameIid(iid, IAudioProcessor_iid)) {
            *obj = static_cast<IAudioProcessor *>(this);
        } else if (sameIid(iid, IEditController_iid)) {
            *obj = static_cast<IEditController *>(this);
        } else {
            *obj = nullptr;
            return kNoInterface;
        }
        addRef();
        return kResultOk;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        const uint32 left = --refs_;
        if (left == 0) delete this;
        return left;
    }

    // IPluginBase (shared by IComponent and IEditController)
    tresult PLUGIN_API initialize(FUnknown *) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    // IComponent
    tresult PLUGIN_API getControllerClassId(TUID) override { return kResultFalse; }
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override {
        if (type == kAudio && dir == kOutput) return 1;
        if (type == kEvent && dir == kInput) return 1;
        return 0;
    }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo &bus) override {
        if (index != 0 || getBusCount(type, dir) == 0) return kInvalidArgument;
        bus.mediaType = type;
        bus.direction = dir;
        bus.channelCount = type == kAudio ? 2 : 16;
        toString128(bus.name, type == kAudio ? "Output" : "MIDI In");
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo &, RoutingInfo &) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool state) override {
        if (state) reset();
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream *) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream *) override { return kResultOk; }

    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement *, int32 numIns, SpeakerArrangement *outs,
                                          int32 numOuts) override {
        return numIns == 0 && numOuts == 1 && outs[0] == SpeakerArr::kStereo ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement &arr) override {
        if (dir != kOutput || index != 0) return kInvalidArgument;
        arr = SpeakerArr::kStereo;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override {
        return size == kSample32 ? kResultTrue : kResultFalse;
    }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override {
        sampleRate_ = static_cast<float>(setup.sampleRate);
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    uint32 PLUGIN_API getTailSamples() override { return kNoTail; }

    tresult PLUGIN_API process(ProcessData &data) override {
        applyParameterChanges(data.inputParameterChanges);
        if (data.numOutputs < 1 || data.outputs[0].numChannels < 2) return kResultOk;
        float *left = data.outputs[0].channelBuffers32[0];
        float *right = data.outputs[0].channelBuffers32[1];
        IEventList *events = data.inputEvents;
        const int32 eventCount = events ? events->getEventCount() : 0;
        int32 nextEvent = 0;
        Event event{};

        const float gain = static_cast<float>(params_[0]);
        const float cutoffHz = 80.0f * std::pow(250.0f, static_cast<float>(params_[1]));
        const float a = 1.0f - std::exp(-6.2831853f * cutoffHz / sampleRate_);
        const float releaseSec = 0.01f + 2.0f * static_cast<float>(params_[2]);
        const float decay = std::exp(-1.0f / (releaseSec * sampleRate_));

        for (int32 f = 0; f < data.numSamples; ++f) {
            while (nextEvent < eventCount && events->getEvent(nextEvent, event) == kResultOk &&
                   event.sampleOffset <= f) {
                handle(event);
                ++nextEvent;
            }
            float mix = 0;
            for (Voice &v : voices_) {
                if (v.note < 0) continue;
                mix += (2.0f * v.phase - 1.0f) * v.level;
                v.phase += v.inc;
                if (v.phase >= 1.0f) v.phase -= 1.0f;
                if (!v.held) {
                    v.level *= decay;
                    if (v.level < 1e-4f) v.note = -1;
                }
            }
            lp_[0] += a * (mix - lp_[0]);
            lp_[1] += a * (lp_[0] - lp_[1]);
            left[f] = right[f] = lp_[1] * gain * 0.25f;
        }
        while (nextEvent < eventCount && events->getEvent(nextEvent++, event) == kResultOk) handle(event);
        data.outputs[0].silenceFlags = 0;
        return kResultOk;
    }

    // IEditController
    tresult PLUGIN_API setComponentState(IBStream *) override { return kResultOk; }
    int32 PLUGIN_API getParameterCount() override { return kNumParams; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo &info) override {
        if (index < 0 || index >= kNumParams) return kInvalidArgument;
        std::memset(&info, 0, sizeof(info));
        info.id = static_cast<ParamID>(index);
        toString128(info.title, kParamNames[index]);
        toString128(info.shortTitle, kParamNames[index]);
        toString128(info.units, "%");
        info.defaultNormalizedValue = kParamDefaults[index];
        info.flags = ParameterInfo::kCanAutomate;
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue value, String128 out) override {
        if (id >= kNumParams) return kInvalidArgument;
        char text[16];
        const int pct = static_cast<int>(value * 100.0 + 0.5);
        int n = 0;
        if (pct >= 100) text[n++] = '1';
        if (pct >= 10) text[n++] = static_cast<char>('0' + (pct / 10) % 10);
        text[n++] = static_cast<char>('0' + pct % 10);
        text[n] = 0;
        toString128(out, text);
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar *, ParamValue &) override { return kResultFalse; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override { return id < kNumParams ? params_[id] : 0; }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override {
        if (id >= kNumParams) return kInvalidArgument;
        params_[id] = v;
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler(IComponentHandler *) override { return kResultOk; }
    IPlugView *PLUGIN_API createView(FIDString) override { return nullptr; }

  private:
    void reset() {
        for (Voice &v : voices_) v = Voice{};
        lp_[0] = lp_[1] = 0;
    }
    void handle(const Event &event) {
        if (event.type == Event::kNoteOnEvent && event.noteOn.velocity > 0) {
            Voice *v = &voices_[0];
            for (Voice &candidate : voices_) {
                if (candidate.note < 0) { v = &candidate; break; }
                if (candidate.level < v->level) v = &candidate;
            }
            v->note = event.noteOn.pitch;
            v->held = true;
            v->level = event.noteOn.velocity;
            v->inc = 440.0f * std::pow(2.0f, (event.noteOn.pitch - 69) / 12.0f) / sampleRate_;
        } else if (event.type == Event::kNoteOffEvent ||
                   (event.type == Event::kNoteOnEvent && event.noteOn.velocity <= 0)) {
            const int16 pitch = event.type == Event::kNoteOffEvent ? event.noteOff.pitch : event.noteOn.pitch;
            for (Voice &v : voices_)
                if (v.note == pitch) v.held = false;
        }
    }
    void applyParameterChanges(IParameterChanges *changes) {
        if (!changes) return;
        for (int32 i = 0; i < changes->getParameterCount(); ++i) {
            IParamValueQueue *queue = changes->getParameterData(i);
            if (!queue || queue->getPointCount() <= 0) continue;
            int32 offset = 0;
            ParamValue value = 0;
            if (queue->getPoint(queue->getPointCount() - 1, offset, value) == kResultOk &&
                queue->getParameterId() < kNumParams)
                params_[queue->getParameterId()] = value;
        }
    }

    std::atomic<uint32> refs_{1};
    double params_[kNumParams] = {kParamDefaults[0], kParamDefaults[1], kParamDefaults[2]};
    float sampleRate_ = 44100.0f;
    Voice voices_[kVoices];
    float lp_[2] = {0, 0};
};

class Factory final : public IPluginFactory {
  public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        if (sameIid(iid, FUnknown_iid) || sameIid(iid, IPluginFactory_iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API getFactoryInfo(PFactoryInfo *info) override {
        *info = {};
        std::strcpy(info->vendor, "prometheos");
        std::strcpy(info->url, "https://github.com/dahlgrenmartin/prometheos-webclap-obxd");
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 1; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo *info) override {
        if (index != 0) return kInvalidArgument;
        *info = {};
        std::memcpy(info->cid, kSynthCid, sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        std::strcpy(info->category, kVstAudioEffectClass);
        std::strcpy(info->name, "PoC Synth VST3");
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void **obj) override {
        *obj = nullptr;
        if (!sameIid(reinterpret_cast<const char *>(cid), kSynthCid)) return kInvalidArgument;
        auto *synth = new PocSynth();
        const tresult result = synth->queryInterface(reinterpret_cast<const char *>(iid), obj);
        synth->release();
        return result;
    }
};

Factory gFactory;

} // namespace

extern "C" {
__declspec(dllexport) IPluginFactory *PLUGIN_API GetPluginFactory() { return &gFactory; }
__declspec(dllexport) bool InitDll() { return true; }
__declspec(dllexport) bool ExitDll() { return true; }
}
