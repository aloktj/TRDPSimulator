#include "trdp_simulator/web_application.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "trdp_simulator/config_loader.hpp"
#include "trdp_simulator/simulator.hpp"
#include "trdp_simulator/trdp_stack_adapter.hpp"

namespace trdp_sim {

namespace {
constexpr std::size_t kMaxConfigFileSize = 512 * 1024;
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

WebApplication::HttpResponse WebApplication::make_error_response(int status, const std::string &message)
{
    return {status, "", "application/json",
            "{\"error\":\"" + json_escape(message) + "\"}"};
}

const std::filesystem::path &WebApplication::resolved_config_root()
{
    static const std::filesystem::path root = std::filesystem::absolute(config_root());
    return root;
}

bool WebApplication::ensure_config_directory(std::string &error)
{
    std::error_code ec;
    if (!std::filesystem::exists(resolved_config_root(), ec)) {
        if (ec) {
            error = "Unable to access config directory: " + ec.message();
            return false;
        }
        std::filesystem::create_directories(resolved_config_root(), ec);
        if (ec) {
            error = "Unable to create config directory: " + ec.message();
            return false;
        }
    }
    bool directory = std::filesystem::is_directory(resolved_config_root(), ec);
    if (ec || !directory) {
        error = ec ? "Unable to validate config directory: " + ec.message()
                    : "Config path is not a directory";
        return false;
    }
    return true;
}

bool WebApplication::ensure_config_directory()
{
    std::string error;
    return ensure_config_directory(error);
}

std::string WebApplication::absolute_path_string(const std::filesystem::path &path)
{
    std::error_code ec;
    auto absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        return path.lexically_normal().generic_string();
    }
    return absolute.lexically_normal().generic_string();
}

WebApplication::HttpResponse WebApplication::write_config_file(const std::string &relative_path,
                                                               const std::string &contents,
                                                               const std::string &success_message)
{
    if (relative_path.empty()) {
        return make_error_response(400, "Missing configuration path");
    }
    if (!is_safe_config_relative_path(relative_path)) {
        return make_error_response(400, "Configuration path is not allowed");
    }
    if (!is_xml_file_name(relative_path)) {
        return make_error_response(400, "Configuration files must use the .xml extension");
    }
    if (contents.size() > kMaxConfigFileSize) {
        return make_error_response(400, "Configuration exceeds size limit (512 KB)");
    }

    std::string error;
    if (!ensure_config_directory(error)) {
        return make_error_response(500, error);
    }

    std::filesystem::path full_path = resolved_config_root() / std::filesystem::path(relative_path);
    std::filesystem::path parent = full_path.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
        bool parent_exists = std::filesystem::exists(parent, ec);
        if (ec) {
            return make_error_response(500, "Unable to access parent directory: " + ec.message());
        }
        if (!parent_exists) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                return make_error_response(500, "Unable to create directories for configuration: " + ec.message());
            }
        }
    }

    std::ofstream output(full_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return make_error_response(500, "Unable to open configuration file for writing");
    }

    output << contents;
    if (!output) {
        return make_error_response(500, "Failed to write configuration file");
    }

    std::ostringstream stream;
    stream << "{\"message\":\"" << json_escape(success_message) << "\",\"path\":\""
           << json_escape(relative_path) << "\",\"absolutePath\":\""
           << json_escape(absolute_path_string(full_path)) << "\"}";
    return {200, "OK", "application/json", stream.str()};
}

