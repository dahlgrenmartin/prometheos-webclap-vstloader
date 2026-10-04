#pragma once

// The VST 2.4 binary interface, written from the published ABI (as LMMS's
// VeSTige and other clean-room hosts do) rather than from Steinberg's
// withdrawn VST2 SDK. Only what this proof of concept uses is declared.
//
// On Windows every VST2 function pointer is __cdecl, and AEffect is laid out
// with the platform's natural alignment (8-byte pack on Win64, 4 on Win32 for
// the pointer-sized fields used here).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define VST2_CALL __cdecl
#define VST2_EXPORT __declspec(dllexport)
#else
#define VST2_CALL
#define VST2_EXPORT __attribute__((visibility("default")))
#endif

#define VST2_MAGIC 0x56737450 /* 'VstP' */

typedef struct AEffect AEffect;
typedef intptr_t(VST2_CALL *Vst2HostCallback)(AEffect *effect, int32_t opcode, int32_t index,
                                              intptr_t value, void *ptr, float opt);
typedef intptr_t(VST2_CALL *Vst2Dispatcher)(AEffect *effect, int32_t opcode, int32_t index,
                                            intptr_t value, void *ptr, float opt);
typedef void(VST2_CALL *Vst2Process)(AEffect *effect, float **inputs, float **outputs,
                                     int32_t frames);
typedef void(VST2_CALL *Vst2ProcessDouble)(AEffect *effect, double **inputs, double **outputs,
                                           int32_t frames);
typedef void(VST2_CALL *Vst2SetParameter)(AEffect *effect, int32_t index, float value);
typedef float(VST2_CALL *Vst2GetParameter)(AEffect *effect, int32_t index);
typedef AEffect *(VST2_CALL *Vst2PluginMain)(Vst2HostCallback host);

struct AEffect {
    int32_t magic;
    Vst2Dispatcher dispatcher;
    Vst2Process process; /* deprecated accumulating process */
    Vst2SetParameter setParameter;
    Vst2GetParameter getParameter;
    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags;
    intptr_t resvd1;
    intptr_t resvd2;
    int32_t initialDelay;
    int32_t realQualities; /* deprecated */
    int32_t offQualities;  /* deprecated */
    float ioRatio;         /* deprecated */
    void *object;
    void *user;
    int32_t uniqueID;
    int32_t version;
    Vst2Process processReplacing;
    Vst2ProcessDouble processDoubleReplacing;
    char future[56];
};

/* AEffect.flags */
enum {
    effFlagsHasEditor = 1 << 0,
    effFlagsCanReplacing = 1 << 4,
    effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth = 1 << 8,
    effFlagsNoSoundInStop = 1 << 9,
    effFlagsCanDoubleReplacing = 1 << 12,
};

/* host -> plugin dispatcher opcodes */
enum {
    effOpen = 0,
    effClose = 1,
    effSetProgram = 2,
    effGetProgram = 3,
    effGetProgramName = 5,
    effGetParamLabel = 6,
    effGetParamDisplay = 7,
    effGetParamName = 8,
    effSetSampleRate = 10,
    effSetBlockSize = 11,
    effMainsChanged = 12,
    effEditGetRect = 13,
    effEditOpen = 14,
    effEditClose = 15,
    effEditIdle = 19,
    effGetChunk = 23,
    effSetChunk = 24,
    effProcessEvents = 25,
    effGetEffectName = 45,
    effGetVendorString = 47,
    effGetProductString = 48,
    effGetVendorVersion = 49,
    effCanDo = 51,
    effGetVstVersion = 58,
    effStartProcess = 71,
    effStopProcess = 72,
};

/* plugin -> host callback opcodes */
enum {
    audioMasterAutomate = 0,
    audioMasterVersion = 1,
    audioMasterCurrentId = 2,
    audioMasterIdle = 3,
    audioMasterGetTime = 7,
    audioMasterProcessEvents = 8,
    audioMasterIOChanged = 13,
    audioMasterSizeWindow = 15,
    audioMasterGetSampleRate = 16,
    audioMasterGetBlockSize = 17,
    audioMasterGetCurrentProcessLevel = 23,
    audioMasterGetVendorString = 32,
    audioMasterGetProductString = 33,
    audioMasterGetVendorVersion = 34,
    audioMasterCanDo = 37,
    audioMasterUpdateDisplay = 42,
    audioMasterBeginEdit = 43,
    audioMasterEndEdit = 44,
};

enum { kVstMidiType = 1 };

typedef struct VstEvent {
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    char data[16];
} VstEvent;

typedef struct VstMidiEvent {
    int32_t type; /* kVstMidiType */
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    int32_t noteLength;
    int32_t noteOffset;
    char midiData[4];
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
} VstMidiEvent;

typedef struct VstEvents {
    int32_t numEvents;
    intptr_t reserved;
    VstEvent *events[2]; /* variable length */
} VstEvents;

typedef struct VstTimeInfo {
    double samplePos;
    double sampleRate;
    double nanoSeconds;
    double ppqPos;
    double tempo;
    double barStartPos;
    double cycleStartPos;
    double cycleEndPos;
    int32_t timeSigNumerator;
    int32_t timeSigDenominator;
    int32_t smpteOffset;
    int32_t smpteFrameRate;
    int32_t samplesToNextClock;
    int32_t flags;
} VstTimeInfo;

enum {
    kVstTransportPlaying = 1 << 1,
    kVstNanosValid = 1 << 8,
    kVstPpqPosValid = 1 << 9,
    kVstTempoValid = 1 << 10,
};

#ifdef __cplusplus
}
#endif
