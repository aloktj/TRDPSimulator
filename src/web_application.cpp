#include "trdp_simulator/web_application.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "trdp_simulator/config_loader.hpp"
#include "trdp_simulator/logger.hpp"
#include "trdp_simulator/simulator.hpp"
#include "trdp_simulator/trdp_stack_adapter.hpp"

namespace trdp_sim {

namespace {
std::string status_message_for(int code)
{
    switch (code) {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 409:
        return "Conflict";
    case 500:
        return "Internal Server Error";
    default:
        return "Unknown";
    }
}
}

WebApplication::WebApplication(std::string host, unsigned short port)
    : host_(std::move(host)),
      port_(port),
      config_store_(std::filesystem::path("config/library"))
{
}

WebApplication::~WebApplication()
{
    request_stop();
    if (simulator_thread_.joinable()) {
        simulator_thread_.join();
    }
}

void WebApplication::run()
{
    stop_requested_.store(false);

    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        throw std::runtime_error("Failed to create socket");
    }

    int enable_reuse = 1;
    if (::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &enable_reuse, sizeof(enable_reuse)) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        throw std::runtime_error("Failed to set socket options");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    if (host_.empty() || host_ == "0.0.0.0" || host_ == "*") {
        address.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (::inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1) {
            ::close(server_fd_);
            server_fd_ = -1;
            throw std::runtime_error("Invalid listen address: " + host_);
        }
    }

    if (::bind(server_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        int err = errno;
        ::close(server_fd_);
        server_fd_ = -1;
        throw std::runtime_error("Failed to bind socket: " + std::string(std::strerror(err)));
    }

    if (::listen(server_fd_, 8) < 0) {
        int err = errno;
        ::close(server_fd_);
        server_fd_ = -1;
        throw std::runtime_error("Failed to listen on socket: " + std::string(std::strerror(err)));
    }

    accept_loop();
}

void WebApplication::accept_loop()
{
    while (!stop_requested_.load()) {
        sockaddr_in client_address{};
        socklen_t client_length = sizeof(client_address);
        int client_fd = ::accept(server_fd_, reinterpret_cast<sockaddr *>(&client_address), &client_length);
        if (client_fd < 0) {
            if (stop_requested_.load()) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            continue;
        }

        handle_client(client_fd);
        ::close(client_fd);
    }

    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
}

void WebApplication::handle_client(int client_fd)
{
    std::string request;
    char buffer[4096];
    ssize_t bytes_read = 0;
    size_t header_end = std::string::npos;
    size_t expected_length = 0;

    while (true) {
        bytes_read = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (bytes_read <= 0) {
            break;
        }
        request.append(buffer, static_cast<std::size_t>(bytes_read));
        if (header_end == std::string::npos) {
            header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string header_part = request.substr(0, header_end);
                std::istringstream header_stream(header_part);
                std::string line;
                while (std::getline(header_stream, line)) {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    std::string key, value;
                    auto colon_pos = line.find(':');
                    if (colon_pos == std::string::npos) {
                        continue;
                    }
                    key = line.substr(0, colon_pos);
                    value = line.substr(colon_pos + 1);
                    while (!value.empty() && value.front() == ' ') {
                        value.erase(value.begin());
                    }
                    if (key == "Content-Length") {
                        try {
                            expected_length = static_cast<std::size_t>(std::stoul(value));
                        } catch (...) {
                            expected_length = 0;
                        }
                    }
                }
                expected_length += header_end + 4;
            }
        }
        if (header_end != std::string::npos && request.size() >= expected_length) {
            break;
        }
    }

    if (request.empty()) {
        return;
    }

    std::istringstream request_stream(request);
    std::string request_line;
    std::getline(request_stream, request_line);
    if (!request_line.empty() && request_line.back() == '\r') {
        request_line.pop_back();
    }

    std::istringstream line_stream(request_line);
    std::string method;
    std::string target;
    std::string http_version;
    line_stream >> method >> target >> http_version;

    std::string body;
    if (header_end != std::string::npos) {
        body = request.substr(header_end + 4);
    }

    HttpResponse response = handle_request(method, target, body);
    if (response.status_message.empty()) {
        response.status_message = status_message_for(response.status_code);
    }

    std::ostringstream response_stream;
    response_stream << "HTTP/1.1 " << response.status_code << ' ' << response.status_message << "\r\n";
    response_stream << "Connection: close\r\n";
    response_stream << "Content-Type: " << response.content_type << "\r\n";
    response_stream << "Content-Length: " << response.body.size() << "\r\n\r\n";
    response_stream << response.body;

    auto response_text = response_stream.str();
    ::send(client_fd, response_text.c_str(), response_text.size(), 0);
}

