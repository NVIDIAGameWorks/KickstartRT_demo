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

set (KickstartRT_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../kickstartrt)
set (KickstartRT_SDK_NAME KickstartRT)

if (WIN32)
set (KickstartRT_Libs 
    ${KickstartRT_SDK_NAME}_VK
    ${KickstartRT_SDK_NAME}_D3D12
    ${KickstartRT_SDK_NAME}_Interop_D3D11
)
else()
set (KickstartRT_Libs 
    ${KickstartRT_SDK_NAME}_VK
)
endif()
target_link_libraries(KickstartRT_Demo PRIVATE 
    donut_render
    donut_app
    donut_engine
    ${VULKAN_SDK_LIB}
    ${KickstartRT_Libs}
)

target_include_directories(KickstartRT_Demo PRIVATE
    ${KickstartRT_SDK_ROOT}/include
    ${KickstartRT_SDK_ROOT}/common/src
)

target_link_directories(KickstartRT_Demo PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/../kickstartrt
    ${CMAKE_CURRENT_BINARY_DIR}/../kickstartrt/interop_d3d11
)

add_dependencies(KickstartRT_Demo KickstartRT_core_DX12 KickstartRT_core_VULKAN KickstartRT_Interop_D3D11)

# Copy some files into the bin directory
if (NOT KickstartRT_PIX_EVENT_RUNTIME_SDK_PATH STREQUAL "")
    add_custom_command(TARGET KickstartRT_Demo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KickstartRT_PIX_EVENT_RUNTIME_SDK_PATH}/bin/x64/WinPixEventRuntime.dll $<TARGET_FILE_DIR:KickstartRT_Demo>
    )
endif()

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3 /MP")
endif()

