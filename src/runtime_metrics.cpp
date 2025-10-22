#include "trdp_simulator/runtime_metrics.hpp"

namespace trdp_sim {

void RuntimeMetrics::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    simulatorRunning_ = false;
    adapterInitialized_ = false;
    adapterState_ = "Idle";
    pdPublishers_.clear();
    pdSubscribers_.clear();
    mdSenders_.clear();
    mdListeners_.clear();
}

void RuntimeMetrics::set_simulator_running(bool running)
{
    std::lock_guard<std::mutex> lock(mutex_);
    simulatorRunning_ = running;
}

void RuntimeMetrics::set_adapter_status(bool initialized, std::string state)
{
    std::lock_guard<std::mutex> lock(mutex_);
    adapterInitialized_ = initialized;
    adapterState_ = std::move(state);
}

void RuntimeMetrics::record_pd_publish(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto &entry = ensure_entry(pdPublishers_, name);
    ++entry.packetsSent;
}

void RuntimeMetrics::record_pd_receive(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto &entry = ensure_entry(pdSubscribers_, name);
    ++entry.packetsReceived;
}

void RuntimeMetrics::record_md_request_sent(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto &entry = ensure_entry(mdSenders_, name);
    ++entry.requestsSent;
}

void RuntimeMetrics::record_md_reply_received(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto &entry = ensure_entry(mdSenders_, name);
    ++entry.repliesReceived;
}

void RuntimeMetrics::record_md_request_received(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto &entry = ensure_entry(mdListeners_, name);
    ++entry.requestsReceived;
}

void RuntimeMetrics::record_md_reply_sent(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto &entry = ensure_entry(mdListeners_, name);
    ++entry.repliesSent;
}

RuntimeMetrics::Snapshot RuntimeMetrics::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot snap;
    snap.simulatorRunning = simulatorRunning_;
    snap.adapterInitialized = adapterInitialized_;
    snap.adapterState = adapterState_;
    snap.pdPublishers.reserve(pdPublishers_.size());
    for (const auto &entry : pdPublishers_) {
        snap.pdPublishers.push_back(entry.second);
    }
    snap.pdSubscribers.reserve(pdSubscribers_.size());
    for (const auto &entry : pdSubscribers_) {
        snap.pdSubscribers.push_back(entry.second);
    }
    snap.mdSenders.reserve(mdSenders_.size());
    for (const auto &entry : mdSenders_) {
        snap.mdSenders.push_back(entry.second);
    }
    snap.mdListeners.reserve(mdListeners_.size());
    for (const auto &entry : mdListeners_) {
        snap.mdListeners.push_back(entry.second);
    }
    return snap;
}

}  // namespace trdp_sim

