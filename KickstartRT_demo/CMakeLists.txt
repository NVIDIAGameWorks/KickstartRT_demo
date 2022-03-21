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

file(GLOB KickstartRT_Demo_source
    *.h
    *.cpp
)

add_executable(KickstartRT_Demo WIN32 ${KickstartRT_Demo_source})

set (KickstartRT_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../KickstartRT)
set (KickstartRT_SDK_NAME KickstartRT)

if (KickstartRT_SDK_WITH_DX12)
    list(APPEND KickstartRT_Libs ${KickstartRT_SDK_NAME}_D3D12)
    list(APPEND KickstartRT_Deps KickstartRT_core_DX12)
    target_compile_definitions(KickstartRT_Demo PRIVATE KickstartRT_Demo_WITH_D3D12=1)
endif()
if (KickstartRT_SDK_WITH_Interop_DX11)
    list(APPEND KickstartRT_Libs ${KickstartRT_SDK_NAME}_Interop_D3D11)
    list(APPEND KickstartRT_Deps KickstartRT_Interop_D3D11)
    target_compile_definitions(KickstartRT_Demo PRIVATE KickstartRT_Demo_WITH_D3D11=1)
endif()
if (KickstartRT_SDK_WITH_VULKAN)
    list(APPEND KickstartRT_Libs ${KickstartRT_SDK_NAME}_VK)
    list(APPEND KickstartRT_Deps KickstartRT_core_VULKAN)
    target_compile_definitions(KickstartRT_Demo PRIVATE KickstartRT_Demo_WITH_VK=1)
endif()
if (KickstartRT_SDK_WITH_NRD)
    target_compile_definitions(KickstartRT_Demo PRIVATE KickstartRT_Demo_WITH_NRD=1)
endif()

if (WIN32)
    if (NOT KickstartRT_SDK_WITH_DX12)
        if (KickstartRT_SDK_WITH_VULKAN)
            set_target_properties(KickstartRT_Demo PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS "-vk")
        endif()
    endif()
endif()

set(KickstartRT_Demo_Libs donut_render donut_app donut_engine)
list(APPEND KickstartRT_Demo_Libs ${KickstartRT_Libs})
if (DONUT_WITH_VULKAN)
    list(APPEND KickstartRT_Demo_Libs ${VULKAN_SDK_LIB})
endif()

target_link_libraries(KickstartRT_Demo PRIVATE ${KickstartRT_Demo_Libs})

target_include_directories(KickstartRT_Demo PRIVATE
    ${KickstartRT_SDK_ROOT}/include
    ${KickstartRT_SDK_ROOT}/common/src
)

set(KickstartRT_Demo_LinkDir ${CMAKE_CURRENT_BINARY_DIR}/../KickstartRT)
if (KickstartRT_SDK_WITH_Interop_DX11)
    list(APPEND KickstartRT_Demo_LinkDir ${CMAKE_CURRENT_BINARY_DIR}/../KickstartRT/interop_d3d11)
endif()

target_link_directories(KickstartRT_Demo PRIVATE ${KickstartRT_Demo_LinkDir})

add_dependencies(KickstartRT_Demo ${KickstartRT_Deps})

# Copy some files into the bin directory
if (NOT KickstartRT_PIX_EVENT_RUNTIME_SDK_PATH STREQUAL "")
    add_custom_command(TARGET KickstartRT_Demo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KickstartRT_PIX_EVENT_RUNTIME_SDK_PATH}/bin/x64/WinPixEventRuntime.dll $<TARGET_FILE_DIR:KickstartRT_Demo>
    )
endif()

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3 /MP")
endif()