WebApplication::HttpResponse WebApplication::handle_request(const std::string &method,
                                                            const std::string &target,
                                                            const std::string &body)
{
    auto query_pos = target.find('?');
    std::string path = target.substr(0, query_pos);
    std::string query = query_pos == std::string::npos ? std::string() : target.substr(query_pos + 1);

    if (path == "/") {
        return {200, "OK", "text/html; charset=utf-8", main_page_html()};
    }

    if (path == "/api/status") {
        return respond_json(200, build_status_json());
    }

    if (path == "/api/metrics") {
        return respond_json(200, build_metrics_json());
    }

    if (path == "/api/configs") {
        std::ostringstream stream;
        stream << "{\"configs\":[";
        auto configs = config_store_.list();
        for (std::size_t i = 0; i < configs.size(); ++i) {
            if (i != 0) {
                stream << ',';
            }
            stream << "{\"name\":\"" << json_escape(configs[i]) << "\"}";
        }
        stream << "]}";
        return respond_json(200, stream.str());
    }

    if (path == "/api/config/parse" && method == "POST") {
        auto params = parse_form_urlencoded(body);
        auto xml_it = params.find("xml");
        if (xml_it == params.end() || xml_it->second.empty()) {
            return respond_json(400, "{\"error\":\"Missing xml parameter\"}");
        }
        try {
            auto config = load_configuration_from_string(xml_it->second);
            std::ostringstream stream;
            stream << "{\"summary\":" << build_config_summary_json(config);
            auto name_it = params.find("name");
            if (name_it != params.end()) {
                stream << ",\"suggestedName\":\"" << json_escape(name_it->second) << "\"";
            }
            stream << "}";
            return respond_json(200, stream.str());
        } catch (const std::exception &ex) {
            return respond_json(400, "{\"error\":\"" + json_escape(ex.what()) + "\"}");
        }
    }

    if (path == "/api/config/save" && method == "POST") {
        auto params = parse_form_urlencoded(body);
        auto name_it = params.find("name");
        auto xml_it = params.find("xml");
        if (name_it == params.end() || name_it->second.empty()) {
            return respond_json(400, "{\"error\":\"Missing name parameter\"}");
        }
        if (xml_it == params.end() || xml_it->second.empty()) {
            return respond_json(400, "{\"error\":\"Missing xml parameter\"}");
        }
        const std::string &name = name_it->second;
        if (!ConfigStore::is_valid_name(name)) {
            return respond_json(400, "{\"error\":\"Invalid configuration name\"}");
        }
        try {
            auto config = load_configuration_from_string(xml_it->second);
            (void)config;
            bool replaced = config_store_.exists(name);
            config_store_.save(name, xml_it->second);
            std::ostringstream stream;
            stream << "{\"message\":\"Configuration saved\",\"name\":\"" << json_escape(name)
                   << "\",\"replaced\":" << (replaced ? "true" : "false") << "}";
            return respond_json(200, stream.str());
        } catch (const std::exception &ex) {
            return respond_json(400, "{\"error\":\"" + json_escape(ex.what()) + "\"}");
        }
    }

    if (path == "/api/config/details") {
        std::string name = extract_parameter(query, "name");
        if (name.empty() && method == "POST") {
            auto params = parse_form_urlencoded(body);
            auto it = params.find("name");
            if (it != params.end()) {
                name = it->second;
            }
        }
        if (name.empty()) {
            return respond_json(400, "{\"error\":\"Missing name parameter\"}");
        }
        if (!ConfigStore::is_valid_name(name)) {
            return respond_json(400, "{\"error\":\"Invalid configuration name\"}");
        }
        try {
            auto xml = config_store_.load_xml(name);
            auto config = load_configuration_from_string(xml);
            std::ostringstream stream;
            stream << "{\"name\":\"" << json_escape(name) << "\",\"summary\":"
                   << build_config_summary_json(config) << ",\"xml\":\"" << json_escape(xml) << "\"}";
            return respond_json(200, stream.str());
        } catch (const std::exception &ex) {
            return respond_json(404, "{\"error\":\"" + json_escape(ex.what()) + "\"}");
        }
    }

    if (path == "/api/simulator/payloads") {
        return respond_json(200, build_payloads_json());
    }

    if (path == "/api/simulator/payload" && method == "POST") {
        auto params = parse_form_urlencoded(body);
        const auto type_it = params.find("type");
        const auto name_it = params.find("name");
        const auto format_it = params.find("format");
        const auto value_it = params.find("value");
        if (type_it == params.end() || name_it == params.end() || format_it == params.end() || value_it == params.end()) {
            return respond_json(400, "{\"error\":\"Missing required parameters\"}");
        }

        std::shared_ptr<Simulator> simulator;
        {
            std::lock_guard<std::mutex> lock(simulator_mutex_);
            simulator = active_simulator_;
        }
        if (!simulator) {
            return respond_json(409, "{\"error\":\"Simulator is not running\"}");
        }

        std::string error;
        try {
            const auto format = payload_format_from_string(format_it->second);
            bool ok = false;
            if (type_it->second == "pd") {
                ok = simulator->set_pd_payload(name_it->second, format, value_it->second, error);
            } else if (type_it->second == "md") {
                ok = simulator->set_md_payload(name_it->second, format, value_it->second, error);
            } else {
                return respond_json(400, "{\"error\":\"Unsupported payload type\"}");
            }
            if (!ok) {
                return respond_json(409, "{\"error\":\"" + json_escape(error) + "\"}");
            }
            return respond_json(200, "{\"message\":\"Payload updated\"}");
        } catch (const std::exception &ex) {
            return respond_json(400, "{\"error\":\"" + json_escape(ex.what()) + "\"}");
        }
    }

    if (path == "/api/start") {
        std::string config_spec = extract_parameter(query, "config");
        if (config_spec.empty() && method == "POST") {
            config_spec = extract_parameter(body, "config");
        }
        if (config_spec.empty()) {
            return respond_json(400, "{\"error\":\"Missing config parameter\"}");
        }
        std::string path_to_use = config_spec;
        std::string label = config_spec;
        if (config_spec.rfind("saved:", 0) == 0) {
            std::string name = config_spec.substr(6);
            if (!ConfigStore::is_valid_name(name) || !config_store_.exists(name)) {
                return respond_json(404, "{\"error\":\"Saved configuration not found\"}");
            }
            path_to_use = config_store_.path_for(name).string();
            label = name;
        } else if (!std::filesystem::exists(path_to_use)) {
            if (ConfigStore::is_valid_name(config_spec) && config_store_.exists(config_spec)) {
                path_to_use = config_store_.path_for(config_spec).string();
                label = config_spec;
            }
        }

        std::string message;
        if (start_simulator(path_to_use, label, message)) {
            std::ostringstream stream;
            stream << "{\"message\":\"" << json_escape(message) << "\",\"config\":\""
                   << json_escape(label) << "\"}";
            return respond_json(202, stream.str());
        }
        return respond_json(409, "{\"error\":\"" + json_escape(message) + "\"}");
    }

    if (path == "/api/stop") {
        std::string message;
        if (stop_simulator(message)) {
            return respond_json(200, "{\"message\":\"" + json_escape(message) + "\"}");
        }
        return respond_json(409, "{\"error\":\"" + json_escape(message) + "\"}");
    }

    return respond_json(404, "{\"error\":\"Not found\"}");
}

