#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <vector>
#include <deque>
#include <string>
#include <atomic>
#include <algorithm>
#include <sstream>

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "imgui.h"
#include "implot.h"
#include "headers/data.h"

void ShowCellDetails(const std::string& type, const std::string& raw_data) {
    auto DisplayVal = [](const char* label, std::string val, const char* unit) {
        if (val == "2147483647" || val == "0" || val == "-1" || val.empty()) 
            ImGui::BulletText("%s: N/A", label);
        else 
            ImGui::BulletText("%s: %s %s", label, val.c_str(), unit);
    };

    std::stringstream ss(raw_data);
    std::string item;
    std::vector<std::string> v;
    while (std::getline(ss, item, ';')) { v.push_back(item); }

    ImGui::BeginTooltip();
    if (v.empty()) { ImGui::Text("Нет данных"); ImGui::EndTooltip(); return; }
    if (type == "LTE" && v.size() >= 13) {
        // Kotlin: LTE; bands; ci; earfcn; mcc; mnc; pci; tac; asu; cqi; rsrp; rsrq; rssi; rssnr; ta
        ImGui::TextColored(ImVec4(0, 1, 1, 1), "LTE Cell ID: %s", v[2].c_str());
        ImGui::Separator();
        ImGui::BulletText("Operator: %s-%s", v[4].c_str(), v[5].c_str());
        ImGui::BulletText("PCI: %s | TAC: %s", v[6].c_str(), v[7].c_str());
        ImGui::BulletText("Band: %s (EARFCN: %s)", v[1].c_str(), v[3].c_str());
        ImGui::Separator();
        DisplayVal("RSRP", v[10], "dBm");
        DisplayVal("RSRQ", v[11], "dB");
        DisplayVal("RSSI", v[12], "dBm");
    } 
    else if (type == "GSM" && v.size() >= 9) {
        // Kotlin: GSM; cid; bsic; arfcn; lac; mcc; mnc; psc; dbm; rssi; ta
        ImGui::TextColored(ImVec4(0, 1, 1, 1), "GSM Cell ID: %s", v[1].c_str());
        ImGui::Separator();
        ImGui::BulletText("Operator: %s-%s", v[5].c_str(), v[6].c_str());
        ImGui::BulletText("LAC: %s | BSIC: %s", v[4].c_str(), v[2].c_str());
        ImGui::BulletText("ARFCN: %s", v[3].c_str());
        ImGui::Separator();
        DisplayVal("Signal", v[8], "dBm");
    }
    else if (type == "NR" && v.size() >= 11) {
        // Kotlin: NR; bands; nci; pci; nrarfcn; tac; mcc; mnc; ssRsrp; ssRsrq; ssSinr; 0
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "NR (5G) Cell ID: %s", v[2].c_str());
        ImGui::Separator();
        ImGui::BulletText("Operator: %s-%s", v[6].c_str(), v[7].c_str());
        ImGui::BulletText("PCI: %s | TAC: %s", v[3].c_str(), v[5].c_str());
        ImGui::BulletText("Band: %s (NRARFCN: %s)", v[1].c_str(), v[4].c_str());
        ImGui::Separator();
        DisplayVal("ssRSRP", v[8], "dBm");
        DisplayVal("ssRSRQ", v[9], "dB");
        DisplayVal("ssSINR", v[10], "dB");
    }
    else {
        ImGui::Text("Raw: %s", raw_data.c_str());
    }
    ImGui::EndTooltip();
}
void ApplyCustomStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 4.0f;
    style.FramePadding = ImVec2(5, 5);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.1f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.2f, 0.2f, 0.25f, 1.0f);
}

