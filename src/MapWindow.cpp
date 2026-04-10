#include "headers/MapWindow.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_IMPLEMENTATION
#define R_TO_D(x) ((x) * 180.0 / kPi)
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <set>

static constexpr float kTileSize = 256.0f;
static constexpr int kMinZoom = 1;
static constexpr int kMaxZoom = 19;
static constexpr double kPi = 3.14159265358979323846;
static constexpr int kHeatmapComputeZoom = 15;
static constexpr int kHeatmapTextureSize = 1024;

static std::string TrimCopy(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

MapWindow::MapWindow(Loader l) : loader(std::move(l)) {
    Reload();
    EnsureHeatmapReady();
}

MapWindow::~MapWindow() {
    ClearHeatmapTextures();
}

void MapWindow::ClearHeatmapTextures() {
    for (auto& [id, tex] : heatmapTextures) {
        glDeleteTextures(1, &tex);
    }
    heatmapTextures.clear();
}

bool MapWindow::TryParseDouble(const std::string& s, double& out) {
    try {
        size_t idx = 0;
        out = std::stod(s, &idx);
        return idx > 0;
    } catch (...) { return false; }
}

bool MapWindow::TryParseInt(const std::string& s, int& out) {
    try {
        size_t idx = 0;
        long v = std::stol(s, &idx, 10);
        if (idx == 0) return false;
        out = static_cast<int>(v);
        return true;
    } catch (...) { return false; }
}

bool MapWindow::IsValidPci(int pci) {
    return pci >= 0 && pci <= 1007 && pci != 2147483647;
}

double MapWindow::MetersPerPixelAtLat(double latDeg, int zoom) {
    const double earthCircumference = 40075017.0;
    const double latRad = latDeg * kPi / 180.0;
    return std::cos(latRad) * earthCircumference / (256.0 * static_cast<double>(1 << zoom));
}

std::string MapWindow::SanitizePathToken(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char uc : s) {
        char c = static_cast<char>(uc);
        if (std::isalnum(uc)) out.push_back(c);
        else if (!out.empty() && out.back() != '_') out.push_back('_');
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "x" : out;
}

ImU32 MapWindow::ColorForHeatmapValue(HeatmapCriterion c, double v, double altMin, double altMax) {
    auto lerpChannel = [](uint8_t a, uint8_t b, float t) -> uint8_t {
        return static_cast<uint8_t>(std::lround(a + (b - a) * t));
    };
    auto lerpColor = [&](ImU32 ca, ImU32 cb, float t) -> ImU32 {
        uint8_t ra = (ca >> IM_COL32_R_SHIFT) & 0xFF, ga = (ca >> IM_COL32_G_SHIFT) & 0xFF, ba = (ca >> IM_COL32_B_SHIFT) & 0xFF;
        uint8_t rb = (cb >> IM_COL32_R_SHIFT) & 0xFF, gb = (cb >> IM_COL32_G_SHIFT) & 0xFF, bb = (cb >> IM_COL32_B_SHIFT) & 0xFF;
        return IM_COL32(lerpChannel(ra, rb, t), lerpChannel(ga, gb, t), lerpChannel(ba, bb, t), 255);
    };

    switch (c) {
    case HeatmapCriterion::RSRP:
        if (!std::isfinite(v)) return IM_COL32(100, 100, 100, 255);
        if (v >= -80.0)  return IM_COL32(255, 0, 0, 255);
        if (v >= -90.0)  return IM_COL32(255, 165, 0, 255);
        if (v >= -100.0) return IM_COL32(0, 255, 0, 255);
        if (v >= -110.0) return IM_COL32(0, 255, 255, 255);
        return IM_COL32(0, 0, 255, 255);

    case HeatmapCriterion::RSRQ:
        if (!std::isfinite(v)) return IM_COL32(100, 100, 100, 255);
        if (v >= -5.0)   return IM_COL32(255, 0, 0, 255);
        if (v >= -8.0)   return IM_COL32(255, 165, 0, 255);
        if (v >= -12.0)  return IM_COL32(0, 255, 0, 255);
        if (v >= -15.0)  return IM_COL32(0, 255, 255, 255);
        return IM_COL32(0, 0, 255, 255);

    case HeatmapCriterion::RSSI:
        if (!std::isfinite(v)) return IM_COL32(100, 100, 100, 255);
        if (v >= -65.0)  return IM_COL32(255, 0, 0, 255);
        if (v >= -75.0)  return IM_COL32(255, 165, 0, 255);
        if (v >= -85.0)  return IM_COL32(0, 255, 0, 255);
        if (v >= -95.0)  return IM_COL32(0, 255, 255, 255);
        return IM_COL32(0, 0, 255, 255);

    case HeatmapCriterion::Altitude:
    default:
        if (!std::isfinite(v) || !std::isfinite(altMin) || !std::isfinite(altMax)) return IM_COL32(100, 100, 100, 255);
        if (std::abs(altMax - altMin) < 1e-3) return IM_COL32(180, 180, 60, 255);
        {
            float t = static_cast<float>((v - altMin) / (altMax - altMin));
            t = std::clamp(t, 0.0f, 1.0f);
            ImU32 low = IM_COL32(0, 80, 200, 255);
            ImU32 high = IM_COL32(255, 60, 40, 255);
            return lerpColor(low, high, t);
        }
    }
}

float MapWindow::SignalToRadius(double s) {
    if (s >= -85.0) return 5.0f;
    if (s >= -105.0) return 4.5f;
    return 4.0f;
}

std::string MapWindow::ShortText(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen) + "...";
}

bool MapWindow::GetPointMetric(const PointItem& p, HeatmapCriterion c, double& out) const {
    switch (c) {
    case HeatmapCriterion::RSRP:
        if (!p.hasRsrp) return false;
        out = p.rsrp;
        return true;
    case HeatmapCriterion::RSRQ:
        if (!p.hasRsrq) return false;
        out = p.rsrqVal;
        return true;
    case HeatmapCriterion::RSSI:
        if (!p.hasRssi) return false;
        out = p.rssiVal;
        return true;
    case HeatmapCriterion::Altitude:
        if (!p.hasAlt) return false;
        out = p.altM;
        return true;
    default:
        return false;
    }
}

