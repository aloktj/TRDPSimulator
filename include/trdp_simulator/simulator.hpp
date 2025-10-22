#pragma once

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include "trdp_simulator/config.hpp"
#include "trdp_simulator/logger.hpp"
#include "trdp_simulator/runtime_metrics.hpp"
#include "trdp_simulator/trdp_stack_adapter.hpp"

namespace trdp_sim {

class PdPublisherWorker;
class MdSenderWorker;

class Simulator {
public:
    Simulator(SimulatorConfig config, std::unique_ptr<TrdpStackAdapter> adapter);
    ~Simulator();

    void run();
    void stop();

    RuntimeMetrics::Snapshot metrics_snapshot() const;

private:
    void setup_logging();
    void setup_pd_workers();
    void setup_md_workers();
    void start_event_loop();

    SimulatorConfig config_;
    std::unique_ptr<TrdpStackAdapter> adapter_;
    Logger logger_;
    std::unique_ptr<std::ofstream> logFile_;

    std::shared_ptr<RuntimeMetrics> metrics_;

    std::vector<std::unique_ptr<PdPublisherWorker>> pdWorkers_;
    std::vector<std::unique_ptr<MdSenderWorker>> mdWorkers_;

    std::atomic<bool> running_{false};
    std::thread eventThread_;
    std::mutex stateMutex_;
    std::condition_variable stateCv_;
    bool cleanedUp_{false};
};

}  // namespace trdp_sim
