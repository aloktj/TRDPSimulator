#include "trdp_simulator/web_application.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "trdp_simulator/config_loader.hpp"
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

    if (simulator) {
        simulator->stop();
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

        {
            std::lock_guard<std::mutex> lock(simulator_mutex_);
            simulator_running_ = false;
            active_simulator_.reset();
            current_config_.clear();
            last_error_.reset();
            simulator_cv_.notify_all();
        }
    } catch (const std::exception &ex) {
        {
            std::lock_guard<std::mutex> lock(simulator_mutex_);
            last_error_ = ex.what();
            simulator_running_ = false;
            simulator_start_pending_ = false;
            active_simulator_.reset();
            current_config_.clear();
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
main { max-width: 640px; margin: 0 auto; padding: 2rem; background: #ffffff; border-radius: 8px; box-shadow: 0 2px 4px rgba(31,35,40,0.1); }
header { margin-bottom: 1.5rem; }
label { display: block; margin-bottom: 0.5rem; font-weight: 600; }
input[type="text"] { width: 100%; padding: 0.5rem; margin-bottom: 1rem; border: 1px solid #d0d7de; border-radius: 4px; }
button { padding: 0.5rem 1rem; margin-right: 0.5rem; border: none; border-radius: 4px; cursor: pointer; font-weight: 600; }
button.start { background: #238636; color: #ffffff; }
button.stop { background: #d1242f; color: #ffffff; }
section { margin-top: 1.5rem; }
pre { background: #f6f8fa; padding: 1rem; border-radius: 4px; overflow: auto; }
#status { font-weight: 600; }
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
<div>
<button class="start" id="startBtn">Start simulator</button>
<button class="stop" id="stopBtn">Stop simulator</button>
</div>
<section>
<h2>Status</h2>
<p id="status">Loading...</p>
<pre id="details"></pre>
</section>
</main>
<script>
async function refreshStatus() {
  try {
    const response = await fetch('/api/status');
    const data = await response.json();
    const status = document.getElementById('status');
    const details = document.getElementById('details');
    if (data.running) {
      status.textContent = 'Simulator is running';
    } else {
      status.textContent = 'Simulator is stopped';
    }
    details.textContent = JSON.stringify(data, null, 2);
  } catch (err) {
    document.getElementById('status').textContent = 'Unable to query status';
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
});

refreshStatus();
setInterval(refreshStatus, 3000);
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

}  // namespace trdp_sim
