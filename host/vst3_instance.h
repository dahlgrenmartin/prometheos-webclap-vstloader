// A VST3 plugin (IComponent + IAudioProcessor, with its IEditController) as a
// PluginInstance. The SDK interfaces stay in vst3_instance.cpp.
#pragma once

#include "plugin_instance.h"

#include <memory>

class Vst3Instance final : public PluginInstance {
  public:
    Vst3Instance();
    ~Vst3Instance() override;

    bool load(const std::string &path, double rate, int block, std::string &error) override;
    void close() override;
    void process(const vstb_request &request, const float *inputs, float *outputs) override;
    std::string describeJson() override;
    std::vector<uint8_t> getState() override;
    bool setState(const uint8_t *data, size_t size, std::string &error) override;
    int latency() const override;

    struct Impl;

  protected:
    void reset() override;

  private:
    std::unique_ptr<Impl> impl_;
};
