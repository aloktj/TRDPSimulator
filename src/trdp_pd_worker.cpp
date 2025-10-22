#include "trdp_simulator/trdp_pd_worker.hpp"

#include <chrono>
#include <thread>

namespace trdp_sim {

PdPublisherWorker::PdPublisherWorker(const PdPublisherConfig &config,
                                     TrdpStackAdapter &adapter,
                                     Logger &logger,
                                     RuntimeMetrics &metrics)
    : config_(config), adapter_(adapter), logger_(logger), metrics_(metrics)
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
            std::vector<std::uint8_t> payloadCopy;
            {
                std::lock_guard<std::mutex> lock(payloadMutex_);
                payloadCopy = payload_;
            }
            adapter_.publish_pd(config_.name, payloadCopy);
            metrics_.record_pd_publish(config_.name);
        } catch (const std::exception &ex) {
            logger_.error("PD publish failed for '" + config_.name + "': " + ex.what());
        }
        std::this_thread::sleep_for(interval);
    }
    logger_.info("Stopping PD publisher '" + config_.name + "'");
}

PayloadConfig PdPublisherWorker::payload_config() const
{
    std::lock_guard<std::mutex> lock(payloadMutex_);
    return config_.payload;
}

bool PdPublisherWorker::update_payload(PayloadConfig::Format format, const std::string &value, std::string &error_message)
{
    PayloadConfig payloadSpec;
    payloadSpec.format = format;
    payloadSpec.value = value;
    try {
        auto data = load_payload(payloadSpec);
        {
            std::lock_guard<std::mutex> lock(payloadMutex_);
            payload_ = std::move(data);
            config_.payload = payloadSpec;
        }
        return true;
    } catch (const std::exception &ex) {
        error_message = ex.what();
        return false;
    }
}

}  // namespace trdp_sim

