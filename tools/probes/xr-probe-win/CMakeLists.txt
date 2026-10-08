cmake_minimum_required(VERSION 3.20)
project(xr_probe_win CXX C)

set(CMAKE_CXX_STANDARD 20)

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../shadps4-arm64-main/externals/openxr-sdk
                 openxr-sdk EXCLUDE_FROM_ALL)

add_executable(xr_probe_win main.cpp)
target_include_directories(xr_probe_win PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../shadps4-arm64-main/externals/vulkan-headers/include)
target_link_libraries(xr_probe_win PRIVATE OpenXR::openxr_loader)
