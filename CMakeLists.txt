#
# Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.


cmake_minimum_required(VERSION 3.10)
project(KickstartRT_Demo VERSION 0.9.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set_property( GLOBAL PROPERTY USE_FOLDERS ON)

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3 /MP")
endif()

string(APPEND CMAKE_CXX_FLAGS_DEBUG " -D_DEBUG")

include(CMakeDependentOption)

option(DONUT_WITH_NVRHI "Enable NVRHI and related projects" ON)

cmake_dependent_option(DONUT_WITH_DX11 "Enable the DX11 version of Donut" ON "WIN32" OFF)
cmake_dependent_option(DONUT_WITH_DX12 "Enable the DX12 version of Donut" ON "WIN32" OFF)
option(DONUT_WITH_VULKAN "Enable the Vulkan version of Donut" ON)

option(DONUT_WITH_AUDIO "Include Audio features (XAudio2)" OFF)
option(DONUT_WITH_LZ4 "Include LZ4" ON)
option(DONUT_WITH_MINIZ "Include miniz (support for zip archives)" ON)
option(DONUT_WITH_TASKFLOW "Include TaskFlow" ON)
option(DONUT_WITH_TINYEXR "Include TinyEXR" ON)
option(DONUT_WITH_UNIT_TESTS "Donut unit-tests (see CMake/CTest documentation)" OFF)

if (WIN32)
    option(KickstartRT_SDK_WITH_DX12 "Enable the DX12 version of SDK" ON)
    option(KickstartRT_SDK_WITH_Interop_DX11 "Enable the DX11 interop layer of SDK" ON)
    option(KickstartRT_SDK_WITH_NRD "Include NRD as part of the SDK. Supported for all backends." ON)
else()
    option(KickstartRT_SDK_WITH_DX12 "Enable the DX12 version of SDK" OFF)
    option(KickstartRT_SDK_WITH_Interop_DX11 "Enable the DX11 interop layer of SDK" OFF)
    option(KickstartRT_SDK_WITH_NRD "Include NRD as part of the SDK. Supported for all backends." ON)
endif()
option(KickstartRT_SDK_WITH_VULKAN "Enable the Vulkan version of SDK" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(DONUT_SHADERS_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/framework")

add_subdirectory(thirdparty)

if (DONUT_WITH_NVRHI)
    set(NVRHI_WITH_DX11 "${DONUT_WITH_DX11}" CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DX12 "${DONUT_WITH_DX12}" CACHE BOOL "" FORCE)
    set(NVRHI_WITH_VULKAN "${DONUT_WITH_VULKAN}" CACHE BOOL "" FORCE)
    add_subdirectory(nvrhi)

    set(DONUT_SHADER_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/include CACHE PATH "" FORCE)

    if (DONUT_WITH_DX11)
        find_package(FXC REQUIRED)
    endif()

    if(DONUT_WITH_DX12)
        find_package(DXCdxil REQUIRED)
        set(DONUT_USE_DXIL_ON_DX12 TRUE CACHE BOOL "Use DXC to compile DXIL shaders on DX12 - otherwise FXC and DXBC")
    endif()

    if (DONUT_WITH_VULKAN)
        find_package(DXCspirv REQUIRED)
    endif()

    add_subdirectory(shaders)
endif()

include(donut-core.cmake)
if (DONUT_WITH_NVRHI)
    include(donut-engine.cmake)
    include(donut-render.cmake)
    include(donut-app.cmake)
endif()

if (DONUT_WITH_UNIT_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()

add_subdirectory(KickstartRT)
add_subdirectory(KickstartRT_demo)
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT KickstartRT_Demo)