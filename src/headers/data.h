#pragma once
#include <string>
#include <vector>
#include <mutex>

struct TelemetryData {
    std::string lat, lon, alt, accuracy, signal, timestamp, net_type, cell_info;
    std::string band, cell_id, pci, tac, mcc, mnc, rsrq, rssi, snr, cqi;
    std::string earfcn;
};

class SharedBuffer {
public:
    SharedBuffer() : flags("1111") {}
    
    void addData(const TelemetryData& d) {
        std::lock_guard<std::mutex> lock(mtx);
        history.push_back(d);
    }

    std::vector<TelemetryData> consumeNewData() {
        std::lock_guard<std::mutex> lock(mtx);
        if (history.empty()) return {};
        std::vector<TelemetryData> temp = std::move(history);
        history.clear(); 
        return temp;
    }

    void setInitialHistory(const std::vector<TelemetryData>& history_data) {
        std::lock_guard<std::mutex> lock(mtx);
        history = history_data;
    }

    void setFlags(const std::string& new_flags) {
        std::lock_guard<std::mutex> lock(mtx);
        flags = new_flags;
    }

    std::string getFlags() {
        std::lock_guard<std::mutex> lock(mtx);
        return flags;
    }
    void setMapData(const std::vector<TelemetryData>& data) {
        std::lock_guard<std::mutex> lock(mtx);
        map_data = data;
    }

    std::vector<TelemetryData> getMapData() {
        std::lock_guard<std::mutex> lock(mtx);
        return map_data;
    }

private:
    std::mutex mtx;
    std::vector<TelemetryData> history;
    std::string flags; 
    std::vector<TelemetryData> map_data;
};
