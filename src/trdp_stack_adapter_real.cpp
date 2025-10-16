#ifdef TRDPSIM_WITH_TRDP

#include "trdp_simulator/trdp_stack_adapter.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

extern "C" {
#include <trdp_if_light.h>
#include <trdp_mdcom.h>
}

namespace trdp_sim {
namespace {

TRDP_IP_ADDR_T parse_ip(const std::string &ip)
{
    if (ip.empty()) {
        return 0U;
    }
    const auto addr = inet_addr(ip.c_str());
    if (addr == INADDR_NONE) {
        throw std::runtime_error("Invalid IP address: " + ip);
    }
    return static_cast<TRDP_IP_ADDR_T>(addr);
}

TRDP_FLAGS_T publisher_flags(const PdPublisherConfig &config)
{
    TRDP_FLAGS_T flags = TRDP_FLAGS_DEFAULT;
    if (config.useSequenceCounter) {
        flags |= TRDP_FLAGS_FORCE_SEQ_CNT;
    }
    return flags;
}

class RealTrdpStackAdapter : public TrdpStackAdapter {
public:
    RealTrdpStackAdapter()
    {
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

        pdMemory_.resize(TRDP_PD_DEFAULT_MEM_SIZE);
        mdMemory_.resize(TRDP_MD_DEFAULT_MEM_SIZE);

        TRDP_DBG_CONFIG_T dbgConfig{};
        dbgConfig.pfLogFunction = nullptr;
        dbgConfig.pLogContext = nullptr;
        dbgConfig.logLevel = TRDP_DBG_MASK_APP | TRDP_DBG_MASK_CON | TRDP_DBG_MASK_COM | TRDP_DBG_MASK_USR;
        dbgConfig.option = 0U;

        TRDP_MEM_CONFIG_T memConfig{};
        memConfig.pdMemSize = static_cast<UINT32>(pdMemory_.size());
        memConfig.pPdMem = pdMemory_.data();
        memConfig.mdMemSize = static_cast<UINT32>(mdMemory_.size());
        memConfig.pMdMem = mdMemory_.data();

        processConfig_.pProcessName = const_cast<char *>(networkConfig.interfaceName.c_str());
        processConfig_.hostName = const_cast<char *>(networkConfig.interfaceName.c_str());
        processConfig_.cycleTime = 0U;
        processConfig_.priority = 0U;
        processConfig_.stackSize = 0U;
        processConfig_.options = TRDP_OPTION_BLOCK;

        const TRDP_ERR_T errInit = tlc_init(&appHandle_, &memConfig, &dbgConfig, &processConfig_);
        if (errInit != TRDP_NO_ERR) {
            throw std::runtime_error("tlc_init failed with error " + std::to_string(errInit));
        }

        TRDP_IF_CONFIG_T ifConfig{};
        ifConfig.ifaceName = const_cast<char *>(networkConfig.interfaceName.c_str());
        ifConfig.ownIP = parse_ip(networkConfig.hostIp);
        ifConfig.mcGroup = 0U;
        ifConfig.port = 0U;
        ifConfig.vlanId = networkConfig.vlanId;
        ifConfig.ttl = networkConfig.ttl;

        const TRDP_ERR_T errSession = tlc_openSession(appHandle_, &appHandle_, 0U, 0U, 0U, 0U, &ifConfig, 0U, 0U, &RealTrdpStackAdapter::pd_callback, this);
        if (errSession != TRDP_NO_ERR) {
            throw std::runtime_error("tlc_openSession failed with error " + std::to_string(errSession));
        }

        TRDP_ERR_T errMdInit = tlm_init(&mdHandle_, appHandle_, &dbgConfig, nullptr);
        if (errMdInit != TRDP_NO_ERR) {
            throw std::runtime_error("tlm_init failed with error " + std::to_string(errMdInit));
        }
    }

    void shutdown() override
    {
        for (auto &entry : pdPublishers_) {
            tlc_unpublish(appHandle_, entry.second.handle);
        }
        pdPublishers_.clear();

        for (auto &entry : pdSubscribers_) {
            tlc_unsubscribe(appHandle_, entry.first);
        }
        pdSubscribers_.clear();

        for (auto &entry : mdListeners_) {
            tlm_unsubscribe(mdHandle_, entry.second.handle);
        }
        mdListeners_.clear();

        if (mdHandle_ != nullptr) {
            tlm_terminate(mdHandle_);
            mdHandle_ = nullptr;
        }
        if (appHandle_ != nullptr) {
            tlc_closeSession(appHandle_);
            tlc_terminate(&appHandle_);
            appHandle_ = nullptr;
        }
    }

