#pragma once

#include <functional>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <deque>
#include <thread>
#include <memory>

#include "imgui.h"
#include "headers/data.h"
#include "headers/MapManager.h"

class MapWindow {
public:
    enum class HeatmapCriterion : int { RSRP = 0, RSRQ, RSSI, Altitude };

    using Loader = std::function<std::vector<TelemetryData>()>;

    explicit MapWindow(Loader loader);
    ~MapWindow();

    void Reload();
    void Render();
    bool IsOpen() const { return open; }

private:
    struct PointItem {
        double lat = 0.0, lon = 0.0;
        double worldX = 0.0, worldY = 0.0;
        double signalValue = 0.0;
        double rsrp = 0.0, rsrqVal = 0.0, rssiVal = 0.0, altM = 0.0;
        bool hasRsrp = false, hasRsrq = false, hasRssi = false, hasAlt = false;
        int pciVal = -1;
        bool hasPci = false;
        std::string earfcnKey;
        std::string timestamp, net_type, signal;
        std::string band, cell_id, pci, tac, mcc, mnc, rsrqStr, rssiStr, snr, cqi, altStr;
    };

    struct HeatmapCell {
        double sumValue = 0.0;
        double sumWeights = 0.0;
    };

    struct HeatmapAsyncJob {
        std::deque<std::pair<std::pair<int,int>, std::vector<uint8_t>>> pendingTiles;
        std::atomic<bool> abort{false};
        std::mutex        mtx;
        bool              done = false;

        std::map<std::pair<int, int>, std::vector<uint8_t>> finalTiles;

        double            altMin = 0.0;
        double            altMax = 1.0;
        int               sourcePoints = 0;
        std::string       statusMsg;
    };

    Loader loader;
    MapManager tiles;
    std::vector<PointItem> points;

    std::map<std::pair<int, int>, GLuint> heatmapTextures;

    bool open = true;
    bool needFit = true;
    bool followLatest = false;
    bool useHeatmap = true;

    int zoom = 13;
    const int kHeatmapBaseZoom = 14;

    ImVec2 pan_px = ImVec2(0.0f, 0.0f);
    double centerWorldX = 0.5;
    double centerWorldY = 0.5;

    int hoveredIndex = -1;
    int selectedIndex = -1;

    std::vector<int> heatmapPciChoices;
    int heatmapPciChoiceIdx = 0;
    std::vector<std::string> heatmapEarfcnChoices;
    int heatmapEarfcnChoiceIdx = 0;
    HeatmapCriterion heatmapCriterion = HeatmapCriterion::RSRP;
    float heatmapInterpRadiusM = 25.0f;
    double heatmapLastAltMin = 0.0;
    double heatmapLastAltMax = 1.0;
    int heatmapLastSourcePoints = 0;
    int heatmapLastTileCount = 0;
    std::string heatmapStatusMsg;

    std::shared_ptr<HeatmapAsyncJob> heatmapJob;

    void BuildPoints(const std::vector<TelemetryData>& raw);
    void RefreshHeatmapComboLists();
    void FitToData(const ImVec2& canvasSize);
    void DrawCanvas(const ImVec2& canvasPos, const ImVec2& canvasSize);

    std::string HeatmapCacheDir() const;
    void GenerateHeatmapTiles();
    void UploadPendingHeatmapTiles();
    void LoadHeatmapTiles();
    void ClearHeatmapTextures();

    static bool TryParseDouble(const std::string& s, double& out);
    static bool TryParseInt(const std::string& s, int& out);
    static bool IsValidPci(int pci);
    void EnsureHeatmapReady();
    static double MetersPerPixelAtLat(double latDeg, int zoom);
    static std::string SanitizePathToken(const std::string& s);

    bool GetPointMetric(const PointItem& p, HeatmapCriterion c, double& out) const;
    bool PointMatchesHeatmapFilter(const PointItem& p) const;
    static ImU32 ColorForHeatmapValue(HeatmapCriterion c, double v, double altMin, double altMax);
    static float SignalToRadius(double s);
    static std::string ShortText(const std::string& s, size_t maxLen);
    void DrawPointDetails(const PointItem& p);
    void DrawLegend();
};