bool MapWindow::PointMatchesHeatmapFilter(const PointItem& p) const {
    if (heatmapPciChoiceIdx >= 0 && heatmapPciChoiceIdx < (int)heatmapPciChoices.size()) {
        int targetPci = heatmapPciChoices[heatmapPciChoiceIdx];
        if (targetPci != -1 && (!p.hasPci || p.pciVal != targetPci)) return false;
    }
    if (heatmapEarfcnChoiceIdx >= 0 && heatmapEarfcnChoiceIdx < (int)heatmapEarfcnChoices.size()) {
        const std::string& targetEar = heatmapEarfcnChoices[heatmapEarfcnChoiceIdx];
        if (targetEar != "All" && p.earfcnKey != targetEar) return false;
    }
    return true;
}

void MapWindow::BuildPoints(const std::vector<TelemetryData>& raw) {
    points.clear();
    points.reserve(raw.size());

    for (const auto& r : raw) {
        double lat = 0.0, lon = 0.0;
        if (!TryParseDouble(r.lat, lat) || !TryParseDouble(r.lon, lon)) continue;

        PointItem p;
        p.lat = lat;
        p.lon = lon;
        p.timestamp = r.timestamp;
        p.net_type = r.net_type;
        p.signal = r.signal;
        p.band = r.band;
        p.cell_id = r.cell_id;
        p.pci = r.pci;
        p.tac = r.tac;
        p.mcc = r.mcc;
        p.mnc = r.mnc;
        p.rsrqStr = r.rsrq;
        p.rssiStr = r.rssi;
        p.snr = r.snr;
        p.cqi = r.cqi;
        p.altStr = r.alt;

        if (TryParseDouble(r.signal, p.rsrp)) {
            p.hasRsrp = true;
            p.signalValue = p.rsrp;
        }
        if (TryParseDouble(r.rsrq, p.rsrqVal)) p.hasRsrq = true;
        if (TryParseDouble(r.rssi, p.rssiVal)) p.hasRssi = true;
        if (TryParseDouble(r.alt, p.altM)) p.hasAlt = true;

        int pciI = -1;
        if (TryParseInt(TrimCopy(r.pci), pciI) && IsValidPci(pciI)) {
            p.pciVal = pciI;
            p.hasPci = true;
        }

        std::string earKey = TrimCopy(r.earfcn);
        if (earKey.empty()) earKey = TrimCopy(r.band);
        p.earfcnKey = earKey.empty() ? std::string("?") : std::move(earKey);

        MapManager::LonLatToWebMercator(lon, lat, p.worldX, p.worldY);
        points.push_back(std::move(p));
    }

    if (!points.empty()) {
        centerWorldX = points.back().worldX;
        centerWorldY = points.back().worldY;
    }
    needFit = true;
    if (selectedIndex >= static_cast<int>(points.size())) selectedIndex = -1;
    if (hoveredIndex >= static_cast<int>(points.size())) hoveredIndex = -1;
}

void MapWindow::RefreshHeatmapComboLists() {
    std::set<int> pciSet;
    for (const auto& p : points) {
        if (p.hasPci) pciSet.insert(p.pciVal);
    }
    heatmapPciChoices.assign(pciSet.begin(), pciSet.end());
    heatmapPciChoices.insert(heatmapPciChoices.begin(), -1); // -1 означает "Все PCI"
    if (heatmapPciChoiceIdx >= static_cast<int>(heatmapPciChoices.size())) heatmapPciChoiceIdx = 0;

    std::set<std::string> earSet;
    int targetPci = heatmapPciChoices[heatmapPciChoiceIdx];
    for (const auto& p : points) {
        if (targetPci == -1 || (p.hasPci && p.pciVal == targetPci)) {
            earSet.insert(p.earfcnKey);
        }
    }
    heatmapEarfcnChoices.assign(earSet.begin(), earSet.end());
    heatmapEarfcnChoices.insert(heatmapEarfcnChoices.begin(), "All");
    if (heatmapEarfcnChoiceIdx >= static_cast<int>(heatmapEarfcnChoices.size())) heatmapEarfcnChoiceIdx = 0;
}

void MapWindow::Reload() {
    if (!loader) return;
    BuildPoints(loader());
    RefreshHeatmapComboLists();
    heatmapStatusMsg.clear();
}

void MapWindow::EnsureHeatmapReady() {
    LoadHeatmapTiles();
    if (useHeatmap && !points.empty() && heatmapTextures.empty() && !heatmapEarfcnChoices.empty()) {
        GenerateHeatmapTiles();
    }
}

void MapWindow::FitToData(const ImVec2& canvasSize) {
    if (points.empty() || canvasSize.x <= 10.0f || canvasSize.y <= 10.0f) return;

    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();

    for (const auto& p : points) {
        minX = std::min(minX, p.worldX);
        minY = std::min(minY, p.worldY);
        maxX = std::max(maxX, p.worldX);
        maxY = std::max(maxY, p.worldY);
    }

    const double rangeX = std::max(1e-8, maxX - minX);
    const double rangeY = std::max(1e-8, maxY - minY);
    const double padX = std::max(0.01, rangeX * 0.08);
    const double padY = std::max(0.01, rangeY * 0.08);

    const double desiredScaleX = std::max(1.0, (canvasSize.x - 24.0f) / (rangeX + padX * 2.0));
    const double desiredScaleY = std::max(1.0, (canvasSize.y - 24.0f) / (rangeY + padY * 2.0));
    const double desiredWorldScale = std::min(desiredScaleX, desiredScaleY);

    double zf = std::log2(desiredWorldScale / kTileSize);
    zoom = std::clamp((int)std::floor(zf), kMinZoom, kMaxZoom);
    centerWorldX = (minX + maxX) * 0.5;
    centerWorldY = (minY + maxY) * 0.5;
    pan_px = ImVec2(0.0f, 0.0f);
    needFit = false;
}

