#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <vector>
#include <deque>
#include <string>
#include <atomic>
#include <algorithm>

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "imgui.h"
#include "implot.h"
#include "headers/data.h"

void ApplyCustomStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.1f, 0.1f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.29f, 0.48f, 1.0f);
}

void RunGui(SharedBuffer& shared_data, std::atomic<bool>& running) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return;

    SDL_Window* window = SDL_CreateWindow("Telemetry app", 
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1400, 900, 
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_SetSwapInterval(1); // VSync

    glewInit();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ApplyCustomStyle();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");
    std::vector<double> hist_lon, hist_lat, hist_sig;
    std::deque<TelemetryData> log_list;
    bool need_recenter = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
        }
        auto incoming = shared_data.consumeNewData();
        for (const auto& d : incoming) {
            try {
                double lat = std::stod(d.lat);
                double lon = std::stod(d.lon);
                double sig = std::stod(d.signal);

                hist_lat.push_back(lat);
                hist_lon.push_back(lon);
                hist_sig.push_back(sig);

                log_list.push_front(d);
                if (log_list.size() > 50) log_list.pop_back();
            } catch (...) { }
        }

        if (hist_lat.size() > 5000) {
            hist_lat.erase(hist_lat.begin());
            hist_lon.erase(hist_lon.begin());
            hist_sig.erase(hist_sig.begin());
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        ImGui::Begin("Telemetry Stream");
        if (!hist_sig.empty()) {
            float cur_sig = (float)hist_sig.back();
            ImGui::Text("Current Signal: %.1f dBm", cur_sig);
            float norm = std::clamp((cur_sig + 100.0f) / 70.0f, 0.0f, 1.0f);
            ImVec4 sig_color = ImVec4(1.0f - norm, norm, 0.2f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, sig_color);
            ImGui::ProgressBar(norm, ImVec2(-1, 20), "");
            ImGui::PopStyleColor();
        }

        ImGui::Separator();
        if (ImGui::BeginTable("Logs", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Lat");
            ImGui::TableSetupColumn("Lon");
            ImGui::TableSetupColumn("Sig");
            ImGui::TableHeadersRow();

            for (const auto& log : log_list) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%s", log.timestamp.c_str());
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.5f", std::atof(log.lat.c_str()));
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.5f", std::atof(log.lon.c_str()));
                ImGui::TableSetColumnIndex(3); ImGui::Text("%s", log.signal.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::End();
        ImGui::Begin("Visual Analytics");
        if (ImPlot::BeginPlot("Movement History", ImVec2(-1, ImGui::GetContentRegionAvail().y * 0.65f))) {
            ImPlot::SetupAxes("Longitude", "Latitude");
            
            if (!hist_lon.empty()) {
                if (need_recenter) {
                    ImPlot::SetupAxesLimits(hist_lon.back()-0.005, hist_lon.back()+0.005, 
                                            hist_lat.back()-0.005, hist_lat.back()+0.005, ImPlotCond_Always);
                    need_recenter = false;
                }
                ImPlot::SetNextLineStyle(ImVec4(0.1f, 0.8f, 1.0f, 1.0f), 2.5f);
                ImPlot::PlotLine("Path", hist_lon.data(), hist_lat.data(), (int)hist_lon.size());
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 6, ImVec4(1, 0, 0, 1), 1, ImVec4(1, 0, 0, 1));
                ImPlot::PlotScatter("Current Location(apprx.)", &hist_lon.back(), &hist_lat.back(), 1);
            }
            ImPlot::EndPlot();
        }

        if (ImPlot::BeginPlot("Signal Strength (dBm)", ImVec2(-1, -1))) {
            ImPlot::SetupAxes("Time Index", "dBm");
            ImPlot::SetupAxisLimits(ImAxis_Y1, -110, -20, ImPlotCond_Always);
            
            if (!hist_sig.empty()) {
                ImPlot::SetNextFillStyle(ImVec4(0.2f, 0.8f, 0.2f, 0.3f));
                ImPlot::PlotShaded("Signal", hist_sig.data(), (int)hist_sig.size(), -110);
                ImPlot::PlotLine("Signal", hist_sig.data(), (int)hist_sig.size());
            }
            ImPlot::EndPlot();
        }
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
