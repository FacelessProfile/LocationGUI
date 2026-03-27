cmake_minimum_required(VERSION 3.10)
project(TelemetryApp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(OpenGL REQUIRED)
find_package(SDL2 REQUIRED)
find_package(CURL REQUIRED)

pkg_check_modules(POSTGRES REQUIRED libpq)
pkg_check_modules(ZMQ REQUIRED libzmq)

find_package(GLEW REQUIRED)

set(APP_SOURCES
    src/main.cpp
    src/network.cpp
    src/GUI.cpp
    src/MapManager.cpp
    src/MapWindow.cpp
    src/AltitudeService.cpp
)

set(IMGUI_DIR third_party/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

set(IMPLOT_DIR third_party/implot)
set(IMPLOT_SOURCES
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
    ${IMPLOT_DIR}/implot_demo.cpp
)
add_executable(telemetry_app ${APP_SOURCES} ${IMGUI_SOURCES} ${IMPLOT_SOURCES})

target_include_directories(telemetry_app PRIVATE
    src
    src/headers
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMPLOT_DIR}
    ${SDL2_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
    ${POSTGRES_INCLUDE_DIRS}
    ${ZMQ_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(telemetry_app PRIVATE
    ${SDL2_LIBRARIES}
    ${OPENGL_LIBRARIES}
    ${GLEW_LIBRARIES}
    ${POSTGRES_LIBRARIES}
    ${ZMQ_LIBRARIES}
    ${CURL_LIBRARIES}
    pthread
    dl
)

target_compile_definitions(telemetry_app PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLEW)