WebApplication::WebApplication(std::string host, unsigned short port)
    : host_(std::move(host)), port_(port)
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
        return {200, "OK", "application/json", build_status_json()};
    }

    if (path == "/api/metrics") {
        return {200, "OK", "application/json", build_metrics_json()};
    }

    if (path == "/api/config/list") {
        return handle_list_configs();
    }

    if (path == "/api/config" && method == "GET") {
        return handle_get_config(query);
    }

    if (path == "/api/config/save" && method == "POST") {
        return handle_save_config(body);
    }

    if (path == "/api/config/upload" && method == "POST") {
        return handle_upload_config(body);
    }

    if (path == "/api/start") {
        std::string config = extract_parameter(query, "config");
        if (config.empty() && method == "POST") {
            config = extract_parameter(body, "config");
        }
        if (config.empty()) {
            return {400, "Bad Request", "application/json",
                    "{\"error\":\"Missing config parameter\"}"};
        }
        std::string message;
        if (start_simulator(url_decode(config), message)) {
            return {202, "Accepted", "application/json",
                    "{\"message\":\"" + json_escape(message) + "\"}"};
        }
        return {409, "Conflict", "application/json",
                "{\"error\":\"" + json_escape(message) + "\"}"};
    }

    if (path == "/api/stop") {
        std::string message;
        if (stop_simulator(message)) {
            return {200, "OK", "application/json",
                    "{\"message\":\"" + json_escape(message) + "\"}"};
        }
        return {409, "Conflict", "application/json",
                "{\"error\":\"" + json_escape(message) + "\"}"};
    }

    return {404, "Not Found", "application/json", "{\"error\":\"Not found\"}"};
}

WebApplication::HttpResponse WebApplication::handle_get_config(const std::string &query) const
{
    std::string error;
    if (!ensure_config_directory(error)) {
        return make_error_response(500, error);
    }

    std::string path = url_decode(extract_parameter(query, "path"));
    if (path.empty()) {
        return make_error_response(400, "Missing path parameter");
    }
    if (!is_safe_config_relative_path(path)) {
        return make_error_response(400, "Configuration path is not allowed");
    }

    std::filesystem::path full_path = resolved_config_root() / std::filesystem::path(path);
    std::error_code ec;
    bool exists = std::filesystem::exists(full_path, ec);
    if (ec || !exists) {
        return make_error_response(404, ec ? "Unable to access configuration file: " + ec.message()
                                          : "Configuration file not found");
    }
    bool regular = std::filesystem::is_regular_file(full_path, ec);
    if (ec || !regular) {
        return make_error_response(404, ec ? "Unable to access configuration file: " + ec.message()
                                          : "Configuration file not found");
    }

    auto size = std::filesystem::file_size(full_path, ec);
    if (ec) {
        return make_error_response(500, "Unable to read configuration file: " + ec.message());
    }
    if (size > kMaxConfigFileSize) {
        return make_error_response(400, "Configuration exceeds size limit (512 KB)");
    }

    std::ifstream input(full_path, std::ios::binary);
    if (!input) {
        return make_error_response(500, "Unable to open configuration file");
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return make_error_response(500, "Failed to read configuration file");
    }

    std::ostringstream body;
    body << "{\"path\":\"" << json_escape(path) << "\",\"absolutePath\":\""
         << json_escape(absolute_path_string(full_path)) << "\",\"contents\":\""
         << json_escape(contents.str()) << "\"}";
    return {200, "OK", "application/json", body.str()};
}

WebApplication::HttpResponse WebApplication::handle_list_configs() const
{
    std::string error;
    if (!ensure_config_directory(error)) {
        return make_error_response(500, error);
    }

    std::error_code ec;
    std::vector<std::pair<std::string, std::string>> files;
    for (std::filesystem::recursive_directory_iterator it(resolved_config_root(), ec), end;
         it != end && !ec; ++it) {
        if (!it->is_regular_file(ec) || ec) {
            continue;
        }
        if (!it->path().has_extension()) {
            continue;
        }
        if (!is_xml_file_name(it->path().filename().string())) {
            continue;
        }
        std::filesystem::path rel = it->path().lexically_relative(resolved_config_root());
        std::string rel_str = rel.generic_string();
        if (!is_safe_config_relative_path(rel_str)) {
            continue;
        }
        files.emplace_back(rel_str, absolute_path_string(it->path()));
    }

    if (ec) {
        return make_error_response(500, "Unable to list configuration directory: " + ec.message());
    }

    std::sort(files.begin(), files.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.first < rhs.first;
    });

    std::ostringstream stream;
    stream << "{\"files\":[";
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (i != 0) {
            stream << ',';
        }
        stream << "{\"path\":\"" << json_escape(files[i].first) << "\",\"absolutePath\":\""
               << json_escape(files[i].second) << "\"}";
    }
    stream << "]}";
    return {200, "OK", "application/json", stream.str()};
}

