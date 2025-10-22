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
    SimulatorConfig current_config() const;
    bool set_pd_payload(const std::string &publisher_name,
                        PayloadConfig::Format format,
                        const std::string &value,
                        std::string &error_message);
    bool set_md_payload(const std::string &sender_name,
                        PayloadConfig::Format format,
                        const std::string &value,
                        std::string &error_message);

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
    mutable std::mutex stateMutex_;
    std::condition_variable stateCv_;
    bool cleanedUp_{false};
};

}  // namespace trdp_sim
