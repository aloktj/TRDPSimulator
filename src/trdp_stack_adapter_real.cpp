#ifdef TRDPSIM_WITH_TRDP

#include "trdp_simulator/trdp_stack_adapter.hpp"

#ifndef MD_SUPPORT
#define MD_SUPPORT 1
#endif

#include <arpa/inet.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <trdp_if_light.h>
#include <vos_sock.h>
#include <vos_thread.h>
#include <vos_utils.h>
}

namespace trdp_sim {
namespace {

std::string fallback_endpoint(const std::string &name, const std::string &ip)
{
    return ip.empty() ? name : ip;
}

TRDP_IP_ADDR_T parse_ip(const std::string &ip)
{
    if (ip.empty()) {
        return VOS_INADDR_ANY;
    }

    const auto addr = inet_addr(ip.c_str());
    if (addr == INADDR_NONE) {
        throw std::runtime_error("Invalid IP address: " + ip);
    }
    return static_cast<TRDP_IP_ADDR_T>(addr);
}

std::string ip_to_string(TRDP_IP_ADDR_T address)
{
    if (address == 0U) {
        return {};
    }

    in_addr in{};
    in.s_addr = address;
    char buffer[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &in, buffer, sizeof(buffer)) == nullptr) {
        return {};
    }
    return buffer;
}

MdSessionId to_session_id(const UINT8 *sessionId)
{
    MdSessionId id{};
    if (sessionId != nullptr) {
        std::copy(sessionId, sessionId + id.size(), id.begin());
    }
    return id;
}

class RealTrdpStackAdapter : public TrdpStackAdapter {
public:
    RealTrdpStackAdapter()
    {
        std::memset(&memConfig_, 0, sizeof(memConfig_));
        std::memset(&processConfig_, 0, sizeof(processConfig_));
        std::memset(&pdConfig_, 0, sizeof(pdConfig_));
        std::memset(&mdConfig_, 0, sizeof(mdConfig_));
    }

    ~RealTrdpStackAdapter() override
    {
        shutdown();
    }

    void initialize(const NetworkConfig &networkConfig, const LoggingConfig &) override
    {
        networkConfig_ = networkConfig;

        std::fill(std::begin(memConfig_.prealloc), std::end(memConfig_.prealloc), 0U);
        memConfig_.p = nullptr;
        memConfig_.size = 0U;

        const TRDP_ERR_T errInit = tlc_init(nullptr, nullptr, &memConfig_);
        if (errInit != TRDP_NO_ERR) {
            throw std::runtime_error("tlc_init failed with error " + std::to_string(errInit));
        }

        std::snprintf(processConfig_.hostName, sizeof(processConfig_.hostName), "%s", networkConfig.interfaceName.c_str());
        processConfig_.hostName[sizeof(processConfig_.hostName) - 1U] = '\0';
        processConfig_.leaderName[0] = '\0';
        processConfig_.type[0] = '\0';
        processConfig_.cycleTime = TRDP_PROCESS_DEFAULT_CYCLE_TIME;
        processConfig_.priority = 0U;
        processConfig_.options = TRDP_OPTION_BLOCK;
        processConfig_.vlanId = networkConfig.vlanId;

        pdConfig_.pfCbFunction = nullptr;
        pdConfig_.pRefCon = nullptr;
        pdConfig_.sendParam.qos = TRDP_PD_DEFAULT_QOS;
        pdConfig_.sendParam.ttl = networkConfig.ttl;
        pdConfig_.sendParam.retries = 0U;
        pdConfig_.flags = TRDP_FLAGS_NONE;
        pdConfig_.timeout = TRDP_PD_DEFAULT_TIMEOUT;
        pdConfig_.toBehavior = TRDP_TO_SET_TO_ZERO;
        pdConfig_.port = 0U;

        mdConfig_.pfCbFunction = nullptr;
        mdConfig_.pRefCon = nullptr;
        mdConfig_.sendParam.qos = TRDP_MD_DEFAULT_QOS;
        mdConfig_.sendParam.ttl = networkConfig.ttl;
        mdConfig_.sendParam.retries = TRDP_MD_DEFAULT_RETRIES;
        mdConfig_.flags = TRDP_FLAGS_NONE;
        mdConfig_.replyTimeout = TRDP_MD_DEFAULT_REPLY_TIMEOUT;
        mdConfig_.confirmTimeout = TRDP_MD_DEFAULT_CONFIRM_TIMEOUT;
        mdConfig_.connectTimeout = TRDP_MD_DEFAULT_CONNECTION_TIMEOUT;
        mdConfig_.sendingTimeout = TRDP_MD_DEFAULT_SENDING_TIMEOUT;
        mdConfig_.udpPort = 0U;
        mdConfig_.tcpPort = 0U;
        mdConfig_.maxNumSessions = TRDP_MD_MAX_NUM_SESSIONS;

        const TRDP_IP_ADDR_T ownIp = parse_ip(networkConfig.hostIp);
        const TRDP_ERR_T errSession = tlc_openSession(&appHandle_, ownIp, 0U, nullptr, &pdConfig_, &mdConfig_, &processConfig_);
        if (errSession != TRDP_NO_ERR) {
            tlc_terminate();
            appHandle_ = nullptr;
            throw std::runtime_error("tlc_openSession failed with error " + std::to_string(errSession));
        }
    }

