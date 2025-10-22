#include "trdp_simulator/config_loader.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_set>

#include "tinyxml2.h"
#include "trdp_simulator/config.hpp"

namespace trdp_sim {
namespace {

std::string require_attribute(const tinyxml2::XMLElement &element, const char *name)
{
    const char *value = element.Attribute(name);
    if (!value) {
        throw std::runtime_error(std::string("Missing attribute '") + name + "' in element '" + element.Name() + "'");
    }
    return value;
}

std::string optional_attribute(const tinyxml2::XMLElement &element, const char *name, const std::string &fallback = "")
{
    const char *value = element.Attribute(name);
    return value ? std::string(value) : fallback;
}

std::uint32_t optional_uint_attribute(const tinyxml2::XMLElement &element, const char *name, std::uint32_t fallback = 0)
{
    const char *value = element.Attribute(name);
    if (!value) {
        return fallback;
    }
    std::uint32_t result{};
    if (element.QueryUnsignedAttribute(name, &result) != tinyxml2::XML_SUCCESS) {
        throw std::runtime_error(std::string("Invalid unsigned attribute '") + name + "' in element '" + element.Name() + "'");
    }
    return result;
}

bool optional_bool_attribute(const tinyxml2::XMLElement &element, const char *name, bool fallback = false)
{
    const char *value = element.Attribute(name);
    if (!value) {
        return fallback;
    }
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (text == "true" || text == "1" || text == "yes") {
        return true;
    }
    if (text == "false" || text == "0" || text == "no") {
        return false;
    }
    throw std::runtime_error(std::string("Invalid boolean attribute '") + name + "' in element '" + element.Name() + "'");
}

PayloadConfig load_payload_element(const tinyxml2::XMLElement &element)
{
    PayloadConfig payload;
    payload.value = element.GetText() ? std::string(element.GetText()) : std::string();
    const char *format = element.Attribute("format");
    if (format) {
        payload.format = payload_format_from_string(format);
    }
    return payload;
}

PdPublisherConfig load_pd_publisher(const tinyxml2::XMLElement &element)
{
    PdPublisherConfig config;
    config.name = require_attribute(element, "name");
    config.comId = optional_uint_attribute(element, "comId");
    config.datasetId = optional_uint_attribute(element, "datasetId");
    config.etbTopoCount = static_cast<std::uint16_t>(optional_uint_attribute(element, "etbTopoCount"));
    config.opTrnTopoCount = static_cast<std::uint16_t>(optional_uint_attribute(element, "opTrnTopoCount"));
    config.sourceIp = optional_attribute(element, "sourceIp");
    config.destIp = optional_attribute(element, "destIp");
    config.cycleTimeMs = optional_uint_attribute(element, "cycleTimeMs", 1000);
    config.redundancyGroup = optional_uint_attribute(element, "redundancyGroup");
    config.useSequenceCounter = optional_bool_attribute(element, "useSequenceCounter");
    const auto *payloadElement = element.FirstChildElement("payload");
    if (payloadElement) {
        config.payload = load_payload_element(*payloadElement);
    }
    return config;
}

PdSubscriberConfig load_pd_subscriber(const tinyxml2::XMLElement &element)
{
    PdSubscriberConfig config;
    config.name = require_attribute(element, "name");
    config.comId = optional_uint_attribute(element, "comId");
    config.etbTopoCount = static_cast<std::uint16_t>(optional_uint_attribute(element, "etbTopoCount"));
    config.opTrnTopoCount = static_cast<std::uint16_t>(optional_uint_attribute(element, "opTrnTopoCount"));
    config.sourceIp = optional_attribute(element, "sourceIp");
    config.destIp = optional_attribute(element, "destIp");
    config.timeoutMs = optional_uint_attribute(element, "timeoutMs");
    config.enableComIdFiltering = optional_bool_attribute(element, "comIdFilter", true);
    return config;
}

MdSenderConfig load_md_sender(const tinyxml2::XMLElement &element)
{
    MdSenderConfig config;
    config.name = require_attribute(element, "name");
    config.comId = optional_uint_attribute(element, "comId");
    config.replyComId = optional_uint_attribute(element, "replyComId");
    config.sourceIp = optional_attribute(element, "sourceIp");
    config.destIp = optional_attribute(element, "destIp");
    config.cycleTimeMs = optional_uint_attribute(element, "cycleTimeMs");
    config.replyTimeoutMs = optional_uint_attribute(element, "replyTimeoutMs", 1000);
    config.expectReply = optional_bool_attribute(element, "expectReply");
    const auto *payloadElement = element.FirstChildElement("payload");
    if (payloadElement) {
        config.payload = load_payload_element(*payloadElement);
    }
    return config;
}

MdListenerConfig load_md_listener(const tinyxml2::XMLElement &element)
{
    MdListenerConfig config;
    config.name = require_attribute(element, "name");
    config.comId = optional_uint_attribute(element, "comId");
    config.sourceIp = optional_attribute(element, "sourceIp");
    config.destIp = optional_attribute(element, "destIp");
    config.autoReply = optional_bool_attribute(element, "autoReply");
    const auto *payloadElement = element.FirstChildElement("replyPayload");
    if (payloadElement) {
        config.replyPayload = load_payload_element(*payloadElement);
    }
    return config;
}

LogLevel parse_log_level(const std::string &value)
{
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lowered == "error") return LogLevel::Error;
    if (lowered == "warn" || lowered == "warning") return LogLevel::Warn;
    if (lowered == "info") return LogLevel::Info;
    if (lowered == "debug") return LogLevel::Debug;
    throw std::runtime_error("Invalid log level: " + value);
}

SimulatorConfig load_configuration_from_document(tinyxml2::XMLDocument &doc)
{
    const auto *root = doc.RootElement();
    if (!root || std::string(root->Name()) != "trdpSimulator") {
        throw std::runtime_error("Root element <trdpSimulator> not found");
    }

    SimulatorConfig config;

    if (const auto *networkElement = root->FirstChildElement("network")) {
        config.network.interfaceName = optional_attribute(*networkElement, "interface", "eth0");
        config.network.hostIp = optional_attribute(*networkElement, "hostIp");
        config.network.gatewayIp = optional_attribute(*networkElement, "gateway");
        config.network.vlanId = static_cast<std::uint16_t>(optional_uint_attribute(*networkElement, "vlanId"));
        config.network.ttl = static_cast<std::uint8_t>(optional_uint_attribute(*networkElement, "ttl", 64));
    }

    if (const auto *loggingElement = root->FirstChildElement("logging")) {
        config.logging.enableConsole = optional_bool_attribute(*loggingElement, "console", true);
        config.logging.filePath = optional_attribute(*loggingElement, "file");
        if (const char *level = loggingElement->Attribute("level")) {
            config.logging.level = parse_log_level(level);
        }
    }

    if (const auto *pdElement = root->FirstChildElement("pd")) {
        for (auto *publisher = pdElement->FirstChildElement("publisher"); publisher; publisher = publisher->NextSiblingElement("publisher")) {
            config.pdPublishers.emplace_back(load_pd_publisher(*publisher));
        }
        for (auto *subscriber = pdElement->FirstChildElement("subscriber"); subscriber; subscriber = subscriber->NextSiblingElement("subscriber")) {
            config.pdSubscribers.emplace_back(load_pd_subscriber(*subscriber));
        }
    }

    if (const auto *mdElement = root->FirstChildElement("md")) {
        for (auto *sender = mdElement->FirstChildElement("sender"); sender; sender = sender->NextSiblingElement("sender")) {
            config.mdSenders.emplace_back(load_md_sender(*sender));
        }
        for (auto *listener = mdElement->FirstChildElement("listener"); listener; listener = listener->NextSiblingElement("listener")) {
            config.mdListeners.emplace_back(load_md_listener(*listener));
        }
    }

    return config;
}

}  // namespace

