#include "trdp_simulator/simulator.hpp"

#include <csignal>
#include <iomanip>
#include <sstream>
#include <thread>

#include "trdp_simulator/trdp_md_worker.hpp"
#include "trdp_simulator/trdp_pd_worker.hpp"

namespace trdp_sim {
namespace {
std::string to_hex(const std::vector<std::uint8_t> &data)
{
    std::ostringstream oss;
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (i != 0) {
            oss << ' ';
        }
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    return oss.str();
}
}  // namespace

Simulator::Simulator(SimulatorConfig config, std::unique_ptr<TrdpStackAdapter> adapter)
    : config_(std::move(config)), adapter_(std::move(adapter)), logger_(config_.logging.level)
{
}

Simulator::~Simulator()
{
    stop();
}

void Simulator::run()
{
    setup_logging();

    logger_.info("Initializing TRDP stack");
    adapter_->initialize(config_.network, config_.logging);

    running_.store(true);
    cleanedUp_ = false;

    // Register PD subscribers
    for (const auto &subscriber : config_.pdSubscribers) {
        adapter_->register_pd_subscriber(subscriber, [this, name = subscriber.name](const PdMessage &message) {
            logger_.info("PD subscriber '" + name + "' received COMID " + std::to_string(message.comId) +
                         " payload=" + to_hex(message.payload));
        });
    }

    // Register MD listeners
    for (const auto &listener : config_.mdListeners) {
        std::vector<std::uint8_t> replyPayload;
        if (listener.autoReply && !listener.replyPayload.value.empty()) {
            replyPayload = load_payload(listener.replyPayload);
        }
        adapter_->register_md_listener(listener,
            [this, cfg = listener, replyPayload](const MdMessage &message) mutable {
                logger_.info("MD listener '" + cfg.name + "' received COMID " + std::to_string(message.comId) +
                             " payload=" + to_hex(message.payload));
                if (cfg.autoReply && !replyPayload.empty()) {
                    try {
                        adapter_->send_md_reply(cfg.name, message, replyPayload);
                        logger_.info("MD listener '" + cfg.name + "' sent automatic reply");
                    } catch (const std::exception &ex) {
                        logger_.error("MD listener '" + cfg.name + "' failed to send reply: " + ex.what());
                    }
                }
            });
    }

    setup_pd_workers();
    setup_md_workers();
    start_event_loop();

    for (auto &worker : pdWorkers_) {
        worker->start();
    }
    for (auto &worker : mdWorkers_) {
        worker->start();
    }

    std::unique_lock<std::mutex> lock(stateMutex_);
    stateCv_.wait(lock, [this] { return !running_.load(); });

    // Cleanup is handled after exit
    if (!cleanedUp_) {
        cleanedUp_ = true;
        lock.unlock();
        stop();
    }
}

void Simulator::stop()
{
    const bool wasRunning = running_.exchange(false);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        stateCv_.notify_all();
    }

    if (wasRunning || !cleanedUp_) {
        cleanedUp_ = true;

        for (auto &worker : pdWorkers_) {
            if (worker) {
                worker->stop();
            }
        }
        for (auto &worker : mdWorkers_) {
            if (worker) {
                worker->stop();
            }
        }

        if (eventThread_.joinable()) {
            eventThread_.join();
        }

        if (adapter_) {
            try {
                adapter_->shutdown();
            } catch (const std::exception &ex) {
                logger_.warn("TRDP adapter shutdown reported error: " + std::string(ex.what()));
            }
        }

        pdWorkers_.clear();
        mdWorkers_.clear();
    }
}

void Simulator::setup_logging()
{
    logger_.set_level(config_.logging.level);
    logger_.enable_console(config_.logging.enableConsole);
    if (!config_.logging.filePath.empty()) {
        logFile_ = std::make_unique<std::ofstream>(config_.logging.filePath, std::ios::app);
        if (!logFile_->is_open()) {
            throw std::runtime_error("Unable to open log file: " + config_.logging.filePath);
        }
        logger_.set_file(logFile_.get());
    }
}

void Simulator::setup_pd_workers()
{
    for (const auto &publisher : config_.pdPublishers) {
        pdWorkers_.push_back(std::make_unique<PdPublisherWorker>(publisher, *adapter_, logger_));
    }
}

void Simulator::setup_md_workers()
{
    for (const auto &sender : config_.mdSenders) {
        mdWorkers_.push_back(std::make_unique<MdSenderWorker>(sender, *adapter_, logger_));
    }
}

void Simulator::start_event_loop()
{
    if (eventThread_.joinable()) {
        return;
    }
    eventThread_ = std::thread([this] {
        while (running_.load()) {
            try {
                adapter_->poll(std::chrono::milliseconds(100));
            } catch (const std::exception &ex) {
                logger_.warn("TRDP poll failed: " + std::string(ex.what()));
            }
        }
    });
}

}  // namespace trdp_sim