    void shutdown() override
    {
        if (appHandle_ == nullptr) {
            return;
        }

        for (auto &publisher : pdPublishers_) {
            tlp_unpublish(appHandle_, publisher.second.handle);
        }
        pdPublishers_.clear();

        for (auto &subscriber : pdSubscribers_) {
            tlp_unsubscribe(appHandle_, subscriber.first);
        }
        pdSubscribers_.clear();

        for (auto &listener : mdListeners_) {
            tlm_delListener(appHandle_, listener.second->handle);
        }
        mdListeners_.clear();
        mdSenders_.clear();

        tlc_closeSession(appHandle_);
        tlc_terminate();
        appHandle_ = nullptr;
    }

    void register_pd_publisher(const PdPublisherConfig &config) override
    {
        PublisherState state;
        state.config = config;

        const TRDP_IP_ADDR_T srcIp = parse_ip(config.sourceIp.empty() ? networkConfig_.hostIp : config.sourceIp);
        const TRDP_IP_ADDR_T destIp = parse_ip(config.destIp);
        const UINT32 interval = config.cycleTimeMs * 1000U;

        const TRDP_ERR_T err = tlp_publish(appHandle_, &state.handle, nullptr, nullptr, 0U, config.comId,
                                           config.etbTopoCount, config.opTrnTopoCount, srcIp, destIp, interval,
                                           config.redundancyGroup, TRDP_FLAGS_DEFAULT, nullptr, 0U);
        if (err != TRDP_NO_ERR) {
            throw std::runtime_error("tlp_publish failed for publisher '" + config.name + "' with error " +
                                     std::to_string(err));
        }

        pdPublishers_.emplace(config.name, std::move(state));
        (void) tlc_updateSession(appHandle_);
    }

    void register_pd_subscriber(const PdSubscriberConfig &config, PdHandler handler) override
    {
        auto state = std::make_unique<SubscriberState>();
        state->config = config;
        state->handler = std::move(handler);

        TRDP_SUB_T handle = nullptr;
        const TRDP_IP_ADDR_T srcIp = config.sourceIp.empty() ? VOS_INADDR_ANY : parse_ip(config.sourceIp);
        const TRDP_IP_ADDR_T destIp = parse_ip(config.destIp);
        const UINT32 timeout = config.timeoutMs == 0 ? 0U : config.timeoutMs * 1000U;

        const TRDP_ERR_T err = tlp_subscribe(appHandle_, &handle, state.get(), &RealTrdpStackAdapter::pd_callback, 0U,
                                             config.comId, config.etbTopoCount, config.opTrnTopoCount, srcIp, srcIp,
                                             destIp, TRDP_FLAGS_DEFAULT, timeout, TRDP_TO_SET_TO_ZERO);
        if (err != TRDP_NO_ERR) {
            throw std::runtime_error("tlp_subscribe failed for subscriber '" + config.name + "' with error " +
                                     std::to_string(err));
        }

        pdSubscribers_.emplace(handle, std::move(state));
        (void) tlc_updateSession(appHandle_);
    }

    void publish_pd(const std::string &publisherName, const std::vector<std::uint8_t> &data) override
    {
        auto it = pdPublishers_.find(publisherName);
        if (it == pdPublishers_.end()) {
            throw std::runtime_error("Unknown PD publisher '" + publisherName + "'");
        }

        const UINT8 *payload = data.empty() ? nullptr : data.data();
        const UINT32 payloadSize = static_cast<UINT32>(data.size());
        const TRDP_ERR_T err = tlp_put(appHandle_, it->second.handle, payload, payloadSize);
        if (err != TRDP_NO_ERR) {
            throw std::runtime_error("tlp_put failed for publisher '" + publisherName + "' with error " +
                                     std::to_string(err));
        }
    }

    void register_md_sender(const MdSenderConfig &config, MdHandler replyHandler) override
    {
        auto state = std::make_unique<MdSenderState>();
        state->config = config;
        state->replyHandler = std::move(replyHandler);
        std::memset(state->sessionId, 0, sizeof(state->sessionId));

        mdSenders_[config.name] = std::move(state);
    }

