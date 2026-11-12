cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OSX_ARCHITECTURES "arm64")

project(CulpritFinder VERSION 0.1.0 LANGUAGES C CXX OBJCXX)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT IOS)
    message(FATAL_ERROR "Culprit Finder is iOS-only.")
endif()

add_library(CulpritFinder SHARED
    src/main.cpp
    src/RefreshRateIOS.mm
    src/PlatformDiagnosticsIOS.mm
)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK. Set GEODE_SDK to your Geode SDK path.")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

find_library(UIKIT_FRAMEWORK UIKit REQUIRED)
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
target_link_libraries(CulpritFinder ${UIKIT_FRAMEWORK} ${FOUNDATION_FRAMEWORK})

setup_geode_mod(CulpritFinder)
