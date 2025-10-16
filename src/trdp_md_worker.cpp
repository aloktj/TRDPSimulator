#include "trdp_simulator/trdp_md_worker.hpp"

#include <chrono>
#include <thread>

namespace trdp_sim {

MdSenderWorker::MdSenderWorker(const MdSenderConfig &config,
                               TrdpStackAdapter &adapter,
                               Logger &logger)
    : config_(config), adapter_(adapter), logger_(logger)
{
    payload_ = load_payload(config.payload);
    adapter_.register_md_sender(config_, [this](const MdMessage &message) {
        logger_.info("Received MD reply for sender '" + config_.name + "' from '" + message.endpoint + "'");
    });
}

MdSenderWorker::~MdSenderWorker()
{
    stop();
}

void MdSenderWorker::start()
{
    if (config_.cycleTimeMs == 0) {
        return;
    }
    if (running_.exchange(true)) {
        return;
    }
    workerThread_ = std::thread(&MdSenderWorker::run, this);
}

void MdSenderWorker::stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void MdSenderWorker::run()
{
    logger_.info("Starting MD sender '" + config_.name + "'");
    const auto interval = std::chrono::milliseconds(config_.cycleTimeMs);
    while (running_) {
        try {
            adapter_.send_md_request(config_.name, payload_);
        } catch (const std::exception &ex) {
            logger_.error("MD request failed for '" + config_.name + "': " + ex.what());
        }
        std::this_thread::sleep_for(interval);
    }
    logger_.info("Stopping MD sender '" + config_.name + "'");
}

}  // namespace trdp_sim