void RunGui(SharedBuffer& shared_data, std::atomic<bool>& running) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return;

    SDL_Window* window = SDL_CreateWindow("Advanced Tracker Telemetry", 
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1500, 950, 
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_SetSwapInterval(1);

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
    bool f_loc = true, f_lte = true, f_gsm = true, f_nr = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
        }

        auto incoming = shared_data.consumeNewData();
        for (const auto& d : incoming) {
            try {
                if (d.lat != "SKIP" && d.lat != "0") {
                    hist_lat.push_back(std::stod(d.lat));
                    hist_lon.push_back(std::stod(d.lon));
                }
                hist_sig.push_back(std::stod(d.signal));
                log_list.push_front(d);
                if (log_list.size() > 100) log_list.pop_back();
            } catch (...) { }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        // --- ОКНО ФИЛЬТРОВ ---
        ImGui::Begin("Station Control");
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Remote Sensors Config:");
        if (ImGui::Checkbox("GPS/Altitude", &f_loc) || ImGui::Checkbox("LTE", &f_lte) || 
            ImGui::Checkbox("GSM", &f_gsm) || ImGui::Checkbox("NR (5G)", &f_nr)) {
            std::string flags = "";
            flags += f_loc ? "1" : "0";
            flags += f_lte ? "1" : "0";
            flags += f_gsm ? "1" : "0";
            flags += f_nr  ? "1" : "0";
            shared_data.setFlags(flags);
        }
        ImGui::End();

        ImGui::Begin("Live Telemetry");
        if (!hist_sig.empty()) {
            float cur_sig = (float)hist_sig.back();
            // Цвет текста в зависимости от качества
            ImVec4 txt_col = (cur_sig > -90) ? ImVec4(0,1,0,1) : (cur_sig > -105 ? ImVec4(1,1,0,1) : ImVec4(1,0,0,1));
            ImGui::Text("Current RSRP: "); ImGui::SameLine();
            ImGui::TextColored(txt_col, "%.1f dBm", cur_sig);

            float norm = std::clamp((cur_sig + 120.0f) / 80.0f, 0.0f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, txt_col);
            ImGui::ProgressBar(norm, ImVec2(-1, 25), "");
            ImGui::PopStyleColor();
        }

        if (ImGui::BeginTable("MainLog", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit)) {
    
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Coordinates(lat/lon)", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableSetupColumn("Alt/Acc(meters)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("NetType", ImGuiTableColumnFlags_WidthFixed, 50.0f);
    ImGui::TableSetupColumn("Sig(RSRP)", ImGuiTableColumnFlags_WidthFixed, 50.0f);
    ImGui::TableSetupColumn("Cell Data", ImGuiTableColumnFlags_WidthStretch);
    
    ImGui::TableHeadersRow();

            for (const auto& log : log_list) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%s", log.timestamp.c_str());
                ImGui::TableSetColumnIndex(1); ImGui::Text("%s, %s", log.lat.c_str(), log.lon.c_str());
                ImGui::TableSetColumnIndex(2); ImGui::Text("%s / %s", log.alt.c_str(), log.accuracy.c_str());
                ImGui::TableSetColumnIndex(3);
                if (log.net_type == "LTE") ImGui::TextColored(ImVec4(0.4, 0.7, 1, 1), "LTE");
                else if (log.net_type == "NR") ImGui::TextColored(ImVec4(0.4, 1, 0.4, 1), "5G");
                else ImGui::Text("%s", log.net_type.c_str());

                ImGui::TableSetColumnIndex(4); ImGui::Text("%s", log.signal.c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%.25s...", log.cell_info.c_str());
                if (ImGui::IsItemHovered()) {
                    ShowCellDetails(log.net_type, log.cell_info);
                }
            }
            ImGui::EndTable();
        }
        ImGui::End();
        ImGui::Begin("Map & Signal Analytics");
        if (ImPlot::BeginPlot("Trajectory", ImVec2(-1, ImGui::GetContentRegionAvail().y * 0.5f))) {
            ImPlot::SetupAxes("Lon", "Lat");
            if (!hist_lon.empty()) {
                if (need_recenter) {
                    ImPlot::SetupAxesLimits(hist_lon.back()-0.01, hist_lon.back()+0.01, hist_lat.back()-0.01, hist_lat.back()+0.01, ImPlotCond_Always);
                    need_recenter = false;
                }
                ImPlot::PlotLine("Path", hist_lon.data(), hist_lat.data(), (int)hist_lon.size());
                ImPlot::PlotScatter("Current", &hist_lon.back(), &hist_lat.back(), 1);
            }
            ImPlot::EndPlot();
        }

        if (ImPlot::BeginPlot("Signal History", ImVec2(-1, -1))) {
            ImPlot::SetupAxes("Ticks", "dBm");
            ImPlot::SetupAxisLimits(ImAxis_Y1, -125, -50, ImPlotCond_Always);
            if (!hist_sig.empty()) {
                ImPlot::SetNextFillStyle(ImVec4(0.1f, 0.5f, 0.1f, 0.3f));
                ImPlot::PlotShaded("dBm", hist_sig.data(), (int)hist_sig.size(), -125);
                ImPlot::PlotLine("dBm", hist_sig.data(), (int)hist_sig.size());
            }
            ImPlot::EndPlot();
        }
        ImGui::End();
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
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