    void send_md_request(const std::string &senderName, const std::vector<std::uint8_t> &data) override
    {
        auto it = mdSenders_.find(senderName);
        if (it == mdSenders_.end()) {
            throw std::runtime_error("Unknown MD sender '" + senderName + "'");
        }

        MdSenderState *state = it->second.get();
        std::memset(state->sessionId, 0, sizeof(state->sessionId));

        const TRDP_IP_ADDR_T srcIp = parse_ip(state->config.sourceIp.empty() ? networkConfig_.hostIp : state->config.sourceIp);
        const TRDP_IP_ADDR_T destIp = parse_ip(state->config.destIp);
        const UINT32 numReplies = state->config.expectReply ? 1U : 0U;
        const UINT32 replyTimeout = state->config.replyTimeoutMs * 1000U;
        TRDP_URI_USER_T srcUri = {0};
        TRDP_URI_USER_T destUri = {0};

        const UINT8 *payload = data.empty() ? nullptr : data.data();
        const UINT32 payloadSize = static_cast<UINT32>(data.size());

        const TRDP_ERR_T err = tlm_request(appHandle_, state, &RealTrdpStackAdapter::md_reply_callback, &state->sessionId,
                                           state->config.comId, 0U, 0U, srcIp, destIp, TRDP_FLAGS_DEFAULT, numReplies,
                                           replyTimeout, &mdConfig_.sendParam, payload, payloadSize, srcUri, destUri);
        if (err != TRDP_NO_ERR) {
            throw std::runtime_error("tlm_request failed for sender '" + senderName + "' with error " +
                                     std::to_string(err));
        }
    }

    void register_md_listener(const MdListenerConfig &config, MdHandler handler) override
    {
        auto state = std::make_unique<MdListenerState>();
        state->config = config;
        state->handler = std::move(handler);
        state->handle = nullptr;

        const TRDP_IP_ADDR_T srcIp = config.sourceIp.empty() ? VOS_INADDR_ANY : parse_ip(config.sourceIp);
        const TRDP_IP_ADDR_T destIp = parse_ip(config.destIp);
        TRDP_URI_USER_T srcUri = {0};
        TRDP_URI_USER_T destUri = {0};

        const TRDP_ERR_T err = tlm_addListener(appHandle_, &state->handle, state.get(),
                                               &RealTrdpStackAdapter::md_request_callback, TRUE, config.comId, 0U, 0U,
                                               srcIp, 0U, destIp, TRDP_FLAGS_DEFAULT, srcUri, destUri);
        if (err != TRDP_NO_ERR) {
            throw std::runtime_error("tlm_addListener failed for listener '" + config.name + "' with error " +
                                     std::to_string(err));
        }

        mdListeners_[config.name] = std::move(state);
        (void) tlc_updateSession(appHandle_);
    }

    void send_md_reply(const std::string &listenerName, const MdMessage &request,
                       const std::vector<std::uint8_t> &data) override
    {
        auto it = mdListeners_.find(listenerName);
        if (it == mdListeners_.end()) {
            throw std::runtime_error("Unknown MD listener '" + listenerName + "'");
        }

        TRDP_UUID_T sessionId{};
        std::memcpy(sessionId, request.sessionId.data(), request.sessionId.size());
        TRDP_URI_USER_T srcUri = {0};
        const UINT8 *payload = data.empty() ? nullptr : data.data();
        const UINT32 payloadSize = static_cast<UINT32>(data.size());

        const TRDP_ERR_T err = tlm_reply(appHandle_, &sessionId, request.comId, 0U, &mdConfig_.sendParam, payload,
                                         payloadSize, srcUri);
        if (err != TRDP_NO_ERR) {
            throw std::runtime_error("tlm_reply failed for listener '" + listenerName + "' with error " +
                                     std::to_string(err));
        }
    }

