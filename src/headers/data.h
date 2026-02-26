#pragma once
#include <string>
#include <vector>
#include <mutex>

struct TelemetryData {
    std::string lat;
    std::string lon;
    std::string signal;
    std::string timestamp;
};

class SharedBuffer {
public:
    SharedBuffer() = default;
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

    std::vector<TelemetryData> getHistoryCopy() {
        std::lock_guard<std::mutex> lock(mtx);
        return history;
    }

    void setInitialHistory(const std::vector<TelemetryData>& history_data) {
        std::lock_guard<std::mutex> lock(mtx);
        history = history_data;
    }

private:
    std::mutex mtx;
    std::vector<TelemetryData> history;
};
