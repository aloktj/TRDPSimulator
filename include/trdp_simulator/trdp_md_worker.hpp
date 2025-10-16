#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "trdp_simulator/config.hpp"
#include "trdp_simulator/logger.hpp"
#include "trdp_simulator/trdp_stack_adapter.hpp"

namespace trdp_sim {

class MdSenderWorker {
public:
    MdSenderWorker(const MdSenderConfig &config,
                   TrdpStackAdapter &adapter,
                   Logger &logger);
    ~MdSenderWorker();

    void start();
    void stop();

private:
    void run();

    MdSenderConfig config_;
    TrdpStackAdapter &adapter_;
    Logger &logger_;

    std::atomic<bool> running_{false};
    std::thread workerThread_;
    std::vector<std::uint8_t> payload_;
};

}  // namespace trdp_sim
