// vsthost: a minimal Windows VST2/VST3 host meant to run under Wine inside
// Boxedwine (and therefore inside a browser tab).
//
// It plays the part of yabridge's Wine-side plugin host: it loads an
// unmodified Windows plugin binary, drives it through the plugin's own ABI and
// hands the audio back to the outside world. In this proof of concept the
// hand-off is a WAV file plus a JSON report in the emulated filesystem (read by
// the web page from Emscripten's FS), and optionally the Windows audio device
// (waveOut), which Boxedwine plays through the browser's AudioWorklet.
//
//   vsthost <plugin.dll|plugin.vst3> [--out render.wav] [--report report.json]
//           [--notes 60,64,67] [--seconds 3] [--rate 44100] [--block 512]
//           [--param index=value] [--play]
//   vsthost --serve <job directory>      (persistent host; see serve())
//
// Built as a 32-bit Windows console program with MinGW: Boxedwine emulates
// 32-bit x86, so the plugins it can host are 32-bit Windows binaries.

#include "vst2_abi.h"

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string plugin;
    std::string out = "render.wav";
    std::string report = "report.json";
    std::vector<int> notes{60, 64, 67};
    double seconds = 3.0;
    double rate = 44100.0;
    int block = 512;
    std::vector<std::pair<int, double>> params;
    bool play = false;
};

struct ParamReport {
    std::string name;
    std::string display;
    double value;
};

struct Report {
    double processStartMs = 0; // when the audio loop started (after plugin init)
    double processEndMs = 0;
    std::string format;
    std::string name;
    std::string vendor;
    bool synth = false;
    int inputs = 0;
    int outputs = 0;
    std::vector<ParamReport> params;
    std::string error;
};

// ---- small utilities -----------------------------------------------------------

// Stage trace, appended to <report>.log and flushed per line, so a plugin that
// hangs or crashes the process still shows where it stopped.
std::string gTracePath;
void trace(const char *stage) {
    std::fprintf(stderr, "[vsthost] %s\n", stage);
    if (gTracePath.empty()) return;
    if (FILE *f = std::fopen(gTracePath.c_str(), "ab")) {
        std::fprintf(f, "%lu %s\n", static_cast<unsigned long>(GetTickCount()), stage);
        std::fclose(f);
    }
}

std::string jsonEscape(const std::string &s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += static_cast<char>(c);
    }
    return out;
}

std::string narrow(const Steinberg::Vst::TChar *text) {
    std::string out;
    for (; text && *text; ++text) out += *text < 0x80 ? static_cast<char>(*text) : '?';
    return out;
}

double nowMs() {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return 1000.0 * static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
}