    void register_pd_publisher(const PdPublisherConfig &config) override
    {
        PublisherState state;
        state.config = config;

        const auto srcIp = parse_ip(config.sourceIp.empty() ? networkConfig_.hostIp : config.sourceIp);
        const auto destIp = parse_ip(config.destIp);

        const TRDP_ERR_T err = tlc_publish(appHandle_, &state.handle, config.comId, config.etbTopoCount, config.opTrnTopoCount,
                                           srcIp, destIp, networkConfig_.ttl, config.datasetId,
                                           nullptr, 0U, config.cycleTimeMs * 1000U, config.redundancyGroup,
                                           publisher_flags(config), nullptr, nullptr);
        if (err != TRDP_NO_ERR) {
            throw std::runtime_error("tlc_publish failed for publisher '" + config.name + "' with error " + std::to_string(err));
        }

        pdPublishers_.emplace(config.name, std::move(state));
    }

    void register_pd_subscriber(const PdSubscriberConfig &config, PdHandler handler) override
    {
        TRDP_SUB_T subHandle{};
        const auto srcIp = parse_ip(config.sourceIp);
        const auto destIp = parse_ip(config.destIp);
        const TRDP_ERR_T err = tlc_subscribe(appHandle_, &subHandle, config.comId, config.etbTopoCount, config.opTrnTopoCount,
                                             srcIp, destIp, 0U, config.timeoutMs, TRDP_FLAGS_DEFAULT,
                                             &RealTrdpStackAdapter::pd_callback, this);
        if (err != TRDP_NO_ERR) {
            throw std::runtime_error("tlc_subscribe failed for subscriber '" + config.name + "' with error " + std::to_string(err));
        }
        pdSubscribers_[subHandle] = {config, std::move(handler)};
    }

    void publish_pd(const std::string &publisherName, const std::vector<std::uint8_t> &data) override
    {
        auto it = pdPublishers_.find(publisherName);
        if (it == pdPublishers_.end()) {
            throw std::runtime_error("Unknown PD publisher '" + publisherName + "'");
        }
        const TRDP_ERR_T err = tlc_update(appHandle_, it->second.handle, data.data(), static_cast<UINT32>(data.size()));
        if (err != TRDP_NO_ERR) {
            throw std::runtime_error("tlc_update failed for publisher '" + publisherName + "' with error " + std::to_string(err));
        }
    }

    void register_md_sender(const MdSenderConfig &config, MdHandler handler) override
    {
        MdSenderState state;
        state.config = config;
        state.replyHandler = std::move(handler);
        mdSenders_.emplace(config.name, std::move(state));
    }

    void send_md_request(const std::string &senderName, const std::vector<std::uint8_t> &data) override
    {
        auto it = mdSenders_.find(senderName);
        if (it == mdSenders_.end()) {
            throw std::runtime_error("Unknown MD sender '" + senderName + "'");
        }
        MdSenderState &state = it->second;
        TRDP_MD_CALLBACK_T callback{&RealTrdpStackAdapter::md_reply_callback, this};
        const auto srcIp = parse_ip(state.config.sourceIp.empty() ? networkConfig_.hostIp : state.config.sourceIp);
        const auto destIp = parse_ip(state.config.destIp);
        TRDP_ERR_T err = tlm_request(mdHandle_, &state.session, state.config.comId, state.config.replyComId, srcIp, destIp,
                                     networkConfig_.ttl, state.config.replyTimeoutMs, data.data(), static_cast<UINT32>(data.size()),
                                     0U, TRDP_FLAGS_DEFAULT, &callback);
        if (err != TRDP_NO_ERR) {
            throw std::runtime_error("tlm_request failed for sender '" + senderName + "' with error " + std::to_string(err));
        }
    }

    void register_md_listener(const MdListenerConfig &config, MdHandler handler) override
    {
        MdListenerState state;
        state.config = config;
        state.handler = std::move(handler);

        TRDP_ERR_T err = tlm_subscribe(mdHandle_, &state.handle, config.comId, parse_ip(config.destIp), parse_ip(config.sourceIp),
                                       TRDP_FLAGS_DEFAULT, &RealTrdpStackAdapter::md_request_callback, this);
        if (err != TRDP_NO_ERR) {
            throw std::runtime_error("tlm_subscribe failed for listener '" + config.name + "' with error " + std::to_string(err));
        }

        mdListeners_.emplace(config.name, std::move(state));
    }