WebApplication::HttpResponse WebApplication::handle_save_config(const std::string &body) const
{
    std::string path = url_decode(extract_parameter(body, "path"));
    std::string contents = url_decode(extract_parameter(body, "contents"));
    return write_config_file(path, contents, "Saved configuration");
}

WebApplication::HttpResponse WebApplication::handle_upload_config(const std::string &body) const
{
    std::string file_name = url_decode(extract_parameter(body, "fileName"));
    std::string contents = url_decode(extract_parameter(body, "contents"));
    return write_config_file(file_name, contents, "Uploaded configuration");
}

bool WebApplication::start_simulator(const std::string &config_path, std::string &message)
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
    has_metrics_snapshot_ = false;
    last_metrics_snapshot_ = RuntimeMetrics::Snapshot{};

    simulator_thread_ = std::thread(&WebApplication::simulator_worker, this, config_path);

    simulator_cv_.wait(lock, [this]() { return !simulator_start_pending_; });

    if (simulator_running_) {
        message = "Simulator started";
        return true;
    }

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
body { font-family: sans-serif; margin: 2rem; background: #f6f8fa; color: #1f2328; }
main { max-width: 780px; margin: 0 auto; padding: 2rem; background: #ffffff; border-radius: 8px; box-shadow: 0 2px 4px rgba(31,35,40,0.1); }
header { margin-bottom: 1.5rem; }
label { display: block; margin-bottom: 0.5rem; font-weight: 600; }
input[type="text"] { width: 100%; padding: 0.5rem; margin-bottom: 1rem; border: 1px solid #d0d7de; border-radius: 4px; }
select { width: 100%; padding: 0.5rem; border: 1px solid #d0d7de; border-radius: 4px; background: #ffffff; }
textarea { width: 100%; min-height: 260px; padding: 0.75rem; border: 1px solid #d0d7de; border-radius: 4px; font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace; font-size: 0.9rem; line-height: 1.4; }
button { padding: 0.5rem 1rem; margin-right: 0.5rem; border: none; border-radius: 4px; cursor: pointer; font-weight: 600; }
button.start { background: #238636; color: #ffffff; }
button.stop { background: #d1242f; color: #ffffff; }
section { margin-top: 1.5rem; }
pre { background: #f6f8fa; padding: 1rem; border-radius: 4px; overflow: auto; border: 1px solid #d0d7de; }
#status { font-weight: 600; }
.control-row { display: flex; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.5rem; }
.control-row button { margin-right: 0; }
.config-controls { display: flex; flex-wrap: wrap; gap: 0.5rem; align-items: center; margin-bottom: 0.75rem; }
.config-controls select { flex: 1 1 260px; min-width: 220px; }
.config-controls button { margin-right: 0; }
.config-actions { display: flex; flex-wrap: wrap; gap: 0.5rem; align-items: center; margin-top: 0.75rem; }
.config-actions button { margin-right: 0; }
.config-actions input[type="file"] { flex: 1 1 260px; }
.notice { margin-top: 0.5rem; font-size: 0.9rem; }
.notice.success { color: #1a7f37; }
.notice.error { color: #cf222e; }
.notice.muted { color: #57606a; font-style: italic; }
.metrics-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 1rem; margin-top: 1rem; }
.metrics-grid h3 { margin-top: 0; font-size: 1.05rem; }
.metrics-grid ul { list-style: none; padding: 0.75rem; margin: 0; background: #f6f8fa; border-radius: 4px; border: 1px solid #d0d7de; }
.metrics-grid li { margin-bottom: 0.5rem; font-size: 0.95rem; }
.metrics-grid li:last-child { margin-bottom: 0; }
.metrics-grid li.muted { color: #57606a; font-style: italic; }
</style>
</head>
<body>
<main>
<header>
<h1>TRDP Simulator Web Interface</h1>
<p>Start and stop the simulator from your browser. Provide the path to a configuration XML file accessible on the host.</p>
</header>
<label for="configPath">Configuration file</label>
<input id="configPath" type="text" placeholder="/path/to/configuration.xml" />
<div class="control-row">
<button class="start" id="startBtn">Start simulator</button>
<button class="stop" id="stopBtn">Stop simulator</button>
</div>
<section>
<h2>Status</h2>
<p id="status">Loading...</p>
<p><strong>Simulator:</strong> <span id="simulatorState">Unknown</span></p>
<pre id="details"></pre>
</section>
<section>
<h2>Configuration editor</h2>
<p>Manage XML files stored under <code>config/</code>. Load, edit, save, or upload new configurations without leaving the dashboard.</p>
<label for="configSelect">Available configurations</label>
<div class="config-controls">
<select id="configSelect"></select>
<button id="loadConfigBtn" type="button">Load</button>
<button id="useConfigBtn" type="button">Use for start</button>
</div>
<label for="configEditorPath">File name (relative to <code>config/</code>)</label>
<input id="configEditorPath" type="text" placeholder="example_configuration.xml" />
<label for="configEditorContents">Contents</label>
<textarea id="configEditorContents" placeholder="&lt;trdpSimulator&gt;&#10;  ...&#10;&lt;/trdpSimulator&gt;"></textarea>
<div class="config-actions">
<button id="saveConfigBtn" type="button">Save changes</button>
<input id="configUpload" type="file" accept=".xml" />
<button id="uploadConfigBtn" type="button">Upload file</button>
</div>
<p id="configMessage" class="notice muted"></p>
</section>
<section>
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
function renderMetricList(elementId, items, formatter, emptyMessage) {
  const list = document.getElementById(elementId);
  if (!list) {
    return;
  }
  const formatItem = typeof formatter === 'function' ? formatter : (value) => String(value);
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
    li.textContent = formatItem(item);
    list.appendChild(li);
  });
}

let lastLoadedConfig = null;

function setConfigMessage(message, type = 'info') {
  const element = document.getElementById('configMessage');
  if (!element) {
    return;
  }
  element.textContent = message || '';
  element.classList.remove('success', 'error', 'muted');
  if (!message) {
    element.classList.add('muted');
    return;
  }
  if (type === 'success') {
    element.classList.add('success');
  } else if (type === 'error') {
    element.classList.add('error');
  } else {
    element.classList.add('muted');
  }
}

async function loadConfigList(selectedPath = null) {
  const select = document.getElementById('configSelect');
  if (!select) {
    return;
  }
  const previous = selectedPath || select.dataset.selected || '';
  try {
    const response = await fetch('/api/config/list');
    if (!response.ok) {
      throw new Error('Request failed');
    }
    const data = await response.json();
    const files = Array.isArray(data.files) ? data.files : [];
    select.innerHTML = '';
    if (files.length === 0) {
      const option = document.createElement('option');
      option.textContent = 'No configurations found';
      option.value = '';
      option.disabled = true;
      select.appendChild(option);
      select.dataset.selected = '';
      return;
    }
    files.forEach((file) => {
      const option = document.createElement('option');
      option.value = file.path;
      option.textContent = file.path;
      if (file.absolutePath) {
        option.dataset.absolutePath = file.absolutePath;
      }
      select.appendChild(option);
    });
    const target = files.some((file) => file.path === previous) ? previous : files[0].path;
    select.value = target;
    select.dataset.selected = select.value;
  } catch (err) {
    select.innerHTML = '';
    const option = document.createElement('option');
    option.textContent = 'Unable to load configurations';
    option.value = '';
    option.disabled = true;
    select.appendChild(option);
    select.dataset.selected = '';
    setConfigMessage('Unable to load configuration list.', 'error');
  }
}

async function fetchConfiguration(path) {
  try {
    const response = await fetch('/api/config?path=' + encodeURIComponent(path));
    if (!response.ok) {
      const errorData = await response.json().catch(() => ({}));
      throw new Error(errorData.error || 'Request failed');
    }
    const data = await response.json();
    document.getElementById('configEditorPath').value = data.path || path;
    document.getElementById('configEditorContents').value = data.contents || '';
    if (data.absolutePath) {
      document.getElementById('configPath').value = data.absolutePath;
    }
    lastLoadedConfig = data;
    setConfigMessage(`Loaded ${data.path || path}.`, 'success');
    await loadConfigList(data.path);
  } catch (err) {
    setConfigMessage(err.message || 'Unable to load configuration.', 'error');
  }
}

async function saveConfiguration() {
  const pathInput = document.getElementById('configEditorPath');
  const contentsInput = document.getElementById('configEditorContents');
  const path = pathInput.value.trim();
  if (!path) {
    setConfigMessage('Enter a file name before saving.', 'error');
    return;
  }
  const body = 'path=' + encodeURIComponent(path) + '&contents=' + encodeURIComponent(contentsInput.value);
  try {
    const response = await fetch('/api/config/save', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body
    });
    const data = await response.json();
    if (!response.ok) {
      throw new Error(data.error || 'Unable to save configuration.');
    }
    pathInput.value = data.path || path;
    if (data.absolutePath) {
      document.getElementById('configPath').value = data.absolutePath;
    }
    lastLoadedConfig = data;
    setConfigMessage(data.message || 'Configuration saved.', 'success');
    await loadConfigList(data.path);
  } catch (err) {
    setConfigMessage(err.message || 'Unable to save configuration.', 'error');
  }
}

async function uploadConfiguration() {
  const fileInput = document.getElementById('configUpload');
  if (!fileInput || !fileInput.files || fileInput.files.length === 0) {
    setConfigMessage('Select an XML file to upload.', 'error');
    return;
  }
  const file = fileInput.files[0];
  if (file.size > 524288) {
    setConfigMessage('Selected file is larger than 512 KB.', 'error');
    return;
  }
  try {
    const contents = await file.text();
    const body = 'fileName=' + encodeURIComponent(file.name) + '&contents=' + encodeURIComponent(contents);
    const response = await fetch('/api/config/upload', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body
    });
    const data = await response.json();
    if (!response.ok) {
      throw new Error(data.error || 'Unable to upload configuration.');
    }
    document.getElementById('configEditorPath').value = data.path || file.name;
    document.getElementById('configEditorContents').value = contents;
    if (data.absolutePath) {
      document.getElementById('configPath').value = data.absolutePath;
    }
    fileInput.value = '';
    lastLoadedConfig = data;
    setConfigMessage(data.message || 'Configuration uploaded.', 'success');
    await loadConfigList(data.path);
  } catch (err) {
    setConfigMessage(err.message || 'Unable to upload configuration.', 'error');
  }
}

document.getElementById('loadConfigBtn').addEventListener('click', async () => {
  const select = document.getElementById('configSelect');
  if (!select || !select.value) {
    setConfigMessage('Select a configuration to load.', 'error');
    return;
  }
  await fetchConfiguration(select.value);
});

document.getElementById('useConfigBtn').addEventListener('click', () => {
  const startInput = document.getElementById('configPath');
  if (!startInput) {
    return;
  }
  if (lastLoadedConfig && lastLoadedConfig.absolutePath) {
    startInput.value = lastLoadedConfig.absolutePath;
    setConfigMessage('Configuration path copied to start field.', 'success');
    return;
  }
  const select = document.getElementById('configSelect');
  const option = select && select.selectedOptions && select.selectedOptions[0] ? select.selectedOptions[0] : null;
  if (option && option.dataset.absolutePath) {
    startInput.value = option.dataset.absolutePath;
    setConfigMessage('Configuration path copied to start field.', 'success');
    return;
  }
  const relative = document.getElementById('configEditorPath').value.trim();
  if (!relative) {
    setConfigMessage('Load or enter a configuration file name first.', 'error');
    return;
  }
  startInput.value = 'config/' + relative;
  setConfigMessage('Using config/' + relative + ' for simulator start.', 'success');
});

document.getElementById('saveConfigBtn').addEventListener('click', saveConfiguration);
document.getElementById('uploadConfigBtn').addEventListener('click', uploadConfiguration);

async function refreshStatus() {
  try {
    const response = await fetch('/api/status');
    const data = await response.json();
    const status = document.getElementById('status');
    const details = document.getElementById('details');
    const simulatorState = document.getElementById('simulatorState');
    if (data.running) {
      status.textContent = 'Simulator is running';
      simulatorState.textContent = 'Running';
    } else {
      status.textContent = 'Simulator is stopped';
      simulatorState.textContent = 'Stopped';
    }
    details.textContent = JSON.stringify(data, null, 2);
  } catch (err) {
    document.getElementById('status').textContent = 'Unable to query status';
    document.getElementById('simulatorState').textContent = 'Unknown';
  }
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
    document.getElementById('adapterState').textContent = 'Unavailable';
    renderMetricList('pdPublishersList', [], () => '', 'No data available');
    renderMetricList('pdSubscribersList', [], () => '', 'No data available');
    renderMetricList('mdSendersList', [], () => '', 'No data available');
    renderMetricList('mdListenersList', [], () => '', 'No data available');
    document.getElementById('metricsRaw').textContent = 'Unable to query metrics';
  }
}

document.getElementById('startBtn').addEventListener('click', async () => {
  const configPath = document.getElementById('configPath').value.trim();
  if (!configPath) {
    alert('Enter the full path to a configuration file.');
    return;
  }
  const response = await fetch('/api/start?config=' + encodeURIComponent(configPath), { method: 'POST' });
  const data = await response.json();
  if (response.ok || response.status === 202) {
    alert(data.message || 'Simulator starting');
  } else {
    alert(data.error || 'Unable to start simulator');
  }
  refreshStatus();
  refreshMetrics();
});

document.getElementById('stopBtn').addEventListener('click', async () => {
  const response = await fetch('/api/stop', { method: 'POST' });
  const data = await response.json();
  if (response.ok) {
    alert(data.message || 'Simulator stopped');
  } else {
    alert(data.error || 'Simulator not running');
  }
  refreshStatus();
  refreshMetrics();
});

loadConfigList();
refreshStatus();
refreshMetrics();
setInterval(() => {
  refreshStatus();
  refreshMetrics();
}, 3000);
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

std::string WebApplication::url_decode(const std::string &value)
{
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            std::string hex = value.substr(i + 1, 2);
            char *end = nullptr;
            long decoded = std::strtol(hex.c_str(), &end, 16);
            if (end != nullptr && *end == '\0') {
                result.push_back(static_cast<char>(decoded));
                i += 2;
                continue;
            }
        } else if (value[i] == '+') {
            result.push_back(' ');
            continue;
        }
        result.push_back(value[i]);
    }
    return result;
}

std::string WebApplication::extract_parameter(const std::string &query, const std::string &key)
{
    std::size_t start = 0;
    while (start < query.size()) {
        auto end = query.find('&', start);
        if (end == std::string::npos) {
            end = query.size();
        }
        auto eq = query.find('=', start);
        if (eq != std::string::npos && eq < end) {
            std::string name = query.substr(start, eq - start);
            if (name == key) {
                return query.substr(eq + 1, end - eq - 1);
            }
        } else {
            if (query.substr(start, end - start) == key) {
                return std::string();
            }
        }
        start = end + 1;
    }
    return std::string();
}

bool WebApplication::is_safe_config_relative_path(const std::string &path)
{
    if (path.empty()) {
        return false;
    }
    unsigned char first = static_cast<unsigned char>(path.front());
    if (first == '/' || first == '\\') {
        return false;
    }
    if (path.find("..") != std::string::npos) {
        return false;
    }
    if (path.find(':') != std::string::npos) {
        return false;
    }
    if (path.find('\\') != std::string::npos) {
        return false;
    }
    for (unsigned char ch : path) {
        if (std::iscntrl(ch)) {
            return false;
        }
    }
    if (path.find("//") != std::string::npos) {
        return false;
    }
    return true;
}

bool WebApplication::is_xml_file_name(const std::string &name)
{
    std::filesystem::path path(name);
    auto extension = path.extension().string();
    if (extension.empty()) {
        return false;
    }
    std::string lower(extension.size(), '\0');
    std::transform(extension.begin(), extension.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lower == ".xml";
}

std::filesystem::path WebApplication::config_root()
{
    return std::filesystem::path("config");
}

}  // namespace trdp_sim
