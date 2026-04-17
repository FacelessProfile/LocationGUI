#pragma once
#include <string>
#include <map>
#include <set>
#include <vector>
#include <curl/curl.h>
#include <GL/glew.h>
#include <mutex>
#include <cmath>
#include <thread>

struct TileID {
    int x, y, z;
    bool operator<(const TileID& other) const {
        if (z != other.z) return z < other.z;
        if (x != other.x) return x < other.x;
        return y < other.y;
    }
};

struct PendingTexture {
    TileID id;
    unsigned char* data;
    int width, height;
};

class MapManager {
public:
    MapManager();
    ~MapManager();

    GLuint GetTileTexture(int x, int y, int z);
    static void LonLatToWebMercator(double lon, double lat, double& outX, double& outY);

private:
    std::map<TileID, GLuint> texture_cache;
    std::set<TileID> downloading_tiles;
    std::vector<PendingTexture> pending_textures;

    std::mutex cache_mutex;
    std::mutex pending_mutex;
};