bool writeWav(const std::string &path, const std::vector<float> &interleaved, int rate) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t dataBytes = static_cast<uint32_t>(interleaved.size() * 2);
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(2); u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate) * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
    for (float s : interleaved) {
        const float c = std::max(-1.0f, std::min(1.0f, s));
        const int16_t v = static_cast<int16_t>(std::lround(c * 32767.0f));
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

// Plays the render through the Windows audio device (Boxedwine: browser audio).
void playWav(const std::vector<float> &interleaved, int rate) {
    std::vector<int16_t> pcm(interleaved.size());
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<int16_t>(std::lround(std::max(-1.0f, std::min(1.0f, interleaved[i])) * 32767.0f));
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = static_cast<DWORD>(rate);
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * 4;
    HWAVEOUT out = nullptr;
    if (waveOutOpen(&out, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        std::printf("waveOut: no audio device\n");
        return;
    }
    WAVEHDR hdr{};
    hdr.lpData = reinterpret_cast<LPSTR>(pcm.data());
    hdr.dwBufferLength = static_cast<DWORD>(pcm.size() * 2);
    waveOutPrepareHeader(out, &hdr, sizeof hdr);
    waveOutWrite(out, &hdr, sizeof hdr);
    while (!(hdr.dwFlags & WHDR_DONE)) Sleep(20);
    waveOutUnprepareHeader(out, &hdr, sizeof hdr);
    waveOutClose(out);
    std::printf("waveOut: played %u frames\n", static_cast<unsigned>(pcm.size() / 2));
}

// A test signal for effects: a decaying saw chord, so an effect has something to process.
float testInput(size_t frame, double rate) {
    const double t = static_cast<double>(frame) / rate;
    const double env = std::exp(-1.5 * std::fmod(t, 1.0));
    double s = 0;
    for (double hz : {110.0, 165.0, 220.0}) s += 2.0 * std::fmod(t * hz, 1.0) - 1.0;
    return static_cast<float>(0.2 * env * s);
}

// Creates the directories leading to `path` (both slash kinds accepted).
void makeParentDirs(const std::string &path) {
    for (size_t i = 3; i < path.size(); ++i)
        if (path[i] == '\\' || path[i] == '/') CreateDirectoryA(path.substr(0, i).c_str(), nullptr);
}

// ---- VST2 ------------------------------------------------------------------------

VstTimeInfo gTime{};
double gRate = 44100.0;
int gBlock = 512;

intptr_t VST2_CALL vst2HostCallback(AEffect *, int32_t opcode, int32_t, intptr_t, void *ptr, float) {
    switch (opcode) {
    case audioMasterVersion: return 2400;
    case audioMasterCurrentId: return 0;
    case audioMasterGetTime: return reinterpret_cast<intptr_t>(&gTime);
    case audioMasterGetSampleRate: return static_cast<intptr_t>(gRate);
    case audioMasterGetBlockSize: return gBlock;
    case audioMasterGetCurrentProcessLevel: return 2; // realtime thread
    case audioMasterGetVendorString: std::strcpy(static_cast<char *>(ptr), "prometheos"); return 1;
    case audioMasterGetProductString: std::strcpy(static_cast<char *>(ptr), "vsthost (Boxedwine PoC)"); return 1;
    case audioMasterGetVendorVersion: return 1;
    case audioMasterCanDo: {
        const char *what = static_cast<const char *>(ptr);
        return what && (!std::strcmp(what, "sendVstEvents") || !std::strcmp(what, "sendVstMidiEvent") ||
                        !std::strcmp(what, "sendVstTimeInfo"))
                   ? 1
                   : 0;
    }
    default: return 0;
    }
}

bool renderVst2(HMODULE dll, const Options &o, Report &r, std::vector<float> &audio) {
    r.format = "vst2";
    auto entry = reinterpret_cast<Vst2PluginMain>(GetProcAddress(dll, "VSTPluginMain"));
    if (!entry) entry = reinterpret_cast<Vst2PluginMain>(GetProcAddress(dll, "main"));
    if (!entry) { r.error = "no VSTPluginMain/main export"; return false; }
    trace("VSTPluginMain");
    AEffect *e = entry(vst2HostCallback);
    if (!e || e->magic != VST2_MAGIC) { r.error = "VSTPluginMain returned no AEffect"; return false; }
    auto call = [&](int32_t op, int32_t index = 0, intptr_t value = 0, void *ptr = nullptr, float opt = 0) {
        return e->dispatcher(e, op, index, value, ptr, opt);
    };

    trace("effOpen");
    call(effOpen);
    trace("opened");
    char text[256] = {};
    call(effGetEffectName, 0, 0, text);
    if (!text[0]) call(effGetProductString, 0, 0, text);
    r.name = text;
    text[0] = 0;
    call(effGetVendorString, 0, 0, text);
    r.vendor = text;
    r.synth = (e->flags & effFlagsIsSynth) != 0;
    r.inputs = e->numInputs;
    r.outputs = e->numOutputs;
    for (auto &[index, value] : o.params)
        if (index >= 0 && index < e->numParams) e->setParameter(e, index, static_cast<float>(value));

    call(effSetSampleRate, 0, 0, nullptr, static_cast<float>(o.rate));
    call(effSetBlockSize, 0, o.block);
    call(effMainsChanged, 0, 1);
    call(effStartProcess);
    trace("processing");
    r.processStartMs = nowMs();

    const size_t total = static_cast<size_t>(o.seconds * o.rate);
    const size_t noteOff = static_cast<size_t>(total * 0.6);
    const int ins = std::max(0, e->numInputs), outs = std::max(2, e->numOutputs);
    std::vector<std::vector<float>> inBuf(ins, std::vector<float>(o.block)), outBuf(outs, std::vector<float>(o.block));
    std::vector<float *> inPtr(ins), outPtr(outs);
    for (int i = 0; i < ins; ++i) inPtr[i] = inBuf[i].data();
    for (int i = 0; i < outs; ++i) outPtr[i] = outBuf[i].data();
    audio.assign(total * 2, 0.0f);

    gTime.sampleRate = o.rate;
    gTime.tempo = 120.0;
    gTime.timeSigNumerator = gTime.timeSigDenominator = 4;
    gTime.flags = kVstTransportPlaying | kVstTempoValid | kVstPpqPosValid;

    for (size_t pos = 0; pos < total; pos += o.block) {
        const int frames = static_cast<int>(std::min<size_t>(o.block, total - pos));
        // MIDI for this block: note-ons at frame 0, note-offs at 60% of the render.
        std::vector<VstMidiEvent> midi;
        for (int note : o.notes) {
            if (pos == 0) midi.push_back({kVstMidiType, sizeof(VstMidiEvent), 0, 0, 0, 0,
                                          {char(0x90), char(note), char(100), 0}, 0, 0, 0, 0});
            if (noteOff >= pos && noteOff < pos + frames)
                midi.push_back({kVstMidiType, sizeof(VstMidiEvent), int32_t(noteOff - pos), 0, 0, 0,
                                {char(0x80), char(note), char(0), 0}, 0, 0, 0, 0});
        }
        if (!midi.empty()) {
            std::vector<char> storage(sizeof(VstEvents) + midi.size() * sizeof(VstEvent *));
            auto *events = reinterpret_cast<VstEvents *>(storage.data());
            events->numEvents = static_cast<int32_t>(midi.size());
            for (size_t i = 0; i < midi.size(); ++i) events->events[i] = reinterpret_cast<VstEvent *>(&midi[i]);
            call(effProcessEvents, 0, 0, events);
        }
        for (int c = 0; c < ins; ++c)
            for (int f = 0; f < frames; ++f) inBuf[c][f] = testInput(pos + f, o.rate);
        for (auto &b : outBuf) std::fill(b.begin(), b.end(), 0.0f);
        gTime.samplePos = static_cast<double>(pos);
        gTime.ppqPos = gTime.samplePos / o.rate * gTime.tempo / 60.0;
        if (e->flags & effFlagsCanReplacing) e->processReplacing(e, inPtr.data(), outPtr.data(), frames);
        else e->process(e, inPtr.data(), outPtr.data(), frames);
        for (int f = 0; f < frames; ++f) {
            audio[(pos + f) * 2] = outBuf[0][f];
            audio[(pos + f) * 2 + 1] = outBuf[outs > 1 ? 1 : 0][f];
        }
    }

    for (int i = 0; i < e->numParams; ++i) {
        ParamReport p;
        char name[256] = {}, display[256] = {}, label[256] = {};
        call(effGetParamName, i, 0, name);
        call(effGetParamDisplay, i, 0, display);
        call(effGetParamLabel, i, 0, label);
        p.name = name;
        p.display = std::string(display) + (label[0] ? std::string(" ") + label : "");
        p.value = e->getParameter(e, i);
        r.params.push_back(p);
    }
    r.processEndMs = nowMs();
    trace("rendered");
    call(effStopProcess);
    call(effMainsChanged, 0, 0);
    call(effClose);
    trace("closed");
    return true;
}

// ---- VST3 ------------------------------------------------------------------------

using namespace Steinberg;
using namespace Steinberg::Vst;

bool sameIid(const TUID a, const TUID b) { return std::memcmp(a, b, sizeof(TUID)) == 0; }

class HostApplication final : public IHostApplication {
  public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        if (sameIid(iid, FUnknown_iid) || sameIid(iid, IHostApplication_iid)) { *obj = this; return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API getName(String128 name) override {
        const char *n = "vsthost (Boxedwine PoC)";
        int i = 0;
        for (; n[i]; ++i) name[i] = static_cast<char16>(n[i]);
        name[i] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID, TUID, void **obj) override {
        *obj = nullptr;
        return kNotImplemented;
    }
};

class EventList final : public IEventList {
  public:
    std::vector<Event> events;
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        if (sameIid(iid, FUnknown_iid) || sameIid(iid, IEventList_iid)) { *obj = this; return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getEventCount() override { return static_cast<int32>(events.size()); }
    tresult PLUGIN_API getEvent(int32 index, Event &e) override {
        if (index < 0 || index >= getEventCount()) return kInvalidArgument;
        e = events[index];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Event &e) override {
        events.push_back(e);
        return kResultOk;
    }
};

HostApplication gHostApp;

template <typename T> T *query(FUnknown *unknown, const TUID iid) {
    void *obj = nullptr;
    return unknown && unknown->queryInterface(iid, &obj) == kResultOk ? static_cast<T *>(obj) : nullptr;
}

bool renderVst3(HMODULE dll, const Options &o, Report &r, std::vector<float> &audio) {
    r.format = "vst3";
    using InitFn = bool (*)();
    if (auto init = reinterpret_cast<InitFn>(GetProcAddress(dll, "InitDll"))) init();
    using FactoryFn = IPluginFactory *(PLUGIN_API *)();
    auto getFactory = reinterpret_cast<FactoryFn>(GetProcAddress(dll, "GetPluginFactory"));
    if (!getFactory) { r.error = "no GetPluginFactory export"; return false; }
    IPluginFactory *factory = getFactory();
    if (!factory) { r.error = "GetPluginFactory returned null"; return false; }
    PFactoryInfo finfo{};
    factory->getFactoryInfo(&finfo);
    r.vendor = finfo.vendor;

    PClassInfo info{};
    bool found = false;
    for (int32 i = 0; i < factory->countClasses(); ++i) {
        if (factory->getClassInfo(i, &info) == kResultOk && !std::strcmp(info.category, kVstAudioEffectClass)) {
            found = true;
            break;
        }
    }
    if (!found) { r.error = "no Audio Module Class in the factory"; return false; }
    r.name = info.name;

    void *obj = nullptr;
    if (factory->createInstance(info.cid, IComponent_iid, &obj) != kResultOk || !obj) {
        r.error = "createInstance(IComponent) failed";
        return false;
    }
    auto *component = static_cast<IComponent *>(obj);
    if (component->initialize(&gHostApp) != kResultOk) { r.error = "IComponent::initialize failed"; return false; }
    auto *processor = query<IAudioProcessor>(component, IAudioProcessor_iid);
    if (!processor) { r.error = "no IAudioProcessor"; return false; }

    // Controller: the component itself (single component) or a separate class.
    IEditController *controller = query<IEditController>(component, IEditController_iid);
    bool separateController = false;
    TUID controllerCid;
    if (!controller && component->getControllerClassId(controllerCid) == kResultOk &&
        factory->createInstance(controllerCid, IEditController_iid, &obj) == kResultOk && obj) {
        controller = static_cast<IEditController *>(obj);
        controller->initialize(&gHostApp);
        separateController = true;
    }

    const int32 audioIns = component->getBusCount(kAudio, kInput);
    const int32 audioOuts = component->getBusCount(kAudio, kOutput);
    r.inputs = audioIns > 0 ? 2 : 0;
    r.outputs = audioOuts > 0 ? 2 : 0;
    r.synth = component->getBusCount(kEvent, kInput) > 0 && audioIns == 0;
    if (audioOuts > 0) component->activateBus(kAudio, kOutput, 0, true);
    if (audioIns > 0) component->activateBus(kAudio, kInput, 0, true);
    if (component->getBusCount(kEvent, kInput) > 0) component->activateBus(kEvent, kInput, 0, true);
    std::vector<SpeakerArrangement> inArr(audioIns, SpeakerArr::kStereo), outArr(audioOuts, SpeakerArr::kStereo);
    processor->setBusArrangements(inArr.data(), audioIns, outArr.data(), audioOuts);

    ProcessSetup setup{kRealtime, kSample32, o.block, o.rate};
    if (processor->setupProcessing(setup) != kResultOk) { r.error = "setupProcessing failed"; return false; }
    if (controller) {
        for (auto &[index, value] : o.params) {
            ParameterInfo pi{};
            if (controller->getParameterInfo(index, pi) == kResultOk) controller->setParamNormalized(pi.id, value);
        }
    }
    component->setActive(true);
    processor->setProcessing(true);
    r.processStartMs = nowMs();

    const size_t total = static_cast<size_t>(o.seconds * o.rate);
    const size_t noteOff = static_cast<size_t>(total * 0.6);
    std::vector<float> inL(o.block), inR(o.block), outL(o.block), outR(o.block);
    float *inPtrs[2] = {inL.data(), inR.data()};
    float *outPtrs[2] = {outL.data(), outR.data()};
    std::vector<AudioBusBuffers> inBus(audioIns), outBus(audioOuts);
    for (auto &b : inBus) { b.numChannels = 2; b.channelBuffers32 = inPtrs; }
    for (auto &b : outBus) { b.numChannels = 2; b.channelBuffers32 = outPtrs; }
    std::vector<std::vector<float>> scratch;
    // Extra output busses get their own scratch buffers.
    for (size_t i = 1; i < outBus.size(); ++i) {
        scratch.emplace_back(o.block);
        scratch.emplace_back(o.block);
    }
    std::vector<float *> scratchPtrs;
    for (auto &s : scratch) scratchPtrs.push_back(s.data());
    for (size_t i = 1; i < outBus.size(); ++i) outBus[i].channelBuffers32 = &scratchPtrs[(i - 1) * 2];

    ProcessContext ctx{};
    ctx.sampleRate = o.rate;
    ctx.tempo = 120.0;
    ctx.timeSigNumerator = ctx.timeSigDenominator = 4;
    ctx.state = ProcessContext::kPlaying | ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid;
    EventList events;
    audio.assign(total * 2, 0.0f);

    for (size_t pos = 0; pos < total; pos += o.block) {
        const int32 frames = static_cast<int32>(std::min<size_t>(o.block, total - pos));
        events.events.clear();
        for (int note : o.notes) {
            Event e{};
            if (pos == 0) {
                e.type = Event::kNoteOnEvent;
                e.noteOn.pitch = static_cast<int16>(note);
                e.noteOn.velocity = 100.0f / 127.0f;
                e.noteOn.noteId = note;
                events.events.push_back(e);
            }
            if (noteOff >= pos && noteOff < pos + frames) {
                e = Event{};
                e.type = Event::kNoteOffEvent;
                e.sampleOffset = static_cast<int32>(noteOff - pos);
                e.noteOff.pitch = static_cast<int16>(note);
                e.noteOff.noteId = note;
                events.events.push_back(e);
            }
        }
        for (int32 f = 0; f < frames; ++f) inL[f] = inR[f] = testInput(pos + f, o.rate);
        std::fill(outL.begin(), outL.end(), 0.0f);
        std::fill(outR.begin(), outR.end(), 0.0f);
        ctx.projectTimeSamples = static_cast<TSamples>(pos);
        ctx.projectTimeMusic = static_cast<double>(pos) / o.rate * 2.0;

        ProcessData data;
        data.processMode = kRealtime;
        data.symbolicSampleSize = kSample32;
        data.numSamples = frames;
        data.numInputs = audioIns;
        data.numOutputs = audioOuts;
        data.inputs = inBus.empty() ? nullptr : inBus.data();
        data.outputs = outBus.empty() ? nullptr : outBus.data();
        data.inputEvents = &events;
        data.processContext = &ctx;
        processor->process(data);
        for (int32 f = 0; f < frames; ++f) {
            audio[(pos + f) * 2] = outL[f];
            audio[(pos + f) * 2 + 1] = outR[f];
        }
    }

    if (controller) {
        for (int32 i = 0; i < controller->getParameterCount(); ++i) {
            ParameterInfo pi{};
            if (controller->getParameterInfo(i, pi) != kResultOk) continue;
            ParamReport p;
            p.name = narrow(pi.title);
            p.value = controller->getParamNormalized(pi.id);
            String128 text{};
            if (controller->getParamStringByValue(pi.id, p.value, text) == kResultOk) p.display = narrow(text);
            const std::string units = narrow(pi.units);
            if (!units.empty()) p.display += " " + units;
            r.params.push_back(p);
        }
    }

    r.processEndMs = nowMs();
    processor->setProcessing(false);
    component->setActive(false);
    processor->release();
    if (controller) {
        if (separateController) controller->terminate();
        controller->release();
    }
    component->terminate();
    component->release();
    using ExitFn = bool (*)();
    if (auto exitDll = reinterpret_cast<ExitFn>(GetProcAddress(dll, "ExitDll"))) exitDll();
    return true;
}

// ---- main --------------------------------------------------------------------------

bool endsWithI(const std::string &s, const char *suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suffix[i]) return false;
    return true;
}

bool parseArgs(int argc, char **argv, Options &o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--out") o.out = next();
        else if (a == "--report") o.report = next();
        else if (a == "--seconds") o.seconds = std::atof(next());
        else if (a == "--rate") o.rate = std::atof(next());
        else if (a == "--block") o.block = std::atoi(next());
        else if (a == "--play") o.play = true;
        else if (a == "--notes") {
            o.notes.clear();
            std::string list = next();
            for (size_t p = 0; p < list.size();) {
                o.notes.push_back(std::atoi(list.c_str() + p));
                const size_t comma = list.find(',', p);
                if (comma == std::string::npos) break;
                p = comma + 1;
            }
        } else if (a == "--param") {
            const std::string kv = next();
            const size_t eq = kv.find('=');
            if (eq != std::string::npos) o.params.push_back({std::atoi(kv.c_str()), std::atof(kv.c_str() + eq + 1)});
        } else if (o.plugin.empty()) o.plugin = a;
        else return false;
    }
    o.seconds = std::max(0.1, std::min(o.seconds, 60.0));
    o.block = std::max(16, std::min(o.block, 8192));
    return !o.plugin.empty();
}

// Renders one job; writes the WAV, the JSON report and its trace log.
int runJob(const Options &o) {
    gRate = o.rate;
    gBlock = o.block;
    gTracePath = o.report + ".log";
    std::remove(gTracePath.c_str());

    Report r;
    std::vector<float> audio;
    const double t0 = nowMs();
    trace("LoadLibrary");
    HMODULE dll = LoadLibraryA(o.plugin.c_str());
    const double loadMs = nowMs() - t0;
    bool ok = false;
    double renderMs = 0, initMs = 0, processMs = 0;
    if (!dll) {
        r.error = "LoadLibrary failed, error " + std::to_string(GetLastError());
    } else {
        const double t1 = nowMs();
        ok = endsWithI(o.plugin, ".vst3") ? renderVst3(dll, o, r, audio) : renderVst2(dll, o, r, audio);
        renderMs = nowMs() - t1;
        if (r.processEndMs > r.processStartMs) {
            initMs = r.processStartMs - t1;
            processMs = r.processEndMs - r.processStartMs;
        }
    }

    double peak = 0, sum = 0;
    unsigned nonFinite = 0;
    for (float &s : audio) {
        if (!std::isfinite(s)) {
            ++nonFinite;
            s = 0.0f;
            continue;
        }
        peak = std::max(peak, static_cast<double>(std::fabs(s)));
        sum += static_cast<double>(s) * s;
    }
    const double rms = audio.empty() ? 0 : std::sqrt(sum / audio.size());
    makeParentDirs(o.out);
    makeParentDirs(o.report);
    if (ok && !writeWav(o.out, audio, static_cast<int>(o.rate))) {
        ok = false;
        r.error = "could not write " + o.out;
    }

    std::string json = "{\n";
    json += "  \"ok\": " + std::string(ok ? "true" : "false") + ",\n";
    json += "  \"plugin\": \"" + jsonEscape(o.plugin) + "\",\n";
    json += "  \"format\": \"" + r.format + "\",\n";
    json += "  \"name\": \"" + jsonEscape(r.name) + "\",\n";
    json += "  \"vendor\": \"" + jsonEscape(r.vendor) + "\",\n";
    json += "  \"synth\": " + std::string(r.synth ? "true" : "false") + ",\n";
    json += "  \"inputs\": " + std::to_string(r.inputs) + ",\n";
    json += "  \"outputs\": " + std::to_string(r.outputs) + ",\n";
    // realtimeFactor is audio length / audio-processing time: plugin start-up
    // (initMs) is a one-off cost reported separately.
    char nums[512];
    std::snprintf(nums, sizeof nums,
                  "  \"sampleRate\": %.0f,\n  \"frames\": %u,\n  \"peak\": %.6f,\n  \"rms\": %.6f,\n"
                  "  \"nonFinite\": %u,\n  \"loadMs\": %.1f,\n  \"renderMs\": %.1f,\n  \"initMs\": %.1f,\n"
                  "  \"processMs\": %.1f,\n  \"realtimeFactor\": %.3f,\n",
                  o.rate, static_cast<unsigned>(audio.size() / 2), peak, rms, nonFinite, loadMs, renderMs, initMs,
                  processMs, processMs > 0 ? (o.seconds * 1000.0) / processMs : 0.0);
    json += nums;
    json += "  \"wav\": \"" + jsonEscape(o.out) + "\",\n";
    json += "  \"error\": \"" + jsonEscape(r.error) + "\",\n";
    json += "  \"params\": [";
    for (size_t i = 0; i < r.params.size(); ++i) {
        char v[32];
        std::snprintf(v, sizeof v, "%.4f", r.params[i].value);
        json += std::string(i ? ",\n" : "\n") + "    {\"name\": \"" + jsonEscape(r.params[i].name) +
                "\", \"display\": \"" + jsonEscape(r.params[i].display) + "\", \"value\": " + v + "}";
    }
    json += r.params.empty() ? "]\n}\n" : "\n  ]\n}\n";

    if (FILE *f = std::fopen(o.report.c_str(), "wb")) {
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
    }
    std::fputs(json.c_str(), stdout);
    std::fflush(stdout);
    if (ok && o.play) playWav(audio, static_cast<int>(o.rate));
    if (dll) FreeLibrary(dll);
    return ok ? 0 : 1;
}

// Persistent host mode, the counterpart of yabridge's long-lived Wine-side host:
// Wine boots once, then jobs arrive through a mailbox file.
//
// The mailbox is <dir>\\inbox.txt, created here. A job is one command-line
// argument per line followed by a line "end"; the host takes it and empties the
// file, so a writer waits for an empty inbox before posting the next job. The
// file has to be created by the guest: Boxedwine caches directory listings, so
// files a browser page creates directly in Emscripten's FS are invisible to a
// directory scan, but a guest-created file's contents are read afresh on open.
std::string readWhole(const std::string &path) {
    std::string text;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        char buf[1024];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    return text;
}

void writeWhole(const std::string &path, const std::string &text) {
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

int serve(const std::string &dir) {
    CreateDirectoryA(dir.c_str(), nullptr);
    const std::string inbox = dir + "\\inbox.txt";
    writeWhole(inbox, "");
    writeWhole(dir + "\\ready", "vsthost serving\n");
    std::printf("vsthost: serving jobs from %s\n", inbox.c_str());
    std::fflush(stdout);
    for (unsigned jobNumber = 1;; Sleep(50)) {
        const std::string text = readWhole(inbox);
        if (text.empty()) continue;
        std::vector<std::string> args{"vsthost"};
        bool complete = false;
        for (size_t pos = 0; pos < text.size();) {
            size_t eol = text.find('\n', pos);
            if (eol == std::string::npos) eol = text.size();
            std::string arg = text.substr(pos, eol - pos);
            while (!arg.empty() && (arg.back() == '\r' || arg.back() == ' ')) arg.pop_back();
            pos = eol + 1;
            if (arg == "end") { complete = true; break; }
            if (!arg.empty()) args.push_back(arg);
        }
        if (!complete) continue; // still being written
        writeWhole(inbox, "");
        if (args.size() == 2 && args[1] == "--quit") return 0;
        std::vector<char *> argv;
        for (std::string &a : args) argv.push_back(a.data());
        Options o;
        if (!parseArgs(static_cast<int>(argv.size()), argv.data(), o)) {
            std::printf("vsthost: ignoring malformed job %u\n", jobNumber++);
            continue;
        }
        std::printf("vsthost: job %u (%s) -> %d\n", jobNumber++, o.plugin.c_str(), runJob(o));
        std::fflush(stdout);
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "--serve") return serve(argv[2]);
    Options o;
    if (!parseArgs(argc, argv, o)) {
        std::fprintf(stderr, "usage: vsthost <plugin.dll|plugin.vst3> [--out f.wav] [--report f.json] "
                             "[--notes 60,64,67] [--seconds 3] [--rate 44100] [--block 512] "
                             "[--param i=v] [--play]\n"
                             "       vsthost --serve <job directory>\n");
        return 2;
    }
    return runJob(o);
}
