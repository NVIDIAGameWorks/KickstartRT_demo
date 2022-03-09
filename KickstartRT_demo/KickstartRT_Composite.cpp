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
#if __linux__
#define ExitProcess exit
#endif

#include "KickStartRT_Composite.h"

using namespace donut;
using namespace donut::math;
using namespace donut::app;
using namespace donut::vfs;
using namespace donut::engine;

KickStart_Composite::KickStart_Composite(
	nvrhi::IDevice* device,
	std::shared_ptr<donut::engine::ShaderFactory> shaderFactory,
	std::shared_ptr<donut::engine::CommonRenderPasses> commonPasses,
	std::shared_ptr<donut::engine::FramebufferFactory> lightingBuffer,
	nvrhi::TextureHandle gbuffer_AlbedoRT,
	nvrhi::TextureHandle gbuffer_RTReflectionRT,
	nvrhi::TextureHandle gbuffer_RTGIRT,
	nvrhi::TextureHandle gbuffer_RTAORT,
	nvrhi::TextureHandle gbuffer_RTShadows
)
{
	nvrhi::IFramebuffer* lightingBufferFB = lightingBuffer->GetFramebuffer(nvrhi::TextureSubresourceSet(0, 1, 0, 1));
	const auto& primaryFbInfo = lightingBufferFB->getFramebufferInfo();
	
	m_CommonPasses = commonPasses;

	{
		nvrhi::BufferDesc constantBufferDesc;
		constantBufferDesc.byteSize = sizeof(KickStart_CommonConstants);
		constantBufferDesc.debugName = "KickStart_CommonConstants";
		constantBufferDesc.isConstantBuffer = true;
		constantBufferDesc.isVolatile = true;
		constantBufferDesc.maxVersions = engine::c_MaxRenderPassConstantBufferVersions;
		m_CommonConstants = device->createBuffer(constantBufferDesc);
	}

	// CS
	{
		auto& pass = m_composite;

		pass.CS = shaderFactory->CreateShader("donut/app/KickStart_Composite_cs.hlsl", "main", nullptr, nvrhi::ShaderType::Compute);
		if (!pass.CS) {
			log::fatal("Faild to createShader.\n");
		}
		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::Compute;
		layoutDesc.bindings = {
			nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Texture_SRV(1),
			nvrhi::BindingLayoutItem::Texture_SRV(2),
			nvrhi::BindingLayoutItem::Texture_SRV(3),
			nvrhi::BindingLayoutItem::Texture_SRV(4),
			nvrhi::BindingLayoutItem::Texture_UAV(0),
		};
		pass.BindingLayout = device->createBindingLayout(layoutDesc);

		nvrhi::BindingSetDesc bsd;
		bsd.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, m_CommonConstants),
			nvrhi::BindingSetItem::Texture_SRV(0, gbuffer_AlbedoRT),
			nvrhi::BindingSetItem::Texture_SRV(1, gbuffer_RTReflectionRT),
			nvrhi::BindingSetItem::Texture_SRV(2, gbuffer_RTGIRT),
			nvrhi::BindingSetItem::Texture_SRV(3, gbuffer_RTAORT),
			nvrhi::BindingSetItem::Texture_SRV(4, gbuffer_RTShadows),
			nvrhi::BindingSetItem::Texture_UAV(0, lightingBuffer->RenderTargets[0]),
		};
		pass.BindingSet = device->createBindingSet(bsd, pass.BindingLayout);

		nvrhi::ComputePipelineDesc pipelineDesc;
		pipelineDesc.CS = pass.CS;
		pipelineDesc.bindingLayouts = { pass.BindingLayout };

		pass.Pipeline = device->createComputePipeline(pipelineDesc);
	}
}

void KickStart_Composite::Render(
	nvrhi::IDevice* device,
	nvrhi::ICommandList* commandList,
	std::shared_ptr<engine::FramebufferFactory> lightingBuffer,
	nvrhi::TextureHandle gbuffer_AlbedoRT,
	nvrhi::TextureHandle gbuffer_RTReflectionRT,
	nvrhi::TextureHandle gbuffer_RTGIRT,
	nvrhi::TextureHandle gbuffer_RTAORT,
	nvrhi::TextureHandle gbuffer_RTShadows,
	bool enableDebug
)
{
	// update common constants.
	{
		KickStart_CommonConstants	cb = {};

		cb.enableRTReflections = gbuffer_RTReflectionRT.Get() != nullptr ? 1 : 0;
		cb.enableRTGI = gbuffer_RTGIRT.Get() != nullptr ? 1 : 0;
		cb.enableRTAO = gbuffer_RTAORT.Get() != nullptr ? 1 : 0;
		cb.enableRTShadows = gbuffer_RTShadows.Get() != nullptr ? 1 : 0;
		cb.enableDebug = enableDebug ? 1 : 0;

		// nothing to composite.
		if (cb.enableRTReflections + cb.enableRTAO + cb.enableRTGI + cb.enableRTShadows == 0)
			return;

		commandList->writeBuffer(m_CommonConstants, &cb, sizeof(cb));
	}

	// get primary surface's resolution
	nvrhi::IFramebuffer* lightingBufferFB = lightingBuffer->GetFramebuffer(nvrhi::TextureSubresourceSet(0, 1, 0, 1));
	const auto& primaryFbInfo = lightingBufferFB->getFramebufferInfo();

	// Copy Depth and calc variance.
	{
		commandList->beginMarker("KickStart_composite");

		auto& pass = m_composite;
		uint32_t dispatchWidth = (primaryFbInfo.width + 7) / 8;
		uint32_t dispatchHeight = (primaryFbInfo.height + 7) / 8;

		nvrhi::ComputeState state;
		state.pipeline = pass.Pipeline;
		state.bindings = { pass.BindingSet };
		commandList->setComputeState(state);
		commandList->dispatch(dispatchWidth, dispatchHeight, 1);

		commandList->endMarker();
	}
};

