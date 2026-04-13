# =============================================================================
# tcxHwVideo - TrussC Hardware-Accelerated Video Player Addon
# =============================================================================
# Linux:   VAAPI (Intel/AMD) → V4L2M2M (RPi/ARM), auto-detected
# macOS:   VideoToolbox (planned)
# Windows: D3D11VA / NVDEC (planned)
# =============================================================================

cmake_minimum_required(VERSION 3.16)

set(ADDON_NAME tcxHwVideo)

# -----------------------------------------------------------------------------
# Sources
# -----------------------------------------------------------------------------

file(GLOB ADDON_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)

if(APPLE)
    file(GLOB ADDON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*_mac.mm)
elseif(WIN32)
    file(GLOB ADDON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*_win.cpp)
else()
    file(GLOB ADDON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*_linux.cpp)
endif()

# -----------------------------------------------------------------------------
# Build addon library
# -----------------------------------------------------------------------------

add_library(${ADDON_NAME} STATIC
    ${ADDON_SOURCES}
    ${ADDON_HEADERS}
)

target_include_directories(${ADDON_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# TrussC provides FFmpeg (libavcodec, libavformat, libswscale, libavutil,
# libswresample) as PUBLIC transitive dependencies.
target_link_libraries(${ADDON_NAME} PUBLIC TrussC)