std::string MapWindow::HeatmapCacheDir() const {
    if (heatmapEarfcnChoices.empty()) return {};
    const std::string& ear = heatmapEarfcnChoices[heatmapEarfcnChoiceIdx];
    std::ostringstream oss;
    oss << "tiles_cache/heatmap/" << kHeatmapBaseZoom << "/";
    int pci = heatmapPciChoices.empty() ? -1 : heatmapPciChoices[heatmapPciChoiceIdx];
    
    if (pci == -1) oss << "pci_all";
    else oss << "pci" << pci;
    
    oss << "_e" << SanitizePathToken(ear)
        << "_c" << static_cast<int>(heatmapCriterion)
        << "_r" << static_cast<int>(std::lround(heatmapInterpRadiusM));
    return oss.str();
}

void MapWindow::LoadHeatmapTiles() {
    ClearHeatmapTextures();
    const std::string dir = HeatmapCacheDir();
    if (dir.empty() || !std::filesystem::exists(dir)) return;

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".png") {
            std::string filename = entry.path().stem().string();
            size_t delim = filename.find('_');
            if (delim != std::string::npos) {
                int tx = std::stoi(filename.substr(0, delim));
                int ty = std::stoi(filename.substr(delim + 1));

                int width, height, channels;
                unsigned char* img = stbi_load(entry.path().string().c_str(), &width, &height, &channels, 4);
                if (img) {
                    GLuint tex;
                    glGenTextures(1, &tex);
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    stbi_image_free(img);
                    heatmapTextures[{tx, ty}] = tex;
                }
            }
        }
    }
}

// =====================================================================================
// PROFESSIONAL GIS / TELECOM HEATMAP GENERATOR
// LTE / NR RF COVERAGE INTERPOLATION
// IDW + QUARTIC KERNEL + LOG DOMAIN AVERAGING + HD TILES
// =====================================================================================