bool WebApplication::start_simulator(const std::string &config_path, const std::string &config_label, std::string &message)
{
    std::unique_lock<std::mutex> lock(simulator_mutex_);
    if (simulator_running_ || simulator_start_pending_) {
        message = "Simulator already running";
        return false;
    }

    if (simulator_thread_.joinable()) {
        lock.unlock();
        simulator_thread_.join();
        lock.lock();
    }

    simulator_start_pending_ = true;
    last_error_.reset();
    current_config_.clear();
    current_config_label_.clear();
    pending_config_label_ = config_label;
    has_metrics_snapshot_ = false;
    last_metrics_snapshot_ = RuntimeMetrics::Snapshot{};

    simulator_thread_ = std::thread(&WebApplication::simulator_worker, this, config_path);

    simulator_cv_.wait(lock, [this]() { return !simulator_start_pending_; });

    if (simulator_running_) {
        message = "Simulator started";
        return true;
    }

    pending_config_label_.clear();
    message = last_error_.value_or("Failed to start simulator");
    return false;
}

bool WebApplication::stop_simulator(std::string &message)
{
    std::shared_ptr<Simulator> simulator;
    {
        std::unique_lock<std::mutex> lock(simulator_mutex_);
        if (!simulator_running_ && !simulator_start_pending_) {
            if (simulator_thread_.joinable()) {
                lock.unlock();
                simulator_thread_.join();
                lock.lock();
            }
            message = "Simulator is not running";
            return false;
        }
        simulator = active_simulator_;
    }

    RuntimeMetrics::Snapshot snapshot;
    bool have_snapshot = false;
    if (simulator) {
        simulator->stop();
        snapshot = simulator->metrics_snapshot();
        have_snapshot = true;
    }

    if (simulator_thread_.joinable()) {
        simulator_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(simulator_mutex_);
        simulator_running_ = false;
        simulator_start_pending_ = false;
        active_simulator_.reset();
        current_config_.clear();
        current_config_label_.clear();
        pending_config_label_.clear();
        message = "Simulator stopped";
        last_error_.reset();
        if (have_snapshot) {
            last_metrics_snapshot_ = snapshot;
            has_metrics_snapshot_ = true;
        }
        simulator_cv_.notify_all();
    }

    return true;
}

std::string WebApplication::build_status_json() const
{
    std::lock_guard<std::mutex> lock(simulator_mutex_);
    std::ostringstream stream;
    stream << "{";
    stream << "\"running\":" << (simulator_running_ ? "true" : "false");
    if (!current_config_.empty()) {
        stream << ",\"config\":\"" << json_escape(current_config_) << "\"";
    }
    if (!current_config_label_.empty()) {
        stream << ",\"configLabel\":\"" << json_escape(current_config_label_) << "\"";
    }
    if (last_error_.has_value()) {
        stream << ",\"lastError\":\"" << json_escape(*last_error_) << "\"";
    }
    stream << "}";
    return stream.str();
}

std::string WebApplication::build_metrics_json() const
{
    RuntimeMetrics::Snapshot snapshot;
    bool have_snapshot = false;

    std::shared_ptr<Simulator> simulator;
    {
        std::lock_guard<std::mutex> lock(simulator_mutex_);
        simulator = active_simulator_;
        if (!simulator && has_metrics_snapshot_) {
            snapshot = last_metrics_snapshot_;
            have_snapshot = true;
        }
    }

    if (simulator) {
        snapshot = simulator->metrics_snapshot();
        have_snapshot = true;
        std::lock_guard<std::mutex> lock(simulator_mutex_);
        last_metrics_snapshot_ = snapshot;
        has_metrics_snapshot_ = true;
    }

    if (!have_snapshot) {
        snapshot = RuntimeMetrics::Snapshot{};
    }

    std::ostringstream stream;
    stream << "{";
    stream << "\"running\":" << (snapshot.simulatorRunning ? "true" : "false");
    stream << ",\"adapterInitialized\":" << (snapshot.adapterInitialized ? "true" : "false");
    stream << ",\"adapterState\":\"" << json_escape(snapshot.adapterState) << "\"";

    stream << ",\"pdPublishers\":[";
    for (std::size_t i = 0; i < snapshot.pdPublishers.size(); ++i) {
        if (i != 0) {
            stream << ',';
        }
        const auto &stats = snapshot.pdPublishers[i];
        stream << "{\"name\":\"" << json_escape(stats.name) << "\",\"packetsSent\":" << stats.packetsSent << "}";
    }
    stream << "]";

    stream << ",\"pdSubscribers\":[";
    for (std::size_t i = 0; i < snapshot.pdSubscribers.size(); ++i) {
        if (i != 0) {
            stream << ',';
        }
        const auto &stats = snapshot.pdSubscribers[i];
        stream << "{\"name\":\"" << json_escape(stats.name) << "\",\"packetsReceived\":" << stats.packetsReceived
               << "}";
    }
    stream << "]";

    stream << ",\"mdSenders\":[";
    for (std::size_t i = 0; i < snapshot.mdSenders.size(); ++i) {
        if (i != 0) {
            stream << ',';
        }
        const auto &stats = snapshot.mdSenders[i];
        stream << "{\"name\":\"" << json_escape(stats.name) << "\",\"requestsSent\":" << stats.requestsSent
               << ",\"repliesReceived\":" << stats.repliesReceived << "}";
    }
    stream << "]";

    stream << ",\"mdListeners\":[";
    for (std::size_t i = 0; i < snapshot.mdListeners.size(); ++i) {
        if (i != 0) {
            stream << ',';
        }
        const auto &stats = snapshot.mdListeners[i];
        stream << "{\"name\":\"" << json_escape(stats.name) << "\",\"requestsReceived\":" << stats.requestsReceived
               << ",\"repliesSent\":" << stats.repliesSent << "}";
    }
    stream << "]";

    stream << "}";
    return stream.str();
}