    void send_md_reply(const std::string &listenerName, const MdMessage &request, const std::vector<std::uint8_t> &data) override
    {
        auto it = mdListeners_.find(listenerName);
        if (it == mdListeners_.end()) {
            throw std::runtime_error("Unknown MD listener '" + listenerName + "'");
        }
        MdListenerState &state = it->second;
        const auto srcIp = parse_ip(state.config.sourceIp.empty() ? networkConfig_.hostIp : state.config.sourceIp);
        const auto destIp = parse_ip(request.endpoint);
        const TRDP_ERR_T err = tlm_reply(mdHandle_, request.sessionId, state.config.comId, srcIp, destIp,
                                         networkConfig_.ttl, data.data(), static_cast<UINT32>(data.size()), TRDP_FLAGS_DEFAULT);
        if (err != TRDP_NO_ERR) {
            throw std::runtime_error("tlm_reply failed for listener '" + listenerName + "' with error " + std::to_string(err));
        }
    }

    void poll(std::chrono::milliseconds timeout) override
    {
        TRDP_TIME_T interval;
        interval.tv_sec = static_cast<UINT32>(timeout.count() / 1000);
        interval.tv_nsec = static_cast<UINT32>((timeout.count() % 1000) * 1000000U);
        tlc_process(appHandle_, &interval, nullptr);
        tlm_process(mdHandle_);
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
        TRDP_SESSION_T session;
    };

    struct MdListenerState {
        MdListenerConfig config;
        MdHandler handler;
        TRDP_MD_HANDLE_T handle;
    };

    static void pd_callback(TRDP_APP_SESSION_T session, TRDP_SUB_T subHandle, const TRDP_PD_INFO_T *info, const UINT8 *data, UINT32 dataSize)
    {
        auto *self = static_cast<RealTrdpStackAdapter *>(tlc_getSessionUserData(session));
        if (!self) {
            return;
        }
        self->handle_pd(subHandle, info, data, dataSize);
    }

    static void md_reply_callback(TRDP_MD_INFO_T *info, const UINT8 *data, UINT32 dataSize, void *refCon)
    {
        auto *self = static_cast<RealTrdpStackAdapter *>(refCon);
        if (!self) {
            return;
        }
        self->handle_md_reply(info, data, dataSize);
    }

    static void md_request_callback(TRDP_MD_INFO_T *info, const UINT8 *data, UINT32 dataSize, void *refCon)
    {
        auto *self = static_cast<RealTrdpStackAdapter *>(refCon);
        if (!self) {
            return;
        }
        self->handle_md_request(info, data, dataSize);
    }

    void handle_pd(TRDP_SUB_T subHandle, const TRDP_PD_INFO_T *info, const UINT8 *data, UINT32 dataSize)
    {
        auto it = pdSubscribers_.find(subHandle);
        if (it == pdSubscribers_.end()) {
            return;
        }
        PdMessage message;
        message.endpoint = it->second.config.name;
        message.comId = info->comId;
        message.sequenceCounter = info->seqCount;
        message.payload.assign(data, data + dataSize);
        it->second.handler(message);
    }

    void handle_md_reply(TRDP_MD_INFO_T *info, const UINT8 *data, UINT32 dataSize)
    {
        for (auto &entry : mdSenders_) {
            if (entry.second.session == info->sessionId && entry.second.replyHandler) {
                MdMessage message;
                message.endpoint = inet_ntoa(*(in_addr *)&info->srcIP);
                message.comId = info->comId;
                message.sessionId = info->sessionId;
                message.payload.assign(data, data + dataSize);
                entry.second.replyHandler(message);
            }
        }
    }

    void handle_md_request(TRDP_MD_INFO_T *info, const UINT8 *data, UINT32 dataSize)
    {
        for (auto &entry : mdListeners_) {
            if (entry.second.config.comId == info->comId) {
                MdMessage message;
                message.endpoint = inet_ntoa(*(in_addr *)&info->srcIP);
                message.comId = info->comId;
                message.sessionId = info->sessionId;
                message.payload.assign(data, data + dataSize);
                if (entry.second.handler) {
                    entry.second.handler(message);
                }
            }
        }
    }

    NetworkConfig networkConfig_;
    TRDP_APP_SESSION_T appHandle_{nullptr};
    TRDP_APP_SESSION_T mdHandle_{nullptr};
    TRDP_PROCESS_CONFIG_T processConfig_{};
    TRDP_PD_CONFIG_T pdConfig_{};
    TRDP_MD_CONFIG_T mdConfig_{};

    std::unordered_map<std::string, PublisherState> pdPublishers_;
    std::unordered_map<TRDP_SUB_T, SubscriberState> pdSubscribers_;
    std::unordered_map<std::string, MdSenderState> mdSenders_;
    std::unordered_map<std::string, MdListenerState> mdListeners_;

    std::vector<UINT8> pdMemory_;
    std::vector<UINT8> mdMemory_;
};

}  // namespace

std::unique_ptr<TrdpStackAdapter> create_real_trdp_stack_adapter()
{
    return std::make_unique<RealTrdpStackAdapter>();
}

}  // namespace trdp_sim

#endif  // TRDPSIM_WITH_TRDP
