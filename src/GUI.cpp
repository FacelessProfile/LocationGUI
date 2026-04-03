#include <GL/glew.h>
#include <SDL2/SDL.h>

#include <vector>
#include <deque>
#include <string>
#include <atomic>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <iostream>
#include <filesystem>

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "imgui.h"
#include "implot.h"

#include "headers/data.h"
#include "headers/MapWindow.h"

std::vector<TelemetryData> FetchMapData();

static void LoadUIFonts(ImGuiIO& io) {
    static const ImWchar ranges[] = {
        0x0020, 0x00FF,
        0x0400, 0x04FF,
        0,
    };
    const char* fontPaths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    };

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;

    for (const char* path : fontPaths) {
        if (std::filesystem::exists(path)) {
            io.Fonts->AddFontFromFileTTF(path, 17.0f, &cfg, ranges);
            return;
        }
    }
    io.Fonts->AddFontDefault();
}

static void ApplyCustomStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    style.WindowRounding = 5.0f;
    style.FrameRounding = 3.0f;
    style.FrameBorderSize = 1.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 3.0f;

    colors[ImGuiCol_Text]                   = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.06f, 0.06f, 0.06f, 0.94f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.08f, 0.08f, 0.08f, 0.98f);
    colors[ImGuiCol_Border]                 = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    colors[ImGuiCol_Header]                 = ImVec4(0.20f, 0.25f, 0.29f, 0.55f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_Button]                 = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.28f, 0.56f, 0.90f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.20f, 0.50f, 0.85f, 1.00f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgActive]           = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_Tab]                    = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_PlotLines]              = ImVec4(0.00f, 0.75f, 1.00f, 1.00f);
    colors[ImGuiCol_PlotHistogram]          = ImVec4(0.20f, 0.80f, 0.20f, 1.00f);
    colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    colors[ImGuiCol_TableBorderLight]       = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
}

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

void RunGui(SharedBuffer& shared_data, std::atomic<bool>& running) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow(
        "Advanced Tracker Telemetry",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1500,
        950,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );

    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window);
        SDL_Quit();
        return;
    }

    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    if (glewInit() != GLEW_OK) {
        std::cerr << "glewInit failed\n";
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    LoadUIFonts(io);
    ApplyCustomStyle();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");

    std::vector<double> hist_lon, hist_lat, hist_sig;
    std::deque<TelemetryData> log_list;

    bool f_loc = true;
    bool f_lte = true;
    bool f_gsm = true;
    bool f_nr  = true;

    MapWindow mapWindow([]() {
        return FetchMapData();
    });

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                running = false;
            }
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                SDL_GetWindowFromID(event.window.windowID) == window) {
                running = false;
            }
        }

        auto incoming = shared_data.consumeNewData();
        for (const auto& d : incoming) {
            try {
                if (d.lat != "SKIP" && d.lat != "0" && d.lon != "0") {
                    hist_lat.push_back(std::stod(d.lat));
                    hist_lon.push_back(std::stod(d.lon));
                }
                hist_sig.push_back(std::stod(d.signal));
                log_list.push_front(d);
                if (log_list.size() > 100) {
                    log_list.pop_back();
                }
            } catch (...) {
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        //ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        ImGui::Begin("Station Control");
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Remote Sensors Config:");

        bool changed = false;
        changed |= ImGui::Checkbox("GPS/Altitude", &f_loc);
        changed |= ImGui::Checkbox("LTE", &f_lte);
        changed |= ImGui::Checkbox("GSM", &f_gsm);
        changed |= ImGui::Checkbox("NR (5G)", &f_nr);

        if (changed) {
            std::string fl;
            fl += (f_loc ? "1" : "0");
            fl += (f_lte ? "1" : "0");
            fl += (f_gsm ? "1" : "0");
            fl += (f_nr  ? "1" : "0");
            shared_data.setFlags(fl);
        }

        ImGui::TextDisabled("Map is in a separate window.");
        if (ImGui::Button("Reload map points")) {
            mapWindow.Reload();
        }
        ImGui::SameLine();
        ImGui::Text("Loaded: %d", (int)hist_sig.size());
        ImGui::End();

        ImGui::Begin("Live Telemetry");
        if (!hist_sig.empty()) {
            float cur_sig = (float)hist_sig.back();
            ImVec4 txt_col = (cur_sig > -90.0f)
                ? ImVec4(0, 1, 0, 1)
                : (cur_sig > -105.0f ? ImVec4(1, 1, 0, 1) : ImVec4(1, 0, 0, 1));

            ImGui::Text("Current RSRP: ");
            ImGui::SameLine();
            ImGui::TextColored(txt_col, "%.1f dBm", cur_sig);

            float norm = std::clamp((cur_sig + 120.0f) / 80.0f, 0.0f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, txt_col);
            ImGui::ProgressBar(norm, ImVec2(-1, 25), "");
            ImGui::PopStyleColor();
        }

        ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Reorderable |
            ImGuiTableFlags_Hideable;

        if (ImGui::BeginTable("MainLog", 6, tableFlags, ImVec2(0.0f, 0.0f))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("Coords");
            ImGui::TableSetupColumn("Alt/Acc");
            ImGui::TableSetupColumn("Net");
            ImGui::TableSetupColumn("Sig");
            ImGui::TableSetupColumn("Cell Data");
            ImGui::TableHeadersRow();

            for (const auto& log : log_list) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(log.timestamp.c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s, %s", log.lat.c_str(), log.lon.c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s / %s", log.alt.c_str(), log.accuracy.c_str());

                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(log.net_type.c_str());

                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(log.signal.c_str());

                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%s", log.cell_info.size() > 40 ? (log.cell_info.substr(0, 40) + "...").c_str() : log.cell_info.c_str());

                if (ImGui::IsItemHovered()) {
                    ShowCellDetails(log.net_type, log.cell_info);
                }
            }

            ImGui::EndTable();
        }
        ImGui::End();

        ImGui::Begin("Analytics");
        if (ImPlot::BeginPlot("Signal History", ImVec2(-1, -1))) {
            ImPlot::SetupAxes("Ticks", "dBm");
            ImPlot::SetupAxisLimits(ImAxis_Y1, -125, -50, ImPlotCond_Always);

            if (!hist_sig.empty()) {
                ImPlot::SetNextFillStyle(ImVec4(0.1f, 0.5f, 0.1f, 0.3f));
                ImPlot::PlotShaded("dBm", hist_sig.data(), (int)hist_sig.size(), -125.0);
                ImPlot::PlotLine("dBm", hist_sig.data(), (int)hist_sig.size());
            }

            ImPlot::EndPlot();
        }
        ImGui::End();
	ImGuiIO& io = ImGui::GetIO();
        mapWindow.Render();

        ImGui::Render();
	
        int display_w = 0;
        int display_h = 0;
        SDL_GL_GetDrawableSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            SDL_Window* backup_current_window = SDL_GL_GetCurrentWindow();
            SDL_GLContext backup_current_context = SDL_GL_GetCurrentContext();

            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();

            SDL_GL_MakeCurrent(backup_current_window, backup_current_context);
        }

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
