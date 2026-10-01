cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OSX_ARCHITECTURES "arm64")

project(DashBoost VERSION 1.0.0 LANGUAGES C CXX OBJCXX)

if (NOT "${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" AND NOT IOS)
    message(FATAL_ERROR "DashBoost is iOS-only.")
endif()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

list(APPEND SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/RefreshRateIOS.mm"
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK. Set GEODE_SDK to your Geode SDK path.")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

find_library(UIKIT_FRAMEWORK UIKit REQUIRED)
target_link_libraries(${PROJECT_NAME} ${UIKIT_FRAMEWORK})

setup_geode_mod(${PROJECT_NAME})
