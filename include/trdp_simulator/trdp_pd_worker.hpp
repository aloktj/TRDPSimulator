#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "trdp_simulator/config.hpp"
#include "trdp_simulator/logger.hpp"
#include "trdp_simulator/trdp_stack_adapter.hpp"

namespace trdp_sim {

class PdPublisherWorker {
public:
    PdPublisherWorker(const PdPublisherConfig &config,
                      TrdpStackAdapter &adapter,
                      Logger &logger);
    ~PdPublisherWorker();

    void start();
    void stop();

private:
    void run();

    PdPublisherConfig config_;
    TrdpStackAdapter &adapter_;
    Logger &logger_;

    std::atomic<bool> running_{false};
    std::thread workerThread_;
    std::vector<std::uint8_t> payload_;
};

}  // namespace trdp_sim