void MapWindow::GenerateHeatmapTiles()
{
    heatmapLastSourcePoints = 0;
    heatmapLastTileCount = 0;

    if (points.empty() || heatmapEarfcnChoices.empty()) {
        heatmapStatusMsg = "Heatmap: no points or no EARFCN.";
        return;
    }

    ClearHeatmapTextures();



    const double scaleBase =
    static_cast<double>(kTileSize) *
    double(1LL << kHeatmapComputeZoom);

    const float scaleFactor =
        static_cast<float>(kHeatmapTextureSize) / kTileSize;

    const HeatmapCriterion crit = heatmapCriterion;

    const bool isAltitude =
        (crit == HeatmapCriterion::Altitude);

    // =========================================================================
    // PROFESSIONAL GIS PARAMETERS
    // =========================================================================

    const float radiusM =
        isAltitude
            ? 100.0f
            : std::clamp(heatmapInterpRadiusM, 15.0f, 45.0f);

    const double p_power =
        isAltitude
            ? 1.0
            : 1.45;

    // =========================================================================
    // LOG DOMAIN CONVERSION
    // =========================================================================

    auto MetricToLinear =
        [](HeatmapCriterion criterion, double val) -> double
    {
        if (criterion == HeatmapCriterion::Altitude)
            return val;

        return std::pow(10.0, val / 10.0);
    };

    auto LinearToMetric =
        [](HeatmapCriterion criterion, double val) -> double
    {
        if (criterion == HeatmapCriterion::Altitude)
            return val;

        if (val <= 1e-20)
            return -140.0;

        return 10.0 * std::log10(val);
    };

    // =========================================================================
    // ALTITUDE QUANTILE NORMALIZATION
    // =========================================================================

    std::vector<double> validValues;

    for (const auto& p : points)
    {
        if (!PointMatchesHeatmapFilter(p))
            continue;

        double v = 0.0;

        if (GetPointMetric(p, crit, v))
            validValues.push_back(v);
    }

    if (validValues.empty()) {
        heatmapStatusMsg = "Heatmap: no valid source points.";
        return;
    }

    std::sort(validValues.begin(), validValues.end());

    if (isAltitude)
    {
        size_t lowIdx =
            static_cast<size_t>(
                validValues.size() * 0.01);

        size_t highIdx =
            static_cast<size_t>(
                validValues.size() * 0.99);

        heatmapLastAltMin = validValues[lowIdx];
        heatmapLastAltMax = validValues[highIdx];

        if (std::abs(heatmapLastAltMax - heatmapLastAltMin) < 0.01)
            heatmapLastAltMax = heatmapLastAltMin + 1.0;
    }

    // =========================================================================
    // TILE STORAGE
    // =========================================================================

    std::map<
        std::pair<int, int>,
        std::vector<HeatmapCell>
    > tileGrids;

    // =========================================================================
    // MAIN IDW INTERPOLATION
    // =========================================================================

    for (const auto& p : points)
    {
        if (!PointMatchesHeatmapFilter(p))
            continue;

        double metric = 0.0;

        if (!GetPointMetric(p, crit, metric))
            continue;

        ++heatmapLastSourcePoints;

        const double mpp =
            MetersPerPixelAtLat(
                p.lat,
                kHeatmapComputeZoom);

        if (mpp <= 1e-12)
            continue;

        const float Rpx =
            static_cast<float>(radiusM / mpp);

        if (Rpx < 1.0f)
            continue;

        // =====================================================================
        // HD COORDINATES
        // =====================================================================

        const float px =
            static_cast<float>(p.worldX * scaleBase)
            * scaleFactor;

        const float py =
            static_cast<float>(p.worldY * scaleBase)
            * scaleFactor;

        const float RpxHD =
            Rpx * scaleFactor;

        const float R2HD =
            RpxHD * RpxHD;

        const int startX =
            static_cast<int>(std::floor(px - RpxHD));

        const int endX =
            static_cast<int>(std::ceil(px + RpxHD));

        const int startY =
            static_cast<int>(std::floor(py - RpxHD));

        const int endY =
            static_cast<int>(std::ceil(py + RpxHD));

        const double metricLinear =
            MetricToLinear(crit, metric);

        // =====================================================================
        // LOCAL PLANAR APPROXIMATION
        // MUCH FASTER THAN HAVERSINE
        // =====================================================================

        const double metersPerDegLat = 111320.0;

        const double metersPerDegLon =
            std::cos(p.lat * kPi / 180.0)
            * 111320.0;

        for (int y = startY; y <= endY; ++y)
        {
            const float dy =
                static_cast<float>(y) - py;

            for (int x = startX; x <= endX; ++x)
            {
                const float dx =
                    static_cast<float>(x) - px;

                const float d2 =
                    dx * dx + dy * dy;

                if (d2 > R2HD)
                    continue;

                // =============================================================
                // PIXEL -> WORLD
                // =============================================================

                double pixelWorldX =
                    (static_cast<double>(x) / scaleFactor)
                    / scaleBase;

                double pixelWorldY =
                    (static_cast<double>(y) / scaleFactor)
                    / scaleBase;

                // =============================================================
                // WORLD -> GEO
                // =============================================================

                double pixLon =
                    pixelWorldX * 360.0 - 180.0;

                double e =
                    std::exp(
                        (0.5 - pixelWorldY)
                        * 2.0
                        * kPi);

                double pixLat =
                    R_TO_D(
                        std::atan(
                            0.5 * (e - 1.0 / e)));

                // =============================================================
                // FAST LOCAL METRIC DISTANCE
                // =============================================================

                double dxm =
                    (pixLon - p.lon)
                    * metersPerDegLon;

                double dym =
                    (pixLat - p.lat)
                    * metersPerDegLat;

                double dm =
                    std::sqrt(
                        dxm * dxm
                        + dym * dym);

                if (dm > radiusM)
                    continue;

                // =============================================================
                // PROFESSIONAL RF IDW
                // =============================================================

                double weight;

                if (dm < 0.05)
                {
                    weight = 1e12;
                }
                else
                {
                    // =========================================================
                    // IDW CORE
                    // =========================================================

                    weight =
                        1.0 /
                        std::pow(dm + 0.1, p_power);

                    // =========================================================
                    // QUARTIC KERNEL
                    // =========================================================

                    double rNorm =
                        dm / static_cast<double>(radiusM);

                    double kernel =
                        1.0 - (rNorm * rNorm);

                    kernel =
                        std::max(kernel, 0.0);

                    kernel *= kernel;

                    weight *= kernel;
                }

                // =============================================================
                // TILE INDEXING
                // =============================================================

                int tx =
                    (x >= 0)
                        ? (x / kHeatmapTextureSize)
                        : ((x - (kHeatmapTextureSize - 1))
                           / kHeatmapTextureSize);

                int ty =
                    (y >= 0)
                        ? (y / kHeatmapTextureSize)
                        : ((y - (kHeatmapTextureSize - 1))
                           / kHeatmapTextureSize);

                int lx =
                    x % kHeatmapTextureSize;

                if (lx < 0)
                    lx += kHeatmapTextureSize;

                int ly =
                    y % kHeatmapTextureSize;

                if (ly < 0)
                    ly += kHeatmapTextureSize;

                auto& grid =
                    tileGrids[{tx, ty}];

                if (grid.empty())
                {
                    grid.resize(
                        kHeatmapTextureSize
                        * kHeatmapTextureSize,
                        HeatmapCell{0.0, 0.0});
                }

                const int idx =
                    ly * kHeatmapTextureSize + lx;

                // =============================================================
                // FORMULA (1)
                // =============================================================

                grid[idx].sumValue +=
                    metricLinear * weight;

                grid[idx].sumWeights +=
                    weight;
            }
        }
    }

    // =========================================================================
    // OUTPUT DIR
    // =========================================================================

    const std::string outDir =
        HeatmapCacheDir();

    if (outDir.empty()) {
        heatmapStatusMsg = "Heatmap cache path empty.";
        return;
    }

    std::filesystem::create_directories(outDir);

    // =========================================================================
    // RASTERIZATION
    // =========================================================================

    for (auto& [tID, grid] : tileGrids)
    {
        std::vector<uint8_t> rgba(
            kHeatmapTextureSize
            * kHeatmapTextureSize
            * 4,
            0);

        // =====================================================================
        // PIXEL LOOP
        // =====================================================================

        for (int i = 0;
             i < kHeatmapTextureSize * kHeatmapTextureSize;
             ++i)
        {
            if (grid[i].sumWeights < 1e-8)
                continue;

            // =============================================================
            // FINAL LOG DOMAIN RESTORE
            // =============================================================

            const double valLinear =
                grid[i].sumValue
                / grid[i].sumWeights;

            const double val =
                LinearToMetric(
                    crit,
                    valLinear);

            // =============================================================
            // COLOR
            // =============================================================

            const ImU32 col =
                ColorForHeatmapValue(
                    crit,
                    val,
                    heatmapLastAltMin,
                    heatmapLastAltMax);

            // =============================================================
            // PROFESSIONAL ALPHA MODEL
            // =============================================================

            uint8_t alpha;

            if (isAltitude)
            {
                alpha = 220;
            }
            else
            {
                float density =
                    static_cast<float>(
                        grid[i].sumWeights);

                density =
                    std::min(
                        density * 0.55f,
                        1.0f);

                density =
                    std::pow(density, 0.65f);

                alpha =
                    static_cast<uint8_t>(
                        density * 190.0f);
            }

            rgba[i * 4 + 0] =
                static_cast<uint8_t>(
                    (col >> IM_COL32_R_SHIFT) & 0xFF);

            rgba[i * 4 + 1] =
                static_cast<uint8_t>(
                    (col >> IM_COL32_G_SHIFT) & 0xFF);

            rgba[i * 4 + 2] =
                static_cast<uint8_t>(
                    (col >> IM_COL32_B_SHIFT) & 0xFF);

            rgba[i * 4 + 3] =
                alpha;
        }

        // =====================================================================
        // PROFESSIONAL POST BLUR
        // =====================================================================

        std::vector<uint8_t> temp = rgba;

        constexpr int blurRadius = 2;

        // horizontal
        for (int y = 0; y < kHeatmapTextureSize; ++y)
        {
            for (int x = 0; x < kHeatmapTextureSize; ++x)
            {
                int r = 0;
                int g = 0;
                int b = 0;
                int a = 0;
                int c = 0;

                for (int k = -blurRadius;
                     k <= blurRadius;
                     ++k)
                {
                    int xx = x + k;

                    if (xx < 0 ||
                        xx >= kHeatmapTextureSize)
                        continue;

                    int idx =
                        (y * kHeatmapTextureSize + xx) * 4;

                    r += temp[idx + 0];
                    g += temp[idx + 1];
                    b += temp[idx + 2];
                    a += temp[idx + 3];

                    ++c;
                }

                int out =
                    (y * kHeatmapTextureSize + x) * 4;

                rgba[out + 0] = r / c;
                rgba[out + 1] = g / c;
                rgba[out + 2] = b / c;
                rgba[out + 3] = a / c;
            }
        }

        // vertical
        temp = rgba;

        for (int y = 0; y < kHeatmapTextureSize; ++y)
        {
            for (int x = 0; x < kHeatmapTextureSize; ++x)
            {
                int r = 0;
                int g = 0;
                int b = 0;
                int a = 0;
                int c = 0;

                for (int k = -blurRadius;
                     k <= blurRadius;
                     ++k)
                {
                    int yy = y + k;

                    if (yy < 0 ||
                        yy >= kHeatmapTextureSize)
                        continue;

                    int idx =
                        (yy * kHeatmapTextureSize + x) * 4;

                    r += temp[idx + 0];
                    g += temp[idx + 1];
                    b += temp[idx + 2];
                    a += temp[idx + 3];

                    ++c;
                }

                int out =
                    (y * kHeatmapTextureSize + x) * 4;

                rgba[out + 0] = r / c;
                rgba[out + 1] = g / c;
                rgba[out + 2] = b / c;
                rgba[out + 3] = a / c;
            }
        }

        // =====================================================================
        // SAVE PNG
        // =====================================================================

        const std::string path =
            outDir
            + "/"
            + std::to_string(tID.first)
            + "_"
            + std::to_string(tID.second)
            + ".png";

        stbi_write_png(
            path.c_str(),
            kHeatmapTextureSize,
            kHeatmapTextureSize,
            4,
            rgba.data(),
            kHeatmapTextureSize * 4);

        // =====================================================================
        // OPENGL
        // =====================================================================

        GLuint tex;

        glGenTextures(1, &tex);

        glBindTexture(GL_TEXTURE_2D, tex);

        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            kHeatmapTextureSize,
            kHeatmapTextureSize,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            rgba.data());

        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_MIN_FILTER,
            GL_LINEAR);

        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_MAG_FILTER,
            GL_LINEAR);

        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_WRAP_S,
            GL_CLAMP_TO_EDGE);

        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_WRAP_T,
            GL_CLAMP_TO_EDGE);

        heatmapTextures[tID] = tex;

        ++heatmapLastTileCount;
    }

    // =========================================================================
    // STATUS
    // =========================================================================

    heatmapStatusMsg =
        "Heatmap OK: "
        + std::to_string(heatmapLastSourcePoints)
        + " points, "
        + std::to_string(heatmapLastTileCount)
        + " GIS RF tiles.";
}

