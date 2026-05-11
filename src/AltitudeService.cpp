#include "headers/AltitudeService.h"

#include <curl/curl.h>
#include <libpq-fe.h>

#include <cmath>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <set>
#include <thread>
#include <chrono>

namespace {

size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const size_t total = size * nmemb;
    out->append(ptr, total);
    return total;
}

bool TryParseDouble(const std::string& s, double& out) {
    if (s.empty()) return false;
    try {
        size_t idx = 0;
        out = std::stod(s, &idx);
        return idx > 0 && std::isfinite(out);
    } catch (...) {
        return false;
    }
}

std::string CoordKey(const std::string& lat, const std::string& lon) {
    return lat + "," + lon;
}

} // namespace

namespace AltitudeService {

bool IsValidAltitude(const std::string& alt) {
    if (alt.empty()) return false;
    if (alt == "2147483647" || alt == "2147483648") return false;
    double v = 0.0;
    if (!TryParseDouble(alt, v)) return false;
    return std::abs(v) < 9000.0;
}

bool FetchElevationsOpenMeteo(const std::vector<std::pair<double, double>>& latLon,
                              std::vector<double>& outElevationsM) {
    outElevationsM.clear();
    if (latLon.empty()) return true;

    std::ostringstream url;
    url << "https://api.open-meteo.com/v1/elevation?latitude=";
    for (size_t i = 0; i < latLon.size(); ++i) {
        if (i > 0) url << ",";
        url << latLon[i].first;
    }
    url << "&longitude=";
    for (size_t i = 0; i < latLon.size(); ++i) {
        if (i > 0) url << ",";
        url << latLon[i].second;
    }

    static std::once_flag curlOnce;
    std::call_once(curlOnce, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "telemetry-app/1.0");

    const CURLcode code = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) {
        //std::cerr << "Elevation API: " << curl_easy_strerror(code) << "\n";
        return false;
    }

    const size_t keyPos = response.find("\"elevation\"");
    if (keyPos == std::string::npos) {
        std::cerr << "Elevation API: unexpected JSON\n";
        return false;
    }

    size_t arrStart = response.find('[', keyPos);
    if (arrStart == std::string::npos) return false;

    outElevationsM.reserve(latLon.size());
    size_t pos = arrStart + 1;
    while (pos < response.size() && outElevationsM.size() < latLon.size()) {
        while (pos < response.size() && (response[pos] == ' ' || response[pos] == '\n')) ++pos;
        if (pos >= response.size() || response[pos] == ']') break;
        if (response[pos] == ',') { ++pos; continue; }

        char* endPtr = nullptr;
        const double v = std::strtod(response.c_str() + pos, &endPtr);
        if (endPtr == response.c_str() + pos) break;
        outElevationsM.push_back(v);
        pos = static_cast<size_t>(endPtr - response.c_str());
    }

    return outElevationsM.size() == latLon.size();
}

static void UpdateDbAltitude(PGconn* con, const std::string& lat, const std::string& lon, double altM) {
  const char* params[3];
  std::string altStr = std::to_string(altM);
  params[0] = altStr.c_str();
  params[1] = lat.c_str();
  params[2] = lon.c_str();

  PGresult* res = PQexecParams(con,
      "UPDATE telemetry SET alt = $1::double precision "
      "WHERE abs(lat::double precision - $2::double precision) < 1e-5 "
      "AND abs(lon::double precision - $3::double precision) < 1e-5 "
      "AND (alt IS NULL OR alt = 0)",
      3, nullptr, params, nullptr, nullptr, 0);
  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
      std::cerr << "Altitude UPDATE: " << PQresultErrorMessage(res) << "\n";
  }
  PQclear(res);
}

void BackfillMissingAltitudes(PGconn* con, std::vector<TelemetryData>& rows) {
    if (!con || PQstatus(con) != CONNECTION_OK) return;
    std::map<std::string, std::pair<std::string, std::string>> missingByKey;
    for (const auto& row : rows) {
        if (IsValidAltitude(row.alt)) continue;
        double lat = 0.0, lon = 0.0;
        if (!TryParseDouble(row.lat, lat) || !TryParseDouble(row.lon, lon)) continue;
        const std::string key = CoordKey(row.lat, row.lon);
        missingByKey.emplace(key, std::make_pair(row.lat, row.lon));
    }

    if (missingByKey.empty()) return;

    std::vector<std::string> keys;
    std::vector<std::pair<double, double>> coords;
    keys.reserve(missingByKey.size());
    coords.reserve(missingByKey.size());

    for (const auto& [key, latLon] : missingByKey) {
        double lat = 0.0, lon = 0.0;
        if (!TryParseDouble(latLon.first, lat) || !TryParseDouble(latLon.second, lon)) continue;
        keys.push_back(key);
        coords.emplace_back(lat, lon);
    }

    constexpr size_t kBatch = 80;
    std::map<std::string, double> resolved;

    for (size_t offset = 0; offset < coords.size(); offset += kBatch) {
        const size_t count = std::min(kBatch, coords.size() - offset);
        std::vector<std::pair<double, double>> batch(coords.begin() + offset, coords.begin() + offset + count);
        std::vector<double> elevations;
        
        if (!FetchElevationsOpenMeteo(batch, elevations)) {
            std::cerr << "Open-Meteo API backfill aborted to prevent rate-limit ban.\n";
            break; // Защита от спама и блокировок
        }

        for (size_t i = 0; i < count; ++i) {
            const std::string& key = keys[offset + i];
            const auto& latLon = missingByKey[key];
            const double altM = elevations[i];
            resolved[key] = altM;
            UpdateDbAltitude(con, latLon.first, latLon.second, altM);
        }
        
        // Пауза 500мс между пачками чтобы API не забанил по лимитам
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    for (auto& row : rows) {
        const std::string key = CoordKey(row.lat, row.lon);
        const auto it = resolved.find(key);
        if (it != resolved.end()) {
            row.alt = std::to_string(it->second);
        }
    }

    if (!resolved.empty()) {
        std::cout << "Altitude: filled " << resolved.size() << " coordinate(s) from Open-Meteo.\n";
    }
}

} // namespace AltitudeService
