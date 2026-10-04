// "PoC Invert": a stereo VST2 effect that outputs its input negated, with no
// latency of its own. Parallel to its dry signal, the mix is exactly silent
// only when the host delays the dry path by the bridge latency: the test for
// buzz-remote's plugin-delay compensation. Built here (MinGW) so the test
// needs no third-party binary.
#include "vst2_abi.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    AEffect effect; /* first, so an AEffect* is an Invert* */
    float gain;
} Invert;

static intptr_t VST2_CALL dispatcher(AEffect *e, int32_t op, int32_t index, intptr_t value, void *ptr, float opt) {
    (void)index; (void)value; (void)opt;
    switch (op) {
    case effClose: free(e); return 1;
    case effGetEffectName: strcpy((char *)ptr, "PoC Invert"); return 1;
    case effGetVendorString: strcpy((char *)ptr, "prometheos"); return 1;
    case effGetProductString: strcpy((char *)ptr, "PoC Invert"); return 1;
    case effGetVendorVersion: return 1;
    case effGetVstVersion: return 2400;
    case effGetParamName: strcpy((char *)ptr, "Gain"); return 0;
    case effGetParamLabel: strcpy((char *)ptr, ""); return 0;
    case effGetParamDisplay: strcpy((char *)ptr, "1.0"); return 0;
    default: return 0;
    }
}

static void VST2_CALL setParameter(AEffect *e, int32_t index, float value) {
    if (index == 0) ((Invert *)e)->gain = value;
}
static float VST2_CALL getParameter(AEffect *e, int32_t index) { return index == 0 ? ((Invert *)e)->gain : 0.0f; }

static void VST2_CALL processReplacing(AEffect *e, float **in, float **out, int32_t frames) {
    const float gain = ((Invert *)e)->gain;
    for (int c = 0; c < 2; ++c)
        for (int32_t i = 0; i < frames; ++i) out[c][i] = -gain * in[c][i];
}

static void VST2_CALL processAccumulating(AEffect *e, float **in, float **out, int32_t frames) {
    const float gain = ((Invert *)e)->gain;
    for (int c = 0; c < 2; ++c)
        for (int32_t i = 0; i < frames; ++i) out[c][i] += -gain * in[c][i];
}

VST2_EXPORT AEffect *VST2_CALL VSTPluginMain(Vst2HostCallback host) {
    (void)host;
    Invert *self = (Invert *)calloc(1, sizeof(Invert));
    if (!self) return NULL;
    self->gain = 1.0f;
    AEffect *effect = &self->effect;
    effect->magic = VST2_MAGIC;
    effect->dispatcher = dispatcher;
    effect->process = processAccumulating;
    effect->setParameter = setParameter;
    effect->getParameter = getParameter;
    effect->numPrograms = 1;
    effect->numParams = 1;
    effect->numInputs = 2;
    effect->numOutputs = 2;
    effect->flags = effFlagsCanReplacing;
    effect->uniqueID = 0x506f4976; /* 'PoIv' */
    effect->version = 1;
    effect->processReplacing = processReplacing;
    return effect;
}