void MapWindow::DrawPointDetails(const PointItem& p) {
    ImGui::Text("Network Type: %s", p.net_type.c_str());
    ImGui::Text("Signal (RSRP): %s dBm", p.signal.c_str());
    ImGui::Separator();
    if (!p.cell_id.empty()) ImGui::BulletText("Cell ID: %s", p.cell_id.c_str());
    if (!p.band.empty()) ImGui::BulletText("Band: %s", p.band.c_str());
    if (!p.earfcnKey.empty()) ImGui::BulletText("EARFCN / band: %s", p.earfcnKey.c_str());
    if (!p.pci.empty()) ImGui::BulletText("PCI: %s", p.pci.c_str());
    if (!p.tac.empty()) ImGui::BulletText("TAC: %s", p.tac.c_str());
    if (!p.mcc.empty() && !p.mnc.empty()) ImGui::BulletText("PLMN: %s-%s", p.mcc.c_str(), p.mnc.c_str());
    if (!p.rsrqStr.empty()) ImGui::BulletText("RSRQ: %s dB", p.rsrqStr.c_str());
    if (!p.rssiStr.empty()) ImGui::BulletText("RSSI: %s dBm", p.rssiStr.c_str());
    if (!p.altStr.empty()) ImGui::BulletText("Altitude: %s m", p.altStr.c_str());
    if (!p.snr.empty()) ImGui::BulletText("SNR: %s dB", p.snr.c_str());
    if (!p.cqi.empty()) ImGui::BulletText("CQI: %s", p.cqi.c_str());
}