std::string WebApplication::build_config_summary_json(const SimulatorConfig &config) const
{
    std::ostringstream stream;
    stream << "{";
    stream << "\"network\":{\"interface\":\"" << json_escape(config.network.interfaceName) << "\"";
    if (!config.network.hostIp.empty()) {
        stream << ",\"hostIp\":\"" << json_escape(config.network.hostIp) << "\"";
    }
    if (!config.network.gatewayIp.empty()) {
        stream << ",\"gateway\":\"" << json_escape(config.network.gatewayIp) << "\"";
    }
    stream << ",\"vlanId\":" << config.network.vlanId;
    stream << ",\"ttl\":" << static_cast<unsigned int>(config.network.ttl) << "}";

    stream << ",\"logging\":{\"console\":" << (config.logging.enableConsole ? "true" : "false")
           << ",\"level\":\"" << json_escape(log_level_to_string(config.logging.level)) << "\"";
    if (!config.logging.filePath.empty()) {
        stream << ",\"file\":\"" << json_escape(config.logging.filePath) << "\"";
    }
    stream << "}";

    auto serialize_payload = [](const PayloadConfig &payload) {
        std::ostringstream s;
        s << "\"format\":\"" << WebApplication::json_escape(payload_format_to_string(payload.format)) << "\"";
        s << ",\"value\":\"" << WebApplication::json_escape(payload.value) << "\"";
        return s.str();
    };

    stream << ",\"pdPublishers\":[";
    for (std::size_t i = 0; i < config.pdPublishers.size(); ++i) {
        if (i != 0) {
            stream << ',';
        }
        const auto &publisher = config.pdPublishers[i];
        stream << "{\"name\":\"" << json_escape(publisher.name) << "\",\"comId\":" << publisher.comId
               << ",\"datasetId\":" << publisher.datasetId
               << ",\"cycleTimeMs\":" << publisher.cycleTimeMs
               << ",\"payload\":{" << serialize_payload(publisher.payload) << "}}";
    }
    stream << "]";

    stream << ",\"pdSubscribers\":[";
    for (std::size_t i = 0; i < config.pdSubscribers.size(); ++i) {
        if (i != 0) {
            stream << ',';
        }
        const auto &subscriber = config.pdSubscribers[i];
        stream << "{\"name\":\"" << json_escape(subscriber.name) << "\",\"comId\":" << subscriber.comId
               << ",\"timeoutMs\":" << subscriber.timeoutMs << "}";
    }
    stream << "]";

    stream << ",\"mdSenders\":[";
    for (std::size_t i = 0; i < config.mdSenders.size(); ++i) {
        if (i != 0) {
            stream << ',';
        }
        const auto &sender = config.mdSenders[i];
        stream << "{\"name\":\"" << json_escape(sender.name) << "\",\"comId\":" << sender.comId
               << ",\"cycleTimeMs\":" << sender.cycleTimeMs
               << ",\"payload\":{" << serialize_payload(sender.payload) << "}}";
    }
    stream << "]";

    stream << ",\"mdListeners\":[";
    for (std::size_t i = 0; i < config.mdListeners.size(); ++i) {
        if (i != 0) {
            stream << ',';
        }
        const auto &listener = config.mdListeners[i];
        stream << "{\"name\":\"" << json_escape(listener.name) << "\",\"comId\":" << listener.comId
               << ",\"autoReply\":" << (listener.autoReply ? "true" : "false");
        if (!listener.replyPayload.value.empty()) {
            stream << ",\"replyPayload\":{" << serialize_payload(listener.replyPayload) << "}";
        }
        stream << "}";
    }
    stream << "]";

    stream << "}";
    return stream.str();
}

std::string WebApplication::build_payloads_json() const
{
    std::shared_ptr<Simulator> simulator;
    bool running = false;
    {
        std::lock_guard<std::mutex> lock(simulator_mutex_);
        simulator = active_simulator_;
        running = simulator_running_;
    }

    SimulatorConfig config;
    bool have_config = false;
    if (simulator) {
        config = simulator->current_config();
        have_config = true;
    } else if (!current_config_.empty()) {
        try {
            config = load_configuration(current_config_);
            have_config = true;
        } catch (...) {
        }
    }

    std::ostringstream stream;
    stream << "{\"running\":" << (running ? "true" : "false") << ",\"pd\":[";
    if (have_config) {
        for (std::size_t i = 0; i < config.pdPublishers.size(); ++i) {
            if (i != 0) {
                stream << ',';
            }
            const auto &publisher = config.pdPublishers[i];
            const bool editable = publisher.payload.format != PayloadConfig::Format::File;
            stream << "{\"name\":\"" << json_escape(publisher.name) << "\",\"format\":\""
                   << json_escape(payload_format_to_string(publisher.payload.format)) << "\",\"value\":\""
                   << json_escape(publisher.payload.value) << "\",\"editable\":"
                   << (editable ? "true" : "false") << "}";
        }
    }
    stream << "],\"md\":[";
    if (have_config) {
        for (std::size_t i = 0; i < config.mdSenders.size(); ++i) {
            if (i != 0) {
                stream << ',';
            }
            const auto &sender = config.mdSenders[i];
            const bool editable = sender.payload.format != PayloadConfig::Format::File;
            stream << "{\"name\":\"" << json_escape(sender.name) << "\",\"format\":\""
                   << json_escape(payload_format_to_string(sender.payload.format)) << "\",\"value\":\""
                   << json_escape(sender.payload.value) << "\",\"editable\":"
                   << (editable ? "true" : "false") << "}";
        }
    }
    stream << "]}";
    return stream.str();
}

