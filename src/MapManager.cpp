#include "MapManager.h"
//#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <iostream>
#include <filesystem>
#include <fstream>
MapManager::MapManager() {
    curl_global_init(CURL_GLOBAL_ALL);
    std::error_code ec;
    std::filesystem::create_directory("tiles_cache", ec);
}

MapManager::~MapManager() {}

void MapManager::LonLatToWebMercator(double lon, double lat, double& outX, double& outY) {
    outX = (lon + 180.0) / 360.0;
    double latRad = lat * M_PI / 180.0;
    outY = (1.0 - std::log(std::tan(latRad) + 1.0 / std::cos(latRad)) / M_PI) / 2.0;
}

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::vector<unsigned char>*)userp)->insert(((std::vector<unsigned char>*)userp)->end(), (unsigned char*)contents, (unsigned char*)contents + size * nmemb);
    return size * nmemb;
}

GLuint MapManager::GetTileTexture(int x, int y, int z) {
    TileID id = {x, y, z};
    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        for (auto& pt : pending_textures) {
            GLuint texID;
            glGenTextures(1, &texID);
            glBindTexture(GL_TEXTURE_2D, texID);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pt.width, pt.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pt.data);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            stbi_image_free(pt.data);

            std::lock_guard<std::mutex> lock2(cache_mutex);
            texture_cache[pt.id] = texID;
            downloading_tiles.erase(pt.id);
        }
        pending_textures.clear();
    }

    std::lock_guard<std::mutex> lock(cache_mutex);
    if (texture_cache.count(id)) return texture_cache[id];
    if (!downloading_tiles.count(id)) {
        downloading_tiles.insert(id);

        std::thread([this, x, y, z, id]() {
            std::filesystem::path cache_path = std::filesystem::path("tiles_cache") 
                                               / std::to_string(z) 
                                               / std::to_string(x) 
                                               / (std::to_string(y) + ".png");
            
            int width, height, channels;
            unsigned char* img = nullptr;
            if (std::filesystem::exists(cache_path)) {
                img = stbi_load(cache_path.string().c_str(), &width, &height, &channels, 4);
            }
            if (!img) {
                std::string url = "https://tile.openstreetmap.org/" + std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y) + ".png";
                CURL* local_curl = curl_easy_init();
                std::vector<unsigned char> image_data;

                curl_easy_setopt(local_curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(local_curl, CURLOPT_WRITEFUNCTION, WriteCallback);
                curl_easy_setopt(local_curl, CURLOPT_WRITEDATA, &image_data);
                curl_easy_setopt(local_curl, CURLOPT_USERAGENT, "ImGui-Maps-App/1.0");

                CURLcode res = curl_easy_perform(local_curl);
                curl_easy_cleanup(local_curl);

                if (res == CURLE_OK && !image_data.empty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(cache_path.parent_path(), ec);
                    std::ofstream outfile(cache_path, std::ios::binary);
                    outfile.write(reinterpret_cast<const char*>(image_data.data()), image_data.size());
                    outfile.close();

                    img = stbi_load_from_memory(image_data.data(), image_data.size(), &width, &height, &channels, 4);
                }
            }

            if (img) {
                std::lock_guard<std::mutex> plock(pending_mutex);
                pending_textures.push_back({id, img, width, height});
                return;
            }

            std::lock_guard<std::mutex> clock(cache_mutex);
            downloading_tiles.erase(id);
        }).detach();
        }

    return 0;
}
