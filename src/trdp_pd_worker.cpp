#include "trdp_simulator/trdp_pd_worker.hpp"

#include <chrono>
#include <thread>

namespace trdp_sim {

PdPublisherWorker::PdPublisherWorker(const PdPublisherConfig &config,
                                     TrdpStackAdapter &adapter,
                                     Logger &logger)
    : config_(config), adapter_(adapter), logger_(logger)
{
    payload_ = load_payload(config.payload);
    adapter_.register_pd_publisher(config_);
}

PdPublisherWorker::~PdPublisherWorker()
{
    stop();
}

void PdPublisherWorker::start()
{
    if (running_.exchange(true)) {
        return;
    }
    workerThread_ = std::thread(&PdPublisherWorker::run, this);
}

void PdPublisherWorker::stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void PdPublisherWorker::run()
{
    logger_.info("Starting PD publisher '" + config_.name + "'");
    const auto interval = std::chrono::milliseconds(config_.cycleTimeMs);
    while (running_) {
        try {
            adapter_.publish_pd(config_.name, payload_);
        } catch (const std::exception &ex) {
            logger_.error("PD publish failed for '" + config_.name + "': " + ex.what());
        }
        std::this_thread::sleep_for(interval);
    }
    logger_.info("Stopping PD publisher '" + config_.name + "'");
}

}  // namespace trdp_sim