std::unordered_map<std::string, std::string> WebApplication::parse_form_urlencoded(const std::string &body) const
{
    std::unordered_map<std::string, std::string> params;
    std::size_t pos = 0;
    while (pos < body.size()) {
        auto amp = body.find('&', pos);
        auto token = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        auto eq = token.find('=');
        std::string key = url_decode(eq == std::string::npos ? token : token.substr(0, eq));
        std::string value = eq == std::string::npos ? std::string() : url_decode(token.substr(eq + 1));
        if (!key.empty()) {
            params[key] = value;
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return params;
}

WebApplication::HttpResponse WebApplication::respond_json(int status, const std::string &body) const
{
    return {status, std::string(), "application/json", body};
}

void WebApplication::request_stop()
{
    stop_requested_.store(true);

    std::string message;
    stop_simulator(message);

    if (server_fd_ >= 0) {
        ::shutdown(server_fd_, SHUT_RDWR);
        ::close(server_fd_);
        server_fd_ = -1;
    }
}

void WebApplication::simulator_worker(std::string config_path)
{
    std::shared_ptr<Simulator> simulator;
    try {
        auto config = load_configuration(config_path);
        auto adapter = create_trdp_stack_adapter();
        simulator = std::make_shared<Simulator>(std::move(config), std::move(adapter));

        {
            std::lock_guard<std::mutex> lock(simulator_mutex_);
            active_simulator_ = simulator;
            simulator_running_ = true;
            simulator_start_pending_ = false;
            current_config_ = config_path;
            if (!pending_config_label_.empty()) {
                current_config_label_ = pending_config_label_;
            } else {
                current_config_label_ = config_path;
            }
            pending_config_label_.clear();
            last_error_.reset();
            simulator_cv_.notify_all();
        }

        simulator->run();

        RuntimeMetrics::Snapshot snapshot = simulator->metrics_snapshot();

        {
            std::lock_guard<std::mutex> lock(simulator_mutex_);
            simulator_running_ = false;
            active_simulator_.reset();
            current_config_.clear();
            last_error_.reset();
            last_metrics_snapshot_ = snapshot;
            has_metrics_snapshot_ = true;
            simulator_cv_.notify_all();
        }
    } catch (const std::exception &ex) {
        RuntimeMetrics::Snapshot snapshot;
        bool have_snapshot = false;
        if (simulator) {
            try {
                simulator->stop();
                snapshot = simulator->metrics_snapshot();
                have_snapshot = true;
            } catch (...) {
            }
        }
        {
            std::lock_guard<std::mutex> lock(simulator_mutex_);
            last_error_ = ex.what();
            simulator_running_ = false;
            simulator_start_pending_ = false;
            active_simulator_.reset();
            current_config_.clear();
            current_config_label_.clear();
            pending_config_label_.clear();
            if (have_snapshot) {
                last_metrics_snapshot_ = snapshot;
                has_metrics_snapshot_ = true;
            }
            simulator_cv_.notify_all();
        }
    }
}

std::string WebApplication::main_page_html()
{
    return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8" />
<title>TRDP Simulator Web</title>
<style>
body { font-family: "Segoe UI", sans-serif; margin: 2rem; background: #f6f8fa; color: #1f2328; }
main { max-width: 960px; margin: 0 auto; padding: 2rem; background: #ffffff; border-radius: 12px; box-shadow: 0 2px 10px rgba(31,35,40,0.08); }
header { margin-bottom: 2rem; }
label { display: block; margin-bottom: 0.35rem; font-weight: 600; }
input[type="text"], select, textarea { width: 100%; padding: 0.6rem; border: 1px solid #d0d7de; border-radius: 6px; font-size: 0.95rem; }
textarea { min-height: 80px; resize: vertical; font-family: monospace; }
button { padding: 0.55rem 1.1rem; margin: 0.25rem 0.25rem 0.25rem 0; border: none; border-radius: 6px; cursor: pointer; font-weight: 600; }
button.start { background: #238636; color: #ffffff; }
button.stop { background: #d1242f; color: #ffffff; }
button.secondary { background: #0969da; color: #ffffff; }
button[disabled] { opacity: 0.6; cursor: not-allowed; }
section { margin-top: 2rem; }
section:first-of-type { margin-top: 0; }
pre { background: #f6f8fa; padding: 1rem; border-radius: 6px; overflow: auto; border: 1px solid #d0d7de; font-size: 0.9rem; }
#status { font-weight: 600; }
.metrics-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 1rem; margin-top: 1rem; }
.metrics-grid h3 { margin-top: 0; font-size: 1.05rem; }
.metrics-grid ul { list-style: none; padding: 0.75rem; margin: 0; background: #f6f8fa; border-radius: 6px; border: 1px solid #d0d7de; }
.metrics-grid li { margin-bottom: 0.5rem; font-size: 0.95rem; }
.metrics-grid li:last-child { margin-bottom: 0; }
.metrics-grid li.muted { color: #57606a; font-style: italic; }
.drop-zone { border: 2px dashed #0969da; padding: 1.5rem; border-radius: 8px; text-align: center; color: #0969da; background: rgba(9,105,218,0.05); transition: background 0.2s ease, border-color 0.2s ease; }
.drop-zone.dragover { background: rgba(9,105,218,0.12); border-color: #0550ae; }
.inline-actions { display: flex; gap: 0.75rem; flex-wrap: wrap; align-items: center; }
.hidden { display: none !important; }
#messages { padding: 0.75rem 1rem; border-radius: 6px; margin-bottom: 1rem; display: none; }
#messages.info { background: #e7f3ff; border: 1px solid #b6daff; color: #054289; }
#messages.error { background: #ffebe9; border: 1px solid #ff8182; color: #b54746; }
#messages.success { background: #dafbe1; border: 1px solid #4ac26b; color: #116329; }
.payload-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); gap: 1rem; margin-top: 1rem; }
.payload-card { border: 1px solid #d0d7de; border-radius: 8px; padding: 1rem; background: #f8fafc; display: flex; flex-direction: column; gap: 0.75rem; }
.payload-card h4 { margin: 0; font-size: 1rem; }
.payload-meta { font-size: 0.85rem; color: #57606a; }
.payload-actions { display: flex; justify-content: flex-end; gap: 0.5rem; }
</style>
</head>
<body>
<main>
<header>
  <h1>TRDP Simulator Web Interface</h1>
  <p>Upload TRDP configuration XML files, review their contents, store them for later use, and control the simulator directly from your browser.</p>
</header>

<div id="messages"></div>

<section id="uploadSection">
  <h2>Upload configuration</h2>
  <div class="drop-zone" id="dropZone">Drop a TRDP XML file here or <strong>click to browse</strong>.<br /><small>Only the XML content is uploaded to the server for validation.</small></div>
  <input id="configFile" type="file" accept=".xml" style="display:none" />
  <div class="inline-actions">
    <div><strong>Parsed file:</strong> <span id="parsedConfigName">None</span></div>
    <button class="secondary" id="saveConfigBtn" disabled>Save configuration</button>
  </div>
  <pre id="configSummary">Drop a configuration to preview its details.</pre>
</section>

<section id="savedConfigsSection">
  <h2>Saved configurations</h2>
  <div class="inline-actions">
    <div style="flex:1; min-width: 240px;">
      <label for="configSelect">Select a saved configuration</label>
      <select id="configSelect"></select>
    </div>
    <button class="secondary" id="viewConfigBtn">View details</button>
  </div>
  <pre id="savedConfigDetails">No configuration selected.</pre>
</section>

<section id="controlSection">
  <h2>Simulator control</h2>
  <p>Choose a saved configuration or provide a manual file path. Saved configurations are referenced as <code>saved:&lt;name&gt;</code> when starting the simulator.</p>
  <label for="configPath">Manual configuration path (optional)</label>
  <input id="configPath" type="text" placeholder="/path/to/configuration.xml" />
  <div>
    <button class="start" id="startBtn">Start simulator</button>
    <button class="stop" id="stopBtn">Stop simulator</button>
  </div>
  <section>
    <h3>Status</h3>
    <p id="status">Loading...</p>
    <p><strong>Simulator:</strong> <span id="simulatorState">Unknown</span></p>
    <pre id="details"></pre>
  </section>
</section>

<section id="payloadEditor" class="hidden">
  <h2>Live payload editor</h2>
  <p>Update PD publisher and MD sender payloads while the simulator is running. Hex payloads should be entered without prefixes (spaces are ignored).</p>
  <div>
    <h3>Process Data publishers</h3>
    <div id="pdPayloads" class="payload-grid"></div>
  </div>
  <div>
    <h3>Message Data senders</h3>
    <div id="mdPayloads" class="payload-grid"></div>
  </div>
</section>

<section id="telemetrySection">
  <h2>Telemetry</h2>
  <p><strong>Adapter:</strong> <span id="adapterState">Idle</span></p>
  <div class="metrics-grid">
    <div>
      <h3>PD Publishers</h3>
      <ul id="pdPublishersList"><li class="muted">No data</li></ul>
    </div>
    <div>
      <h3>PD Subscribers</h3>
      <ul id="pdSubscribersList"><li class="muted">No data</li></ul>
    </div>
    <div>
      <h3>MD Senders</h3>
      <ul id="mdSendersList"><li class="muted">No data</li></ul>
    </div>
    <div>
      <h3>MD Listeners</h3>
      <ul id="mdListenersList"><li class="muted">No data</li></ul>
    </div>
  </div>
  <pre id="metricsRaw"></pre>
</section>
</main>
<script>
const dropZone = document.getElementById('dropZone');
const fileInput = document.getElementById('configFile');
const configSummaryPre = document.getElementById('configSummary');
const parsedConfigName = document.getElementById('parsedConfigName');
const saveConfigBtn = document.getElementById('saveConfigBtn');
const messageBox = document.getElementById('messages');
const configSelect = document.getElementById('configSelect');
const savedConfigDetails = document.getElementById('savedConfigDetails');
let lastParsedXml = '';
let lastSuggestedName = '';

function showMessage(text, variant = 'info') {
  if (!text) {
    messageBox.style.display = 'none';
    return;
  }
  messageBox.textContent = text;
  messageBox.className = variant;
  messageBox.style.display = 'block';
  if (variant === 'success') {
    setTimeout(() => { messageBox.style.display = 'none'; }, 4000);
  }
}

function renderMetricList(elementId, items, formatter, emptyMessage) {
  const list = document.getElementById(elementId);
  if (!list) {
    return;
  }
  list.innerHTML = '';
  if (!Array.isArray(items) || items.length === 0) {
    const li = document.createElement('li');
    li.textContent = emptyMessage;
    li.classList.add('muted');
    list.appendChild(li);
    return;
  }
  items.forEach((item) => {
    const li = document.createElement('li');
    li.textContent = formatter(item);
    list.appendChild(li);
  });
}

function preventDefaults(event) {
  event.preventDefault();
  event.stopPropagation();
}

function highlightDropZone() { dropZone.classList.add('dragover'); }
function unhighlightDropZone() { dropZone.classList.remove('dragover'); }

dropZone.addEventListener('click', () => fileInput.click());
['dragenter', 'dragover'].forEach((eventName) => {
  dropZone.addEventListener(eventName, (event) => { preventDefaults(event); highlightDropZone(); });
});
['dragleave', 'drop'].forEach((eventName) => {
  dropZone.addEventListener(eventName, (event) => { preventDefaults(event); unhighlightDropZone(); });
});

dropZone.addEventListener('drop', (event) => {
  const files = event.dataTransfer.files;
  if (files && files.length > 0) {
    readConfigurationFile(files[0]);
  }
});

fileInput.addEventListener('change', (event) => {
  const files = event.target.files;
  if (files && files.length > 0) {
    readConfigurationFile(files[0]);
  }
});

function readConfigurationFile(file) {
  if (!file.name.toLowerCase().endsWith('.xml')) {
    showMessage('Please select an XML configuration file.', 'error');
    return;
  }
  const reader = new FileReader();
  reader.onload = async () => {
    const xmlText = reader.result;
    await parseUploadedConfiguration(xmlText, file.name.replace(/\.[^.]+$/, ''));
  };
  reader.readAsText(file);
}

async function parseUploadedConfiguration(xmlText, suggestedName = '') {
  try {
    const params = new URLSearchParams();
    params.set('xml', xmlText);
    if (suggestedName) {
      params.set('name', suggestedName);
    }
    const response = await fetch('/api/config/parse', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: params,
    });
    const data = await response.json();
    if (!response.ok) {
      showMessage(data.error || 'Failed to parse configuration.', 'error');
      configSummaryPre.textContent = data.error || 'Unable to parse configuration.';
      saveConfigBtn.disabled = true;
      return;
    }
    lastParsedXml = xmlText;
    lastSuggestedName = (suggestedName || data.suggestedName || '').replace(/\s+/g, '_');
    parsedConfigName.textContent = lastSuggestedName || '(unspecified)';
    configSummaryPre.textContent = JSON.stringify(data.summary, null, 2);
    saveConfigBtn.disabled = false;
    showMessage('Configuration parsed successfully.', 'success');
  } catch (error) {
    showMessage('Failed to parse configuration: ' + error.message, 'error');
  }
}

saveConfigBtn.addEventListener('click', async () => {
  if (!lastParsedXml) {
    return;
  }
  let name = prompt('Enter a name for the configuration', lastSuggestedName || 'trdp-config');
  if (!name) {
    return;
  }
  name = name.trim();
  try {
    const params = new URLSearchParams();
    params.set('name', name);
    params.set('xml', lastParsedXml);
    const response = await fetch('/api/config/save', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: params,
    });
    const data = await response.json();
    if (!response.ok) {
      showMessage(data.error || 'Unable to save configuration.', 'error');
      return;
    }
    showMessage(`Configuration "${data.name}" saved successfully.`, 'success');
    await refreshSavedConfigs();
    configSelect.value = data.name;
    await loadSelectedConfiguration();
  } catch (error) {
    showMessage('Saving configuration failed: ' + error.message, 'error');
  }
});

document.getElementById('viewConfigBtn').addEventListener('click', loadSelectedConfiguration);

async function refreshSavedConfigs() {
  try {
    const response = await fetch('/api/configs');
    const data = await response.json();
    configSelect.innerHTML = '';
    if (!data.configs || data.configs.length === 0) {
      const option = document.createElement('option');
      option.value = '';
      option.textContent = 'No saved configurations';
      option.disabled = true;
      option.selected = true;
      configSelect.appendChild(option);
      savedConfigDetails.textContent = 'No configuration selected.';
      return;
    }
    const placeholder = document.createElement('option');
    placeholder.value = '';
    placeholder.textContent = 'Select a configuration';
    placeholder.disabled = true;
    placeholder.selected = true;
    configSelect.appendChild(placeholder);
    data.configs.forEach((item) => {
      const option = document.createElement('option');
      option.value = item.name;
      option.textContent = item.name;
      configSelect.appendChild(option);
    });
  } catch (error) {
    showMessage('Unable to fetch saved configurations: ' + error.message, 'error');
  }
}

async function loadSelectedConfiguration() {
  const name = configSelect.value;
  if (!name) {
    savedConfigDetails.textContent = 'No configuration selected.';
    return;
  }
  try {
    const response = await fetch(`/api/config/details?name=${encodeURIComponent(name)}`);
    const data = await response.json();
    if (!response.ok) {
      showMessage(data.error || 'Unable to load configuration details.', 'error');
      return;
    }
    savedConfigDetails.textContent = JSON.stringify(data.summary, null, 2);
  } catch (error) {
    showMessage('Unable to load configuration details: ' + error.message, 'error');
  }
}

async function startSimulator() {
  const manualPath = document.getElementById('configPath').value.trim();
  const savedName = configSelect.value;
  let configSpec = '';
  if (manualPath) {
    configSpec = manualPath;
  } else if (savedName) {
    configSpec = `saved:${savedName}`;
  }
  if (!configSpec) {
    showMessage('Please choose a saved configuration or provide a path before starting the simulator.', 'error');
    return;
  }
  try {
    const params = new URLSearchParams();
    params.set('config', configSpec);
    const response = await fetch('/api/start', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: params,
    });
    const data = await response.json();
    if (!response.ok) {
      showMessage(data.error || 'Failed to start simulator.', 'error');
      return;
    }
    showMessage(data.message || 'Simulator started.', 'success');
    refreshStatus();
    refreshMetrics();
    refreshPayloads();
  } catch (error) {
    showMessage('Failed to start simulator: ' + error.message, 'error');
  }
}

async function stopSimulator() {
  try {
    const response = await fetch('/api/stop', { method: 'POST' });
    const data = await response.json();
    if (!response.ok) {
      showMessage(data.error || 'Failed to stop simulator.', 'error');
      return;
    }
    showMessage(data.message || 'Simulator stopped.', 'success');
    refreshStatus();
    refreshMetrics();
    refreshPayloads();
  } catch (error) {
    showMessage('Failed to stop simulator: ' + error.message, 'error');
  }
}

document.getElementById('startBtn').addEventListener('click', startSimulator);
document.getElementById('stopBtn').addEventListener('click', stopSimulator);

async function refreshStatus() {
  try {
    const response = await fetch('/api/status');
    const data = await response.json();
    const status = document.getElementById('status');
    const details = document.getElementById('details');
    const simulatorState = document.getElementById('simulatorState');
    if (data.running) {
      status.textContent = `Simulator is running${data.configLabel ? ' with ' + data.configLabel : ''}.`;
      simulatorState.textContent = 'Running';
    } else {
      status.textContent = 'Simulator is stopped';
      simulatorState.textContent = 'Stopped';
    }
    details.textContent = JSON.stringify(data, null, 2);
    if (data.running) {
      refreshPayloads();
    } else {
      hidePayloadEditor();
    }
  } catch (err) {
    document.getElementById('status').textContent = 'Unable to query status';
    document.getElementById('simulatorState').textContent = 'Unknown';
    hidePayloadEditor();
  }
}

function hidePayloadEditor() {
  document.getElementById('payloadEditor').classList.add('hidden');
  document.getElementById('pdPayloads').innerHTML = '';
  document.getElementById('mdPayloads').innerHTML = '';
}

async function refreshMetrics() {
  try {
    const response = await fetch('/api/metrics');
    if (!response.ok) {
      throw new Error('Request failed');
    }
    const data = await response.json();
    document.getElementById('adapterState').textContent = data.adapterState || 'Unknown';
    renderMetricList('pdPublishersList', data.pdPublishers || [],
      (item) => `${item.name}: ${item.packetsSent} packets sent`, 'No PD publishers');
    renderMetricList('pdSubscribersList', data.pdSubscribers || [],
      (item) => `${item.name}: ${item.packetsReceived} packets received`, 'No PD subscribers');
    renderMetricList('mdSendersList', data.mdSenders || [],
      (item) => `${item.name}: ${item.requestsSent} requests / ${item.repliesReceived} replies`, 'No MD senders');
    renderMetricList('mdListenersList', data.mdListeners || [],
      (item) => `${item.name}: ${item.requestsReceived} requests / ${item.repliesSent} replies`, 'No MD listeners');
    document.getElementById('metricsRaw').textContent = JSON.stringify(data, null, 2);
  } catch (err) {
    document.getElementById('metricsRaw').textContent = 'Unable to query metrics';
  }
}

async function refreshPayloads() {
  try {
    const response = await fetch('/api/simulator/payloads');
    if (!response.ok) {
      throw new Error('Unable to fetch payloads');
    }
    const data = await response.json();
    const editorSection = document.getElementById('payloadEditor');
    if (!data.running) {
      hidePayloadEditor();
      return;
    }
    editorSection.classList.remove('hidden');
    renderPayloadCards('pdPayloads', data.pd || [], 'pd');
    renderPayloadCards('mdPayloads', data.md || [], 'md');
  } catch (error) {
    showMessage('Unable to refresh payload information: ' + error.message, 'error');
  }
}

function renderPayloadCards(containerId, items, type) {
  const container = document.getElementById(containerId);
  container.innerHTML = '';
  if (!items || items.length === 0) {
    const placeholder = document.createElement('div');
    placeholder.textContent = 'No entries available.';
    placeholder.classList.add('payload-meta');
    container.appendChild(placeholder);
    return;
  }
  items.forEach((item) => {
    const card = document.createElement('div');
    card.className = 'payload-card';
    const title = document.createElement('h4');
    title.textContent = item.name;
    const meta = document.createElement('div');
    meta.className = 'payload-meta';
    meta.textContent = `Format: ${item.format}${item.editable ? '' : ' (read-only)'}`;
    const input = document.createElement('textarea');
    input.value = item.value || '';
    input.disabled = !item.editable;
    const actions = document.createElement('div');
    actions.className = 'payload-actions';
    const button = document.createElement('button');
    button.textContent = 'Update payload';
    button.className = 'secondary';
    button.disabled = !item.editable;
    button.addEventListener('click', async () => {
      await updatePayload(type, item.name, item.format, input.value);
    });
    actions.appendChild(button);
    card.appendChild(title);
    card.appendChild(meta);
    card.appendChild(input);
    card.appendChild(actions);
    container.appendChild(card);
  });
}

async function updatePayload(type, name, format, value) {
  try {
    const params = new URLSearchParams();
    params.set('type', type);
    params.set('name', name);
    params.set('format', format);
    params.set('value', value);
    const response = await fetch('/api/simulator/payload', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: params,
    });
    const data = await response.json();
    if (!response.ok) {
      showMessage(data.error || 'Unable to update payload.', 'error');
      return;
    }
    showMessage(data.message || 'Payload updated.', 'success');
    refreshPayloads();
  } catch (error) {
    showMessage('Unable to update payload: ' + error.message, 'error');
  }
}

refreshSavedConfigs();
refreshStatus();
refreshMetrics();
setInterval(refreshStatus, 4000);
setInterval(refreshMetrics, 5000);
</script>
</body>
</html>)HTML";
}

std::string WebApplication::json_escape(const std::string &value)
{
    std::ostringstream stream;
    for (char ch : value) {
        unsigned char uc = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"':
            stream << "\\\"";
            break;
        case '\\':
            stream << "\\\\";
            break;
        case '\n':
            stream << "\\n";
            break;
        case '\r':
            stream << "\\r";
            break;
        case '\t':
            stream << "\\t";
            break;
        default:
            if (uc < 0x20) {
                stream << "\\u00";
                const char *hex = "0123456789ABCDEF";
                stream << hex[(uc >> 4) & 0x0F] << hex[uc & 0x0F];
            } else {
                stream << ch;
            }
            break;
        }
    }
    return stream.str();
}

}  // namespace trdp_sim
