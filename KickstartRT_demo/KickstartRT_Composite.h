//----------------------------------------------------------------------------------
// Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//----------------------------------------------------------------------------------

#include <string>
#include <vector>
#include <memory>
#include <array>

#if __linux__
#define ExitProcess exit
#endif

#include <nvrhi/nvrhi.h>
#include <nvrhi/utils.h>

#include <donut/core/vfs/VFS.h>
#include <donut/core/log.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ConsoleInterpreter.h>
#include <donut/engine/ConsoleObjects.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/MediaFileSystem.h>
#include <donut/app/Camera.h>
#include <donut/app/DeviceManager.h>


#if defined(__cplusplus)
#pragma pack(push, 4)
#endif

#if defined(__cplusplus)
#define CPP_NS donut::math::
#else
#define CPP_NS 
#endif

struct KickStart_CommonConstants
{
    CPP_NS uint     enableRTReflections;
    CPP_NS uint     enableRTGI;
    CPP_NS uint     enableRTAO;
    CPP_NS uint     _pad0;

    CPP_NS uint     enableDebug;
    CPP_NS uint     enableYCoCgToLienarOnRTReflections;
    CPP_NS uint     enableYCoCgToLienarOnRTGI;
    CPP_NS uint     _pad1;

};

#undef CPP_NS

#if defined(__cplusplus)
#pragma pack(pop)
#endif


class KickStart_Composite
{
public:
    nvrhi::TextureHandle m_RenderTarget;

private:
    std::shared_ptr<donut::engine::CommonRenderPasses> m_CommonPasses;

    nvrhi::BufferHandle m_CommonConstants;

    struct CSSubPass
    {
        nvrhi::ShaderHandle CS;
        nvrhi::BindingLayoutHandle BindingLayout;
        nvrhi::BindingSetHandle BindingSet;
        nvrhi::ComputePipelineHandle Pipeline;

    };

    CSSubPass m_composite;

public:
    KickStart_Composite(
        nvrhi::IDevice* device,
        std::shared_ptr<donut::engine::ShaderFactory> shaderFactory,
        std::shared_ptr<donut::engine::CommonRenderPasses> commonPasses,
        std::shared_ptr<donut::engine::FramebufferFactory> lightingBuffer,
        nvrhi::TextureHandle gbuffer_AlbedoRT,
        nvrhi::TextureHandle gbuffer_RTReflectionRT,
        nvrhi::TextureHandle gbuffer_RTGIRT,
        nvrhi::TextureHandle gbuffer_RTAORT,
        nvrhi::TextureHandle gbuffer_RTShadows
    );
    
    void Render(
        nvrhi::IDevice* device,
        nvrhi::ICommandList* commandList,
        std::shared_ptr<donut::engine::FramebufferFactory> lightingBuffer,
        nvrhi::TextureHandle gbuffer_AlbedoRT,
        nvrhi::TextureHandle gbuffer_RTReflectionRT,
        nvrhi::TextureHandle gbuffer_RTGIRT,
        nvrhi::TextureHandle gbuffer_RTAORT,
        nvrhi::TextureHandle gbuffer_RTShadows,
        bool enableDebug,
        bool enableYCoCgToLinear
    );
};

