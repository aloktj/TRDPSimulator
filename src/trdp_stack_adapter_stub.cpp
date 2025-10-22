#include "trdp_simulator/trdp_stack_adapter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace trdp_sim {
namespace {

std::string fallback_endpoint(const std::string &name, const std::string &ip)
{
    return ip.empty() ? name : ip;
}

MdSessionId make_session_id(std::uint32_t value)
{
    MdSessionId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[id.size() - 1U - index] = static_cast<std::uint8_t>(value & 0xFFU);
        value >>= 8U;
    }
    return id;
}

struct MdSessionIdHash {
    std::size_t operator()(const MdSessionId &id) const noexcept
    {
        std::size_t value = 0U;
        for (auto byte : id) {
            value = (value * 131U) ^ static_cast<std::size_t>(byte);
        }
        return value;
    }
};

class StubTrdpStackAdapter : public TrdpStackAdapter {
public:
    void initialize(const NetworkConfig &, const LoggingConfig &) override {}

    void shutdown() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pdPublishers_.clear();
        pdSubscribers_.clear();
        mdSenders_.clear();
        mdListeners_.clear();
        mdSessions_.clear();
    }

    void register_pd_publisher(const PdPublisherConfig &config) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pdPublishers_[config.name] = {config, 0U};
    }

    void register_pd_subscriber(const PdSubscriberConfig &config, PdHandler handler) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pdSubscribers_.push_back({config, std::move(handler)});
    }

    void publish_pd(const std::string &publisherName, const std::vector<std::uint8_t> &data) override
    {
        PdPublisherConfig publisherConfig;
        std::uint64_t sequence{};
        std::vector<PdSubscriberState> targets;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = pdPublishers_.find(publisherName);
            if (it == pdPublishers_.end()) {
                throw std::runtime_error("Unknown PD publisher '" + publisherName + "'");
            }
            publisherConfig = it->second.config;
            sequence = ++it->second.sequenceCounter;

            for (const auto &subscriber : pdSubscribers_) {
                if (!matches_pd_subscription(subscriber.config, publisherConfig)) {
                    continue;
                }
                targets.push_back(subscriber);
            }
        }

        if (targets.empty()) {
            return;
        }

        PdMessage message;
        message.endpoint = fallback_endpoint(publisherName, publisherConfig.sourceIp);
        message.comId = publisherConfig.comId;
        message.payload = data;
        message.sequenceCounter = sequence;

        for (const auto &subscriber : targets) {
            if (subscriber.handler) {
                subscriber.handler(message);
            }
        }
    }

    void register_md_sender(const MdSenderConfig &config, MdHandler handler) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mdSenders_[config.name] = {config, std::move(handler)};
    }

    void send_md_request(const std::string &senderName, const std::vector<std::uint8_t> &data) override
    {
        MdSenderConfig senderConfig;
        MdHandler replyHandler;
        std::vector<MdListenerState> listeners;
        MdSessionId sessionId{};

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = mdSenders_.find(senderName);
            if (it == mdSenders_.end()) {
                throw std::runtime_error("Unknown MD sender '" + senderName + "'");
            }

            senderConfig = it->second.config;
            replyHandler = it->second.replyHandler;
            const auto numericSession = nextSessionId_++;
            sessionId = make_session_id(numericSession);

            for (const auto &listener : mdListeners_) {
                if (!matches_md_listener(listener.config, senderConfig)) {
                    continue;
                }
                listeners.push_back(listener);
            }

            if (replyHandler) {
                mdSessions_[sessionId] = {senderName, replyHandler};
            }
        }

        MdMessage request;
        request.endpoint = fallback_endpoint(senderName, senderConfig.sourceIp);
        request.comId = senderConfig.comId;
        request.sessionId = sessionId;
        request.payload = data;

        for (const auto &listener : listeners) {
            if (listener.handler) {
                listener.handler(request);
            }
        }

        if (!senderConfig.expectReply && replyHandler) {
            MdMessage reply;
            reply.endpoint = senderConfig.destIp.empty() ? "stub-listener" : senderConfig.destIp;
            reply.comId = senderConfig.replyComId == 0 ? senderConfig.comId : senderConfig.replyComId;
            reply.sessionId = sessionId;
            reply.payload.clear();
            replyHandler(reply);

            std::lock_guard<std::mutex> lock(mutex_);
            mdSessions_.erase(sessionId);
        }
    }

    void register_md_listener(const MdListenerConfig &config, MdHandler handler) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mdListeners_.push_back({config, std::move(handler)});
    }

    void send_md_reply(const std::string &listenerName, const MdMessage &request, const std::vector<std::uint8_t> &data) override
    {
        MdHandler replyHandler;
        MdListenerConfig listenerConfig;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto sessionIt = mdSessions_.find(request.sessionId);
            if (sessionIt == mdSessions_.end()) {
                return;
            }
            replyHandler = sessionIt->second.replyHandler;
            mdSessions_.erase(sessionIt);

            const auto listenerIt = std::find_if(mdListeners_.begin(), mdListeners_.end(),
                [&listenerName](const MdListenerState &state) { return state.config.name == listenerName; });
            if (listenerIt != mdListeners_.end()) {
                listenerConfig = listenerIt->config;
            }
        }

        if (!replyHandler) {
            return;
        }

        MdMessage reply;
        reply.endpoint = fallback_endpoint(listenerName, listenerConfig.sourceIp);
        reply.comId = request.comId;
        reply.sessionId = request.sessionId;
        reply.payload = data;
        replyHandler(reply);
    }

    void poll(std::chrono::milliseconds timeout) override
    {
        std::this_thread::sleep_for(timeout);
    }

