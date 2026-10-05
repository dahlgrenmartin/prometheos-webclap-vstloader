// A VST2 plugin (AEffect) as a PluginInstance.
#pragma once

#include "plugin_instance.h"
#include "vst2_abi.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

class Vst2Instance final : public PluginInstance {
  public:
    ~Vst2Instance() override;

    bool load(const std::string &path, double rate, int block, std::string &error) override;
    void close() override;
    void process(const vstb_request &request, const float *inputs, float *outputs) override;
    std::string describeJson() override;
    std::vector<uint8_t> getState() override;
    bool setState(const uint8_t *data, size_t size, std::string &error) override;
    int latency() const override { return effect_ ? effect_->initialDelay : 0; }

    // The host callback's view of this instance.
    VstTimeInfo time{};

  protected:
    void reset() override;

  private:
    intptr_t dispatch(int32_t op, int32_t index = 0, intptr_t value = 0, void *ptr = nullptr, float opt = 0);

    HMODULE dll_ = nullptr;
    AEffect *effect_ = nullptr;
    std::vector<std::vector<float>> inBuf_, outBuf_;
    std::vector<float *> inPtr_, outPtr_;
    std::vector<VstMidiEvent> midi_;
    std::vector<char> eventStorage_;
    std::string path_;
};
