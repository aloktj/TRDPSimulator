#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "trdp_simulator/logger.hpp"

namespace trdp_sim {

struct NetworkConfig {
    std::string interfaceName;
    std::string hostIp;
    std::string gatewayIp;
    std::uint16_t vlanId{0};
    std::uint8_t ttl{64};
};

struct LoggingConfig {
    bool enableConsole{true};
    std::string filePath;
    LogLevel level{LogLevel::Info};
};

struct PayloadConfig {
    enum class Format {
        Hex,
        Text,
        File
    };

    Format format{Format::Hex};
    std::string value;
};

PayloadConfig::Format payload_format_from_string(const std::string &value);
std::string payload_format_to_string(PayloadConfig::Format format);

struct PdPublisherConfig {
    std::string name;
    std::uint32_t comId{0};
    std::uint32_t datasetId{0};
    std::uint16_t etbTopoCount{0};
    std::uint16_t opTrnTopoCount{0};
    std::string sourceIp;
    std::string destIp;
    std::uint32_t cycleTimeMs{1000};
    std::uint32_t redundancyGroup{0};
    bool useSequenceCounter{false};
    PayloadConfig payload;
};

struct PdSubscriberConfig {
    std::string name;
    std::uint32_t comId{0};
    std::uint16_t etbTopoCount{0};
    std::uint16_t opTrnTopoCount{0};
    std::string sourceIp;
    std::string destIp;
    std::uint32_t timeoutMs{0};
    bool enableComIdFiltering{true};
};

struct MdSenderConfig {
    std::string name;
    std::uint32_t comId{0};
    std::uint32_t replyComId{0};
    std::string sourceIp;
    std::string destIp;
    std::uint32_t cycleTimeMs{0};
    std::uint32_t replyTimeoutMs{1000};
    bool expectReply{false};
    PayloadConfig payload;
};

struct MdListenerConfig {
    std::string name;
    std::uint32_t comId{0};
    std::string sourceIp;
    std::string destIp;
    bool autoReply{false};
    PayloadConfig replyPayload;
};

struct SimulatorConfig {
    NetworkConfig network;
    LoggingConfig logging;
    std::vector<PdPublisherConfig> pdPublishers;
    std::vector<PdSubscriberConfig> pdSubscribers;
    std::vector<MdSenderConfig> mdSenders;
    std::vector<MdListenerConfig> mdListeners;
};

std::vector<std::uint8_t> load_payload(const PayloadConfig &payload);

}  // namespace trdp_sim