void MapWindow::DrawLegend() {
    const HeatmapCriterion c = heatmapCriterion;
    if (c == HeatmapCriterion::RSRP) {
        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "RSRP (dBm)");
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + ImGui::GetContentRegionAvail().x, p0.y + 20);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const int segments = 4;
        const float stepX = (p1.x - p0.x) / segments;
        const ImU32 colors[] = {
            IM_COL32(0, 0, 255, 255),
            IM_COL32(0, 255, 255, 255),
            IM_COL32(0, 255, 0, 255),
            IM_COL32(255, 165, 0, 255),
            IM_COL32(255, 0, 0, 255)
        };
        for (int i = 0; i < segments; i++) {
            ImVec2 rectMin(p0.x + i * stepX, p0.y);
            ImVec2 rectMax(p0.x + (i + 1) * stepX, p1.y);
            draw_list->AddRectFilledMultiColor(rectMin, rectMax, colors[i], colors[i + 1], colors[i + 1], colors[i]);
        }
        ImGui::Dummy(ImVec2(0, 25));
        ImGui::Text("<-110");
        ImGui::SameLine(stepX - 15);   ImGui::Text("-100");
        ImGui::SameLine(stepX * 2 - 15); ImGui::Text("-90");
        ImGui::SameLine(stepX * 3 - 15); ImGui::Text("-80");
        ImGui::SameLine(p1.x - p0.x - 30); ImGui::Text(">-80");
    } else if (c == HeatmapCriterion::RSRQ) {
        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "RSRQ (dB), лучше ближе к 0");
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + ImGui::GetContentRegionAvail().x, p0.y + 20);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilledMultiColor(p0, p1,
            IM_COL32(0, 0, 255, 255), IM_COL32(0, 255, 255, 255),
            IM_COL32(255, 0, 0, 255), IM_COL32(0, 255, 0, 255));
        ImGui::Dummy(ImVec2(0, 25));
        ImGui::Text("~-20");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.5f - 10); ImGui::Text("~-10");
        ImGui::SameLine(0); ImGui::SameLine(ImGui::GetContentRegionAvail().x - 40); ImGui::Text("~-3");
    } else if (c == HeatmapCriterion::RSSI) {
        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "RSSI (dBm)");
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + ImGui::GetContentRegionAvail().x, p0.y + 20);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilledMultiColor(p0, p1,
            IM_COL32(0, 0, 255, 255), IM_COL32(0, 255, 255, 255),
            IM_COL32(0, 255, 0, 255), IM_COL32(255, 0, 0, 255));
        ImGui::Dummy(ImVec2(0, 25));
        ImGui::Text("-95..-85");
        ImGui::SameLine(); ImGui::TextDisabled("(сильнее — теплее)");
    } else {
        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "Altitude (m), selected PCI/EARFCN");
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + ImGui::GetContentRegionAvail().x, p0.y + 20);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilledMultiColor(p0, p1,
            IM_COL32(0, 80, 200, 255), IM_COL32(0, 200, 200, 255),
            IM_COL32(255, 200, 60, 255), IM_COL32(255, 60, 40, 255));
        ImGui::Dummy(ImVec2(0, 25));
        ImGui::Text("min: %.1f", heatmapLastAltMin);
        ImGui::SameLine();
        ImGui::Text("max: %.1f", heatmapLastAltMax);
    }
}

void MapWindow::DrawCanvas(const ImVec2& canvasPos, const ImVec2& canvasSize) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 canvasMax(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);

    dl->PushClipRect(canvasPos, canvasMax, true);
    dl->AddRectFilled(canvasPos, canvasMax, IM_COL32(16, 18, 22, 255), 10.0f);

    const double worldScale = kTileSize * double(1 << zoom);
    const ImVec2 worldCenterPx(float(centerWorldX * worldScale), float(centerWorldY * worldScale));
    ImVec2 originPx(worldCenterPx.x - canvasSize.x * 0.5f - pan_px.x, worldCenterPx.y - canvasSize.y * 0.5f - pan_px.y);

    const int tilesPerAxis = 1 << zoom;
    int tileX0 = (int)std::floor(originPx.x / kTileSize) - 1;
    int tileY0 = (int)std::floor(originPx.y / kTileSize) - 1;
    int tileX1 = (int)std::floor((originPx.x + canvasSize.x) / kTileSize) + 1;
    int tileY1 = (int)std::floor((originPx.y + canvasSize.y) / kTileSize) + 1;

    for (int ty = tileY0; ty <= tileY1; ++ty) {
        if (ty < 0 || ty >= tilesPerAxis) continue;
        for (int tx = tileX0; tx <= tileX1; ++tx) {
            int wrappedX = tx % tilesPerAxis;
            if (wrappedX < 0) wrappedX += tilesPerAxis;

            GLuint tex = tiles.GetTileTexture(wrappedX, ty, zoom);
            if (!tex) continue;

            const float x0 = canvasPos.x + float(tx * kTileSize - originPx.x);
            const float y0 = canvasPos.y + float(ty * kTileSize - originPx.y);
            dl->AddImage((ImTextureID)(intptr_t)tex, ImVec2(x0, y0), ImVec2(x0 + kTileSize, y0 + kTileSize));
        }
    }

    if (useHeatmap && !heatmapTextures.empty()) {
        const double baseZoomScale =
    static_cast<double>(kHeatmapTextureSize) *
    double(1LL << kHeatmapComputeZoom);
        const double currentScale = worldScale;
        const float renderScale =
    static_cast<float>(
        currentScale /
        (kTileSize * double(1LL << kHeatmapComputeZoom))
    );
        const float renderSize = kTileSize * renderScale;
        const ImU32 heatTint = IM_COL32(255, 255, 255, 200);

        for (const auto& [tID, tex] : heatmapTextures) {
            double tileWorldX =
    static_cast<double>(tID.first) /
    double(1LL << kHeatmapComputeZoom);

double tileWorldY =
    static_cast<double>(tID.second) /
    double(1LL << kHeatmapComputeZoom);

float sxf = canvasPos.x + float(tileWorldX * currentScale - originPx.x);
float syf = canvasPos.y + float(tileWorldY * currentScale - originPx.y);

// Ceil для начала и floor+1 для конца — тайлы перекрываются на 1px, зазоры исчезают
float sx = std::floor(sxf);
float sy = std::floor(syf);
float ex = std::floor(sxf + renderSize) + 1.0f;
float ey = std::floor(syf + renderSize) + 1.0f;

if (ex < canvasPos.x || sx > canvasMax.x || ey < canvasPos.y || sy > canvasMax.y) continue;

dl->AddImage((ImTextureID)(intptr_t)tex, ImVec2(sx, sy), ImVec2(ex, ey),
    ImVec2(0, 0), ImVec2(1, 1), heatTint);
        }
    }
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    hoveredIndex = -1;
    float bestDist2 = 16.0f * 16.0f;

    for (int i = 0; i < (int)points.size(); ++i) {
        const auto& p = points[i];
        const float sx = canvasPos.x + float(p.worldX * worldScale - originPx.x);
        const float sy = canvasPos.y + float(p.worldY * worldScale - originPx.y);

        if (sx < canvasPos.x - 20.0f || sx > canvasMax.x + 20.0f ||
            sy < canvasPos.y - 20.0f || sy > canvasMax.y + 20.0f) continue;

// Применяем фильтр КО ВСЕМ ТОЧКАМ (не только к Heatmap)
        bool pointVisible = PointMatchesHeatmapFilter(p);
        if (!pointVisible) continue;

        if (!useHeatmap) {
            const float radius = SignalToRadius(p.signalValue);
            const ImU32 fill = ColorForHeatmapValue(HeatmapCriterion::RSRP, p.signalValue, 0.0, 1.0);
            
            // Используем 6 сегментов вместо 16! Это в 3 раза снизит нагрузку на OpenGL/CPU
            // при отрисовке десятков тысяч кружков, избавив от лагов интерфейса.
            dl->AddCircleFilled(ImVec2(sx, sy), radius, fill, 6);
            dl->AddCircle(ImVec2(sx, sy), radius, IM_COL32(0, 0, 0, 255), 6, 1.0f);
        }
        const float dx = mouse.x - sx;
        const float dy = mouse.y - sy;
        const float d2 = dx * dx + dy * dy;

        if (d2 < bestDist2) {
            bestDist2 = d2;
            hoveredIndex = i;
        }
    }

    if (hoveredIndex >= 0 && ImGui::IsMouseHoveringRect(canvasPos, canvasMax)) {
        const auto& p = points[hoveredIndex];
        ImGui::SetNextWindowBgAlpha(0.96f);
        ImGui::BeginTooltip();
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 1.0f, 1.0f), "Telemetry point details");
        ImGui::Separator();
        ImGui::Text("Time: %s", p.timestamp.c_str());
        ImGui::Text("Net: %s | Signal: %s dBm", p.net_type.c_str(), p.signal.c_str());
        ImGui::TextDisabled("Click area to pin details in panel");
        ImGui::EndTooltip();

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            selectedIndex = hoveredIndex;
        }
    }

    if (followLatest && !points.empty()) {
        selectedIndex = (int)points.size() - 1;
    }

    dl->PopClipRect();
}

