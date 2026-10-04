// One loaded VST2 plugin, driven block by block from vstbridge requests.
//
// The real-time bridge (vsthost --bridge) and the offline replay
// (vsthost --replay) both run plugins through this class on a thread set up by
// prepareAudioThread(), so a recorded request stream replayed offline renders
// exactly what the bridge streamed.
#pragma once

#include "vst2_abi.h"
#include "vstbridge_abi.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// FTZ/DAZ in MXCSR, so denormals cannot stall the emulated FPU, and a
// time-critical priority. (The x87 unit has no flush-to-zero; its control word
// keeps the thread default.)
void prepareAudioThread();

class Vst2Instance {
  public:
    ~Vst2Instance();

    bool load(const std::string &path, double rate, int block, std::string &error);
    void close();

    // Renders a few seconds of chords and their release, then resets the
    // plugin, so live notes don't pay first-time JIT translation of the note path.
    void warmUp();

    // One block: events from `request`, inputs planar (inPorts x 2 x block),
    // outputs planar (outPorts x 2 x block). No allocation.
    void process(const vstb_request &request, const float *inputs, float *outputs);

    std::string describeJson();
    std::vector<uint8_t> getState();
    bool setState(const uint8_t *data, size_t size, std::string &error);

    int block() const { return block_; }
    int inPorts() const { return inPorts_; }
    int outPorts() const { return outPorts_; }
    int latency() const { return effect_ ? effect_->initialDelay : 0; }
    bool takeIoChanged();

    // The host callback's view of this instance.
    VstTimeInfo time{};
    double rate = 44100.0;
    bool ioChanged = false;

  private:
    intptr_t dispatch(int32_t op, int32_t index = 0, intptr_t value = 0, void *ptr = nullptr, float opt = 0);

    HMODULE dll_ = nullptr;
    AEffect *effect_ = nullptr;
    int block_ = 256;
    int inPorts_ = 0;
    int outPorts_ = 1;
    std::vector<std::vector<float>> inBuf_, outBuf_;
    std::vector<float *> inPtr_, outPtr_;
    std::vector<VstMidiEvent> midi_;
    std::vector<char> eventStorage_;
    std::string path_;
};