void validate_configuration(const SimulatorConfig &config)
{
    if (config.network.interfaceName.empty()) {
        throw std::runtime_error("Network interface name must not be empty");
    }

    auto ensure_unique = [](const auto &items, const char *kind) {
        std::unordered_set<std::string> names;
        for (const auto &item : items) {
            if (!names.insert(item.name).second) {
                throw std::runtime_error(std::string("Duplicate ") + kind + " name '" + item.name + "'");
            }
        }
    };

    ensure_unique(config.pdPublishers, "PD publisher");
    ensure_unique(config.pdSubscribers, "PD subscriber");
    ensure_unique(config.mdSenders, "MD sender");
    ensure_unique(config.mdListeners, "MD listener");

    for (const auto &publisher : config.pdPublishers) {
        if (publisher.cycleTimeMs == 0) {
            throw std::runtime_error("PD publisher '" + publisher.name + "' must specify cycleTimeMs > 0");
        }
    }

    for (const auto &sender : config.mdSenders) {
        if (sender.expectReply && sender.replyTimeoutMs == 0) {
            throw std::runtime_error("MD sender '" + sender.name + "' expects a reply but replyTimeoutMs is 0");
        }
    }

    for (const auto &listener : config.mdListeners) {
        if (listener.autoReply && listener.replyPayload.value.empty()) {
            throw std::runtime_error("MD listener '" + listener.name + "' autoReply requires a replyPayload");
        }
    }
}

SimulatorConfig load_configuration(const std::string &path)
{
    tinyxml2::XMLDocument doc;
    const auto result = doc.LoadFile(path.c_str());
    if (result != tinyxml2::XML_SUCCESS) {
        throw std::runtime_error("Failed to load configuration XML: " + std::string(doc.ErrorStr() ? doc.ErrorStr() : "unknown error"));
    }

    SimulatorConfig config = load_configuration_from_document(doc);
    validate_configuration(config);
    return config;
}

SimulatorConfig load_configuration_from_string(const std::string &xml)
{
    tinyxml2::XMLDocument doc;
    const auto result = doc.Parse(xml.c_str(), xml.size());
    if (result != tinyxml2::XML_SUCCESS) {
        throw std::runtime_error("Failed to parse configuration XML: " + std::string(doc.ErrorStr() ? doc.ErrorStr() : "unknown error"));
    }

    SimulatorConfig config = load_configuration_from_document(doc);
    validate_configuration(config);
    return config;
}

}  // namespace trdp_sim
