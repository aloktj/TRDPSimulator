#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "trdp_simulator/config.hpp"
#include "trdp_simulator/logger.hpp"
#include "trdp_simulator/runtime_metrics.hpp"
#include "trdp_simulator/trdp_stack_adapter.hpp"

namespace trdp_sim {

class MdSenderWorker {
public:
    MdSenderWorker(const MdSenderConfig &config,
                   TrdpStackAdapter &adapter,
                   Logger &logger,
                   RuntimeMetrics &metrics);
    ~MdSenderWorker();

    void start();
    void stop();

    const std::string &name() const { return config_.name; }
    PayloadConfig payload_config() const;
    bool update_payload(PayloadConfig::Format format, const std::string &value, std::string &error_message);

private:
    void run();

    MdSenderConfig config_;
    TrdpStackAdapter &adapter_;
    Logger &logger_;
    RuntimeMetrics &metrics_;

    std::atomic<bool> running_{false};
    std::thread workerThread_;
    mutable std::mutex payloadMutex_;
    std::vector<std::uint8_t> payload_;
};

}  // namespace trdp_sim

