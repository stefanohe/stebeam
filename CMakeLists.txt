cmake_minimum_required(VERSION 3.16)
project(SteBeam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)   # resources/resources.qrc embeds the logo/banner images so the exe is self-contained

# Qt6::Charts is deliberately NOT used: Qt6Charts is GPL-3.0-only, while the profile/size charts are drawn by
# the self-drawn LineChartView (chart_view.h), keeping the whole dependency closure within LGPL-3.0-optional modules.
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# app.rc references app.ico via an ICON statement, but CMake's RC dependency scanning only parses INCLUDE, not
# ICON files — without this dependency, replacing app.ico does not rebuild app.rc.obj and the exe icon stays stale.
set_source_files_properties(resources/app.rc PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico)

add_executable(SteBeam WIN32
    src/main.cpp
    src/camera_source.cpp
    src/beam_analyzer.cpp
    src/csv_recorder.cpp
    src/selftest.cpp
    src/capture.cpp
    src/simulated_source.cpp
    src/mvs_source.cpp
    src/shutter_controller.cpp
    src/spot_view.cpp
    src/view3d.cpp
    src/mainwindow.cpp
    resources/resources.qrc
    resources/app.rc
)

# Hikrobot MVS SDK header (compile time only needs MvCameraControl.h; at runtime the SDK is bound dynamically
# via LoadLibrary — no import library is linked and no vendor DLL is shipped). This repository does not carry
# that header (copyright Hikrobot); build it after installing the MVS SDK and passing its Includes directory via
# -DMVS_SDK_DIR (mandatory; the path is checked at configure time).
set(MVS_SDK_DIR "" CACHE PATH "Path to Hikrobot MVS SDK Includes directory (containing MvCameraControl.h)")
if(NOT MVS_SDK_DIR OR NOT EXISTS "${MVS_SDK_DIR}/MvCameraControl.h")
    message(FATAL_ERROR "MVS_SDK_DIR not set or MvCameraControl.h not found there. "
        "Install the Hikrobot MVS SDK and configure with -DMVS_SDK_DIR=<path-to-MVS>/Development/Includes")
endif()

target_include_directories(SteBeam PRIVATE
    src
    ${MVS_SDK_DIR}
)

target_link_libraries(SteBeam PRIVATE Qt6::Widgets)