    void poll(std::chrono::milliseconds timeout) override
    {
        if (appHandle_ == nullptr) {
            if (timeout.count() > 0) {
                vos_threadDelay(static_cast<UINT32>(timeout.count()));
            }
            return;
        }

        TRDP_FDS_T rfds;
        FD_ZERO(&rfds);
        INT32 noDesc = 0;
        TRDP_TIME_T interval{0, 0};

        const TRDP_ERR_T intervalErr = tlc_getInterval(appHandle_, &interval, &rfds, &noDesc);
        if (intervalErr != TRDP_NO_ERR) {
            interval.tv_sec = 0;
            interval.tv_usec = 0;
            noDesc = 0;
        }

        if (timeout.count() >= 0) {
            TRDP_TIME_T requested{0, 0};
            requested.tv_sec = static_cast<INT32>(timeout.count() / 1000);
            requested.tv_usec = static_cast<INT32>((timeout.count() % 1000) * 1000);
            if (vos_cmpTime(&interval, &requested) > 0) {
                interval = requested;
            }
        }

        if (noDesc <= 0) {
            const UINT32 delayMs = static_cast<UINT32>(interval.tv_sec * 1000LL) + static_cast<UINT32>(interval.tv_usec / 1000);
            if (delayMs > 0U) {
                vos_threadDelay(delayMs);
            } else if (timeout.count() > 0) {
                vos_threadDelay(static_cast<UINT32>(timeout.count()));
            }

            INT32 dummy = 0;
            (void) tlc_process(appHandle_, nullptr, &dummy);
#if MD_SUPPORT
            (void) tlm_process(appHandle_, nullptr, &dummy);
#endif
            return;
        }

        INT32 ready = vos_select(noDesc, &rfds, nullptr, nullptr, &interval);
        if (ready < 0) {
            return;
        }

        (void) tlc_process(appHandle_, &rfds, &ready);
#if MD_SUPPORT
        (void) tlm_process(appHandle_, &rfds, &ready);
#endif
    }

private:
    struct PublisherState {
        PdPublisherConfig config;
        TRDP_PUB_T handle;
    };

    struct SubscriberState {
        PdSubscriberConfig config;
        PdHandler handler;
    };

    struct MdSenderState {
        MdSenderConfig config;
        MdHandler replyHandler;
        TRDP_UUID_T sessionId;
    };

    struct MdListenerState {
        MdListenerConfig config;
        MdHandler handler;
        TRDP_LIS_T handle;
    };

    static void pd_callback(void *refCon, TRDP_APP_SESSION_T, const TRDP_PD_INFO_T *info, UINT8 *data, UINT32 dataSize)
    {
        auto *state = static_cast<SubscriberState *>(refCon);
        if (state == nullptr || !state->handler) {
            return;
        }

        PdMessage message;
        message.endpoint = fallback_endpoint(state->config.name, ip_to_string(info->srcIpAddr));
        message.comId = info->comId;
        message.sequenceCounter = info->seqCount;
        const UINT8 *payload = data;
        message.payload.assign(payload, payload + dataSize);
        state->handler(message);
    }

    static void md_reply_callback(void *refCon, TRDP_APP_SESSION_T, const TRDP_MD_INFO_T *info, UINT8 *data, UINT32 dataSize)
    {
        auto *state = static_cast<MdSenderState *>(refCon);
        if (state == nullptr || !state->replyHandler) {
            return;
        }

        if (std::memcmp(state->sessionId, info->sessionId, MdSessionIdSize) != 0) {
            return;
        }

        MdMessage message;
        message.endpoint = ip_to_string(info->srcIpAddr);
        if (message.endpoint.empty()) {
            message.endpoint = "md-reply";
        }
        message.comId = info->comId;
        message.payload.assign(data, data + dataSize);
        message.sessionId = to_session_id(info->sessionId);
        state->replyHandler(message);
    }

    static void md_request_callback(void *refCon, TRDP_APP_SESSION_T, const TRDP_MD_INFO_T *info, UINT8 *data, UINT32 dataSize)
    {
        auto *state = static_cast<MdListenerState *>(refCon);
        if (state == nullptr || !state->handler) {
            return;
        }

        MdMessage message;
        message.endpoint = fallback_endpoint(state->config.name, ip_to_string(info->srcIpAddr));
        message.comId = info->comId;
        message.payload.assign(data, data + dataSize);
        message.sessionId = to_session_id(info->sessionId);
        state->handler(message);
    }

    NetworkConfig networkConfig_;
    TRDP_APP_SESSION_T appHandle_{nullptr};
    TRDP_MEM_CONFIG_T memConfig_{};
    TRDP_PROCESS_CONFIG_T processConfig_{};
    TRDP_PD_CONFIG_T pdConfig_{};
    TRDP_MD_CONFIG_T mdConfig_{};

    std::unordered_map<std::string, PublisherState> pdPublishers_;
    std::unordered_map<TRDP_SUB_T, std::unique_ptr<SubscriberState>> pdSubscribers_;
    std::unordered_map<std::string, std::unique_ptr<MdSenderState>> mdSenders_;
    std::unordered_map<std::string, std::unique_ptr<MdListenerState>> mdListeners_;
};

}  // namespace

std::unique_ptr<TrdpStackAdapter> create_real_trdp_stack_adapter()
{
    return std::make_unique<RealTrdpStackAdapter>();
}

}  // namespace trdp_sim

#endif  // TRDPSIM_WITH_TRDP