void MapWindow::Render() {
    if (!open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::SetNextWindowSize(ImVec2(1280, 880), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("OSM Map", &open, flags)) {
        ImGui::End();
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    if (ImGui::Button("Reload Points")) Reload();
    ImGui::SameLine();

    if (ImGui::Button("Regenerate Heatmap Cache")) {
        GenerateHeatmapTiles();
    }
    ImGui::SameLine();

    if (ImGui::Button("Fit all")) needFit = true;
    ImGui::SameLine();
    if (ImGui::Button("Latest")) {
        if (!points.empty()) {
            selectedIndex = (int)points.size() - 1;
            centerWorldX = points.back().worldX;
            centerWorldY = points.back().worldY;
            pan_px = ImVec2(0.0f, 0.0f);
            needFit = false;
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow latest", &followLatest);
    ImGui::SameLine();
    ImGui::Checkbox("Layer: Heatmap", &useHeatmap);
    ImGui::SameLine();
    ImGui::SliderInt("Zoom", &zoom, kMinZoom, kMaxZoom);
    ImGui::PopStyleVar();

    ImGui::Separator();
    ImGui::TextDisabled("Heatmap: one PCI (if present) and one EARFCN/band. RSRP/RSRQ/RSSI do not require altitude.");
if (!heatmapPciChoices.empty()) {
        int selPci = heatmapPciChoices[heatmapPciChoiceIdx];
        char pciBuf[32];
        if (selPci == -1) std::snprintf(pciBuf, sizeof(pciBuf), "PCI: Все");
        else std::snprintf(pciBuf, sizeof(pciBuf), "PCI %d", selPci);
        
        if (ImGui::BeginCombo("PCI (filter)", pciBuf)) {
            for (int i = 0; i < static_cast<int>(heatmapPciChoices.size()); ++i) {
                const bool sel = (i == heatmapPciChoiceIdx);
                char label[48];
                if (heatmapPciChoices[i] == -1) std::snprintf(label, sizeof(label), "Все PCI");
                else std::snprintf(label, sizeof(label), "PCI %d", heatmapPciChoices[i]);

                if (ImGui::Selectable(label, sel)) {
                    heatmapPciChoiceIdx = i;
                    heatmapEarfcnChoiceIdx = 0;
                    RefreshHeatmapComboLists();
                    LoadHeatmapTiles();
                    if (heatmapTextures.empty()) GenerateHeatmapTiles();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextDisabled("No PCI in data — heatmap uses all points for the selected band");
    }

    if (!heatmapEarfcnChoices.empty()) {
        const std::string& ear = heatmapEarfcnChoices[heatmapEarfcnChoiceIdx];
        if (ImGui::BeginCombo("EARFCN / band", ShortText(ear, 48).c_str())) {
            for (int i = 0; i < static_cast<int>(heatmapEarfcnChoices.size()); ++i) {
                const bool sel = (i == heatmapEarfcnChoiceIdx);
                if (ImGui::Selectable(heatmapEarfcnChoices[i].c_str(), sel)) {
                    heatmapEarfcnChoiceIdx = i;
                    LoadHeatmapTiles();
                    if (heatmapTextures.empty()) GenerateHeatmapTiles();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    int critInt = static_cast<int>(heatmapCriterion);
    if (ImGui::Combo("Heatmap metric", &critInt, "RSRP\0RSRQ\0RSSI\0Altitude\0\0")) {
        heatmapCriterion = static_cast<HeatmapCriterion>(critInt);
        LoadHeatmapTiles();
        if (heatmapTextures.empty()) GenerateHeatmapTiles();
    }

    if (ImGui::SliderFloat("Interp. radius (m)", &heatmapInterpRadiusM, 10.0f, 40.0f, "%.0f")) {
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            LoadHeatmapTiles();
            if (heatmapTextures.empty()) GenerateHeatmapTiles();
        }
    }
    if (ImGui::Button("Load heatmap cache")) {
        LoadHeatmapTiles();
        if (heatmapTextures.empty()) GenerateHeatmapTiles();
    }

    if (!heatmapStatusMsg.empty()) {
        const bool ok = heatmapStatusMsg.rfind("Heatmap OK", 0) == 0;
        ImGui::TextColored(ok ? ImVec4(0.4f, 0.9f, 0.5f, 1.0f) : ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                           "%s", heatmapStatusMsg.c_str());
    }

    ImGui::Separator();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float sideWidth = 360.0f;
    bool sideBySide = avail.x > 900.0f;

    if (needFit && !points.empty()) {
        const ImVec2 fitSize = sideBySide ? ImVec2(std::max(100.0f, avail.x - sideWidth - 10.0f), std::max(100.0f, avail.y)) : avail;
        FitToData(fitSize);
    }

    if (sideBySide) {
        float mapWidth = std::max(300.0f, avail.x - sideWidth - 10.0f);
        ImGui::BeginChild("map_left_panel", ImVec2(mapWidth, avail.y), false, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextColored(ImVec4(0.9f, 0.95f, 1.0f, 1.0f), "Map viewport");
        ImGui::SameLine();
        ImGui::TextDisabled("wheel = zoom, drag = pan, click = pin");

        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        if (canvasSize.x < 10.0f) canvasSize.x = 10.0f;
        if (canvasSize.y < 10.0f) canvasSize.y = 10.0f;

        ImGui::InvisibleButton("map_canvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft);
        bool hovered = ImGui::IsItemHovered();
        bool active = ImGui::IsItemActive();
        ImVec2 canvasPos = ImGui::GetItemRectMin();

        if (hovered && io.MouseWheel != 0.0f) {
            const int oldZoom = zoom;
            const int newZoom = std::clamp(oldZoom + (io.MouseWheel > 0.0f ? 1 : -1), kMinZoom, kMaxZoom);
            if (newZoom != oldZoom) {
                const double oldScale = kTileSize * double(1 << oldZoom);
                const double newScale = kTileSize * double(1 << newZoom);
                const ImVec2 mouseLocal(io.MousePos.x - canvasPos.x, io.MousePos.y - canvasPos.y);

                const ImVec2 worldCenterOldPx(float(centerWorldX * oldScale), float(centerWorldY * oldScale));
                const ImVec2 oldOriginPx(worldCenterOldPx.x - canvasSize.x * 0.5f - pan_px.x, worldCenterOldPx.y - canvasSize.y * 0.5f - pan_px.y);
                const double worldXNorm = (oldOriginPx.x + mouseLocal.x) / oldScale;
                const double worldYNorm = (oldOriginPx.y + mouseLocal.y) / oldScale;

                zoom = newZoom;

                const ImVec2 newOriginPx(float(worldXNorm * newScale - mouseLocal.x), float(worldYNorm * newScale - mouseLocal.y));
                const ImVec2 worldCenterNewPx(float(centerWorldX * newScale), float(centerWorldY * newScale));

                pan_px.x = worldCenterNewPx.x - canvasSize.x * 0.5f - newOriginPx.x;
                pan_px.y = worldCenterNewPx.y - canvasSize.y * 0.5f - newOriginPx.y;
                needFit = false;
            }
        }

        if (hovered && active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            pan_px.x += io.MouseDelta.x;
            pan_px.y += io.MouseDelta.y;
        }

        DrawCanvas(canvasPos, canvasSize);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("map_right_panel", ImVec2(0, avail.y), true);
        ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.35f, 1.0f), "Telemetry details");
        ImGui::Separator();
        ImGui::Text("Points loaded: %d", (int)points.size());
        ImGui::Text("Zoom: %d", zoom);
        ImGui::Separator();

        int showIndex = hoveredIndex >= 0 ? hoveredIndex : selectedIndex;
        if (showIndex >= 0 && showIndex < (int)points.size()) {
            DrawPointDetails(points[showIndex]);
        } else {
            ImGui::TextDisabled("Hover a point or click it to pin details here");
        }

        ImGui::Separator();
        DrawLegend();
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("map_stack_panel", ImVec2(0, avail.y), true);
        ImGui::TextColored(ImVec4(0.9f, 0.95f, 1.0f, 1.0f), "Map viewport");
        ImGui::TextDisabled("wheel = zoom, drag = pan, click = pin");

        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        if (canvasSize.y > 260.0f) canvasSize.y -= 260.0f;

        ImGui::InvisibleButton("map_canvas_small", canvasSize, ImGuiButtonFlags_MouseButtonLeft);
        DrawCanvas(ImGui::GetItemRectMin(), canvasSize);

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.35f, 1.0f), "Telemetry details");
        int showIndex = hoveredIndex >= 0 ? hoveredIndex : selectedIndex;
        if (showIndex >= 0 && showIndex < (int)points.size()) {
            DrawPointDetails(points[showIndex]);
        } else {
            ImGui::TextDisabled("Hover a point or click it to pin details here.");
        }
        ImGui::Separator();
        DrawLegend();
        ImGui::EndChild();
    }
    ImGui::End();
}
