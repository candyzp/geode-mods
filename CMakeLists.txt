cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

project(DashBoost VERSION 1.0.0)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/RefreshRateIOS.mm")
endif()

add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK. Set GEODE_SDK to your Geode SDK path.")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    find_library(UIKIT_FRAMEWORK UIKit)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${UIKIT_FRAMEWORK})
endif()

setup_geode_mod(${PROJECT_NAME})
