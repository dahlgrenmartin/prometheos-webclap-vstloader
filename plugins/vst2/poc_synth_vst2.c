/*
 * PoC Synth (VST2): a tiny 8-voice saw synth with a one-pole low-pass filter,
 * built as a 32-bit Windows DLL. It exists to prove that a Windows VST2 binary
 * loads, receives MIDI and renders audio under Wine inside Boxedwine; it uses
 * only the clean-room ABI in include/vst2_abi.h.
 */
#include "vst2_abi.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VOICES 8
#define NUM_PARAMS 3

enum { P_GAIN, P_CUTOFF, P_RELEASE };
static const char *kParamNames[NUM_PARAMS] = {"Gain", "Cutoff", "Release"};

typedef struct {
    int note; /* -1 when free */
    int held;
    float phase, inc, level, velocity;
} Voice;

typedef struct {
    AEffect effect;
    Vst2HostCallback host;
    float params[NUM_PARAMS];
    float sampleRate;
    Voice voices[VOICES];
    float lp[2];
    /* events of the current block, applied at their sample offset */
    VstMidiEvent pending[256];
    int pendingCount;
} Synth;

static Synth *synthOf(AEffect *e) { return (Synth *)e->object; }

static void noteOn(Synth *s, int note, int velocity) {
    Voice *v = &s->voices[0];
    for (int i = 0; i < VOICES; ++i) {
        if (s->voices[i].note < 0) { v = &s->voices[i]; break; }
        if (s->voices[i].level < v->level) v = &s->voices[i];
    }
    v->note = note;
    v->held = 1;
    v->velocity = velocity / 127.0f;
    v->inc = 440.0f * powf(2.0f, (note - 69) / 12.0f) / s->sampleRate;
    v->level = v->velocity;
}

static void noteOff(Synth *s, int note) {
    for (int i = 0; i < VOICES; ++i)
        if (s->voices[i].note == note) s->voices[i].held = 0;
}

static void applyMidi(Synth *s, const VstMidiEvent *m) {
    unsigned char status = (unsigned char)m->midiData[0] & 0xf0;
    int d1 = m->midiData[1] & 0x7f, d2 = m->midiData[2] & 0x7f;
    if (status == 0x90 && d2 > 0) noteOn(s, d1, d2);
    else if (status == 0x80 || (status == 0x90 && d2 == 0)) noteOff(s, d1);
    else if (status == 0xb0 && (d1 == 120 || d1 == 123))
        for (int i = 0; i < VOICES; ++i) s->voices[i].held = 0;
}

static void VST2_CALL processReplacing(AEffect *e, float **in, float **out, int32_t frames) {
    Synth *s = synthOf(e);
    const float gain = s->params[P_GAIN];
    const float cutoffHz = 80.0f * powf(250.0f, s->params[P_CUTOFF]);
    const float a = 1.0f - expf(-6.2831853f * cutoffHz / s->sampleRate);
    const float releaseSec = 0.01f + 2.0f * s->params[P_RELEASE];
    const float decay = expf(-1.0f / (releaseSec * s->sampleRate));
    int next = 0;
    for (int32_t f = 0; f < frames; ++f) {
        while (next < s->pendingCount && s->pending[next].deltaFrames <= f) applyMidi(s, &s->pending[next++]);
        float mix = 0.0f;
        for (int i = 0; i < VOICES; ++i) {
            Voice *v = &s->voices[i];
            if (v->note < 0) continue;
            mix += (2.0f * v->phase - 1.0f) * v->level;
            v->phase += v->inc;
            if (v->phase >= 1.0f) v->phase -= 1.0f;
            if (!v->held) {
                v->level *= decay;
                if (v->level < 1e-4f) v->note = -1;
            }
        }
        s->lp[0] += a * (mix - s->lp[0]);
        s->lp[1] += a * (s->lp[0] - s->lp[1]);
        const float y = s->lp[1] * gain * 0.25f;
        out[0][f] = y;
        out[1][f] = y;
    }
    while (next < s->pendingCount) applyMidi(s, &s->pending[next++]);
    s->pendingCount = 0;
}

static void VST2_CALL setParameter(AEffect *e, int32_t index, float value) {
    if (index >= 0 && index < NUM_PARAMS) synthOf(e)->params[index] = value;
}

static float VST2_CALL getParameter(AEffect *e, int32_t index) {
    return index >= 0 && index < NUM_PARAMS ? synthOf(e)->params[index] : 0.0f;
}

static void copyString(void *ptr, const char *text, size_t capacity) {
    strncpy((char *)ptr, text, capacity - 1);
    ((char *)ptr)[capacity - 1] = '\0';
}

static intptr_t VST2_CALL dispatcher(AEffect *e, int32_t opcode, int32_t index, intptr_t value,
                                     void *ptr, float opt) {
    Synth *s = synthOf(e);
    switch (opcode) {
    case effClose:
        free(s);
        return 1;
    case effSetSampleRate:
        s->sampleRate = opt;
        return 1;
    case effGetEffectName:
        copyString(ptr, "PoC Synth", 32);
        return 1;
    case effGetVendorString:
        copyString(ptr, "prometheos", 64);
        return 1;
    case effGetProductString:
        copyString(ptr, "PoC Synth VST2", 64);
        return 1;
    case effGetParamName:
        if (index >= 0 && index < NUM_PARAMS) copyString(ptr, kParamNames[index], 8);
        return 0;
    case effGetParamDisplay: {
        char text[16];
        int pct = (int)(s->params[index] * 100.0f + 0.5f);
        text[0] = '\0';
        if (index >= 0 && index < NUM_PARAMS) {
            int n = 0, d = pct >= 100 ? 100 : pct;
            if (d >= 100) text[n++] = '1';
            if (d >= 10) text[n++] = (char)('0' + (d / 10) % 10);
            text[n++] = (char)('0' + d % 10);
            text[n++] = '%';
            text[n] = '\0';
        }
        copyString(ptr, text, 8);
        return 0;
    }
    case effProcessEvents: {
        const VstEvents *events = (const VstEvents *)ptr;
        for (int i = 0; i < events->numEvents && s->pendingCount < 256; ++i)
            if (events->events[i]->type == kVstMidiType)
                s->pending[s->pendingCount++] = *(const VstMidiEvent *)events->events[i];
        return 1;
    }
    case effCanDo:
        return ptr && (!strcmp((const char *)ptr, "receiveVstEvents") ||
                       !strcmp((const char *)ptr, "receiveVstMidiEvent"))
                   ? 1
                   : 0;
    case effGetVstVersion:
        return 2400;
    default:
        return 0;
    }
}

VST2_EXPORT AEffect *VST2_CALL VSTPluginMain(Vst2HostCallback host) {
    if (!host || !host(NULL, audioMasterVersion, 0, 0, NULL, 0.0f)) return NULL;
    Synth *s = (Synth *)calloc(1, sizeof(Synth));
    if (!s) return NULL;
    s->host = host;
    s->sampleRate = 44100.0f;
    s->params[P_GAIN] = 0.8f;
    s->params[P_CUTOFF] = 0.6f;
    s->params[P_RELEASE] = 0.2f;
    for (int i = 0; i < VOICES; ++i) s->voices[i].note = -1;
    AEffect *e = &s->effect;
    e->magic = VST2_MAGIC;
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numPrograms = 1;
    e->numParams = NUM_PARAMS;
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsIsSynth;
    e->object = s;
    e->uniqueID = 0x50437379; /* 'PCsy' */
    e->version = 1;
    return e;
}

/* Older hosts look for "main"; poc_synth_vst2.def exports it as an alias. */
