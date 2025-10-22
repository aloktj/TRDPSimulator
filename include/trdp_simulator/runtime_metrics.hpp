#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace trdp_sim {

class RuntimeMetrics {
public:
    struct PdPublisherStats {
        std::string name;
        std::uint64_t packetsSent{0};
    };

    struct PdSubscriberStats {
        std::string name;
        std::uint64_t packetsReceived{0};
    };

    struct MdSenderStats {
        std::string name;
        std::uint64_t requestsSent{0};
        std::uint64_t repliesReceived{0};
    };

    struct MdListenerStats {
        std::string name;
        std::uint64_t requestsReceived{0};
        std::uint64_t repliesSent{0};
    };

    struct Snapshot {
        bool simulatorRunning{false};
        bool adapterInitialized{false};
        std::string adapterState{"Idle"};
        std::vector<PdPublisherStats> pdPublishers;
        std::vector<PdSubscriberStats> pdSubscribers;
        std::vector<MdSenderStats> mdSenders;
        std::vector<MdListenerStats> mdListeners;
    };

    void reset();
    void set_simulator_running(bool running);
    void set_adapter_status(bool initialized, std::string state);

    void record_pd_publish(const std::string &name);
    void record_pd_receive(const std::string &name);
    void record_md_request_sent(const std::string &name);
    void record_md_reply_received(const std::string &name);
    void record_md_request_received(const std::string &name);
    void record_md_reply_sent(const std::string &name);

    Snapshot snapshot() const;

private:
    template <typename StatsMap>
    static typename StatsMap::mapped_type &ensure_entry(StatsMap &map, const std::string &name)
    {
        auto &entry = map[name];
        if (entry.name.empty()) {
            entry.name = name;
        }
        return entry;
    }

    template <typename StatsMap>
    static const typename StatsMap::mapped_type &ensure_entry(const StatsMap &map, const std::string &name)
    {
        auto it = map.find(name);
        if (it != map.end()) {
            return it->second;
        }
        static typename StatsMap::mapped_type empty{};
        return empty;
    }

    mutable std::mutex mutex_;
    bool simulatorRunning_{false};
    bool adapterInitialized_{false};
    std::string adapterState_{"Idle"};
    std::map<std::string, PdPublisherStats> pdPublishers_;
    std::map<std::string, PdSubscriberStats> pdSubscribers_;
    std::map<std::string, MdSenderStats> mdSenders_;
    std::map<std::string, MdListenerStats> mdListeners_;
};

}  // namespace trdp_sim

