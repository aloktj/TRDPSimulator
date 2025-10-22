#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "trdp_simulator/config.hpp"

namespace trdp_sim {

struct PdMessage {
    std::string endpoint;
    std::uint32_t comId{0};
    std::vector<std::uint8_t> payload;
    std::uint64_t sequenceCounter{0};
};

inline constexpr std::size_t MdSessionIdSize = 16U;
using MdSessionId = std::array<std::uint8_t, MdSessionIdSize>;

struct MdMessage {
    std::string endpoint;
    std::uint32_t comId{0};
    std::vector<std::uint8_t> payload;
    MdSessionId sessionId{};
};

class TrdpStackAdapter {
public:
    using PdHandler = std::function<void(const PdMessage &)>;
    using MdHandler = std::function<void(const MdMessage &)>;

    virtual ~TrdpStackAdapter() = default;

    virtual void initialize(const NetworkConfig &networkConfig, const LoggingConfig &loggingConfig) = 0;
    virtual void shutdown() = 0;

    virtual void register_pd_publisher(const PdPublisherConfig &config) = 0;
    virtual void register_pd_subscriber(const PdSubscriberConfig &config, PdHandler handler) = 0;
    virtual void publish_pd(const std::string &publisherName, const std::vector<std::uint8_t> &data) = 0;

    virtual void register_md_sender(const MdSenderConfig &config, MdHandler replyHandler) = 0;
    virtual void send_md_request(const std::string &senderName, const std::vector<std::uint8_t> &data) = 0;

    virtual void register_md_listener(const MdListenerConfig &config, MdHandler requestHandler) = 0;
    virtual void send_md_reply(const std::string &listenerName, const MdMessage &request, const std::vector<std::uint8_t> &data) = 0;

    virtual void poll(std::chrono::milliseconds timeout) = 0;
};

std::unique_ptr<TrdpStackAdapter> create_trdp_stack_adapter();

}  // namespace trdp_sim