private:
    struct PdPublisherState {
        PdPublisherConfig config;
        std::uint64_t sequenceCounter;
    };

    struct PdSubscriberState {
        PdSubscriberConfig config;
        PdHandler handler;
    };

    struct MdSenderState {
        MdSenderConfig config;
        MdHandler replyHandler;
    };

    struct MdListenerState {
        MdListenerConfig config;
        MdHandler handler;
    };

    struct MdSessionState {
        std::string senderName;
        MdHandler replyHandler;
    };

    static bool matches_pd_subscription(const PdSubscriberConfig &subscriber, const PdPublisherConfig &publisher)
    {
        if (subscriber.enableComIdFiltering && subscriber.comId != 0 && subscriber.comId != publisher.comId) {
            return false;
        }
        if (!subscriber.sourceIp.empty() && !publisher.destIp.empty() && subscriber.sourceIp != publisher.destIp) {
            return false;
        }
        if (!subscriber.destIp.empty() && !publisher.sourceIp.empty() && subscriber.destIp != publisher.sourceIp) {
            return false;
        }
        return true;
    }

    static bool matches_md_listener(const MdListenerConfig &listener, const MdSenderConfig &sender)
    {
        if (listener.comId != 0 && listener.comId != sender.comId) {
            return false;
        }
        if (!listener.destIp.empty() && !sender.destIp.empty() && listener.destIp != sender.destIp) {
            return false;
        }
        if (!listener.sourceIp.empty() && !sender.sourceIp.empty() && listener.sourceIp != sender.sourceIp) {
            return false;
        }
        return true;
    }

    std::mutex mutex_;
    std::unordered_map<std::string, PdPublisherState> pdPublishers_;
    std::vector<PdSubscriberState> pdSubscribers_;
    std::unordered_map<std::string, MdSenderState> mdSenders_;
    std::vector<MdListenerState> mdListeners_;
    std::unordered_map<MdSessionId, MdSessionState, MdSessionIdHash> mdSessions_;
    std::atomic<std::uint32_t> nextSessionId_{1};
};
}  // namespace

std::unique_ptr<TrdpStackAdapter> create_stub_trdp_stack_adapter()
{
    return std::make_unique<StubTrdpStackAdapter>();
}

}  // namespace trdp_sim
