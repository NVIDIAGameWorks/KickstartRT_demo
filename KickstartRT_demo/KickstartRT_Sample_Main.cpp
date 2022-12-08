/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#include <string>
#include <vector>
#include <memory>
#include <chrono>

#include <donut/core/vfs/VFS.h>
#include <donut/core/log.h>
#include <donut/core/string_utils.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ConsoleInterpreter.h>
#include <donut/engine/ConsoleObjects.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/Scene.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/render/BloomPass.h>
#include <donut/render/CascadedShadowMap.h>
#include <donut/render/DeferredLightingPass.h>
#include <donut/render/DepthPass.h>
#include <donut/render/DrawStrategy.h>
#include <donut/render/ForwardShadingPass.h>
#include <donut/render/GBuffer.h>
#include <donut/render/GBufferFillPass.h>
#include <donut/render/LightProbeProcessingPass.h>
#include <donut/render/PixelReadbackPass.h>
#include <donut/render/SkyPass.h>
#include <donut/render/SsaoPass.h>
#include <donut/render/TemporalAntiAliasingPass.h>
#include <donut/render/ToneMappingPasses.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/UserInterfaceUtils.h>
#include <donut/app/Camera.h>
#include <donut/app/DeviceManager.h>
#include <donut/app/imgui_console.h>
#include <donut/app/imgui_renderer.h>
#include <nvrhi/utils.h>
#include <nvrhi/common/misc.h>
#ifdef WIN32
#include <nvrhi/d3d12.h>
#include <nvrhi/d3d11.h>
#endif
#include "KickstartRT_Composite.h"

#ifdef DONUT_WITH_TASKFLOW
#include <taskflow/taskflow.hpp>
#endif

#define ENABLE_KickStartSDK

#if defined(ENABLE_KickStartSDK)
#if KickstartRT_Demo_WITH_D3D11
#define KickstartRT_Graphics_API_D3D11
#endif
#if KickstartRT_Demo_WITH_D3D12
#define KickstartRT_Graphics_API_D3D12
#endif
#if KickstartRT_Demo_WITH_VK
#define KickstartRT_Graphics_API_Vulkan
#endif
#include "KickstartRT.h"
namespace SDK = KickstartRT;

#else
#undef KickstartRT_Demo_WITH_D3D11
#undef KickstartRT_Demo_WITH_D3D11
#undef KickstartRT_Demo_WITH_VK
#endif

using namespace donut;
using namespace donut::math;
using namespace donut::app;
using namespace donut::vfs;
using namespace donut::engine;
using namespace donut::render;

static bool g_PrintSceneGraph = false;

class RenderTargets : public GBufferRenderTargets
{
public:
    nvrhi::TextureHandle HdrColor;
    nvrhi::TextureHandle LdrColor;
    nvrhi::TextureHandle MaterialIDs;
    nvrhi::TextureHandle ResolvedColor;
    nvrhi::TextureHandle TemporalFeedback1;
    nvrhi::TextureHandle TemporalFeedback2;
    nvrhi::TextureHandle AmbientOcclusion;

    nvrhi::HeapHandle Heap;

    std::shared_ptr<FramebufferFactory> ForwardFramebuffer;
    std::shared_ptr<FramebufferFactory> HdrFramebuffer;
    std::shared_ptr<FramebufferFactory> LdrFramebuffer;
    std::shared_ptr<FramebufferFactory> ResolvedFramebuffer;
    std::shared_ptr<FramebufferFactory> MaterialIDFramebuffer;
    
    void Init(
        nvrhi::IDevice* device,
        dm::uint2 size,
        dm::uint sampleCount,
        bool enableMotionVectors,
        bool useReverseProjection,
        bool sharedAcrossDevice) override
    {
        GBufferRenderTargets::Init(device, size, sampleCount, enableMotionVectors, useReverseProjection, sharedAcrossDevice);
        
        nvrhi::TextureDesc desc;
        desc.width = size.x;
        desc.height = size.y;
        desc.isRenderTarget = true;
        desc.useClearValue = true;
        desc.clearValue = nvrhi::Color(1.f);
        desc.sampleCount = sampleCount;
        desc.dimension = sampleCount > 1 ? nvrhi::TextureDimension::Texture2DMS : nvrhi::TextureDimension::Texture2D;
        desc.keepInitialState = true;
        desc.isVirtual = device->queryFeatureSupport(nvrhi::Feature::VirtualResources);

        desc.clearValue = nvrhi::Color(0.f);
        desc.isTypeless = false;
        desc.isUAV = sampleCount == 1;
        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.initialState = nvrhi::ResourceStates::RenderTarget;
        if (sharedAcrossDevice)
            desc.sharedResourceFlags = nvrhi::SharedResourceFlags::Shared;
        desc.debugName = "HdrColor";
        HdrColor = device->createTexture(desc);
        desc.sharedResourceFlags = nvrhi::SharedResourceFlags::None;

        desc.format = nvrhi::Format::RG16_UINT;
        desc.isUAV = false;
        desc.debugName = "MaterialIDs";
        MaterialIDs = device->createTexture(desc);

        // The render targets below this point are non-MSAA
        desc.sampleCount = 1;
        desc.dimension = nvrhi::TextureDimension::Texture2D;

        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.isUAV = true;
        desc.debugName = "ResolvedColor";
        ResolvedColor = device->createTexture(desc);

        desc.format = nvrhi::Format::RGBA16_SNORM;
        desc.debugName = "TemporalFeedback1";
        TemporalFeedback1 = device->createTexture(desc);
        desc.debugName = "TemporalFeedback2";
        TemporalFeedback2 = device->createTexture(desc);

        desc.format = nvrhi::Format::SRGBA8_UNORM;
        desc.isUAV = false;
        desc.debugName = "LdrColor";
        LdrColor = device->createTexture(desc);

        desc.format = nvrhi::Format::R8_UNORM;
        desc.isUAV = true;
        desc.debugName = "AmbientOcclusion";
        AmbientOcclusion = device->createTexture(desc);

        if (desc.isVirtual)
        {
            uint64_t heapSize = 0;
            nvrhi::ITexture* const textures[] = {
                HdrColor,
                MaterialIDs,
                ResolvedColor,
                TemporalFeedback1,
                TemporalFeedback2,
                LdrColor,
                AmbientOcclusion,
                GBufferRTShadowsFinal, 
                GBufferRTAO
            };

            for (auto texture : textures)
            {
                nvrhi::MemoryRequirements memReq = device->getTextureMemoryRequirements(texture);
                heapSize = nvrhi::align(heapSize, memReq.alignment);
                heapSize += memReq.size;
            }

            nvrhi::HeapDesc heapDesc;
            heapDesc.type = nvrhi::HeapType::DeviceLocal;
            heapDesc.capacity = heapSize;
            heapDesc.debugName = "RenderTargetHeap";

            Heap = device->createHeap(heapDesc);

            uint64_t offset = 0;
            for (auto texture : textures)
            {
                nvrhi::MemoryRequirements memReq = device->getTextureMemoryRequirements(texture);
                offset = nvrhi::align(offset, memReq.alignment);

                device->bindTextureMemory(texture, Heap, offset);

                offset += memReq.size;
            }
        }
        
        ForwardFramebuffer = std::make_shared<FramebufferFactory>(device);
        ForwardFramebuffer->RenderTargets = { HdrColor };
        ForwardFramebuffer->DepthTarget = Depth;

        HdrFramebuffer = std::make_shared<FramebufferFactory>(device);
        HdrFramebuffer->RenderTargets = { HdrColor };

        LdrFramebuffer = std::make_shared<FramebufferFactory>(device);
        LdrFramebuffer->RenderTargets = { LdrColor };

        ResolvedFramebuffer = std::make_shared<FramebufferFactory>(device);
        ResolvedFramebuffer->RenderTargets = { ResolvedColor };

        MaterialIDFramebuffer = std::make_shared<FramebufferFactory>(device);
        MaterialIDFramebuffer->RenderTargets = { MaterialIDs };
        MaterialIDFramebuffer->DepthTarget = Depth;
    }

    [[nodiscard]] bool IsUpdateRequired(uint2 size, uint sampleCount) const
    {
        if (any(m_Size != size) || m_SampleCount != sampleCount)
            return true;

        return false;
    }

    void Clear(nvrhi::ICommandList* commandList) override
    {
        GBufferRenderTargets::Clear(commandList);

        commandList->clearTextureFloat(HdrColor, nvrhi::AllSubresources, nvrhi::Color(0.f));
    }
};

enum class AntiAliasingMode
{
    NONE,
    TEMPORAL,
    MSAA_2X,
    MSAA_4X,
    MSAA_8X
};

struct UIData
{
    bool                                ShowUI = true;
	bool                                ShowConsole = false;
    bool                                UseDeferredShading = true;
    bool                                Stereo = false;
    bool                                EnableSsao = true;
    SsaoParameters                      SsaoParams;
    ToneMappingParameters               ToneMappingParams;
    TemporalAntiAliasingParameters      TemporalAntiAliasingParams;
    SkyParameters                       SkyParams;
    enum AntiAliasingMode               AntiAliasingMode = AntiAliasingMode::NONE;
    enum TemporalAntiAliasingJitter     TemporalAntiAliasingJitter = TemporalAntiAliasingJitter::Halton;
    bool                                EnableVsync = true;
    bool                                ShaderReoladRequested = false;
    bool                                EnableProceduralSky = true;
    bool                                EnableBloom = true;
    float                               BloomSigma = 32.f;
    float                               BloomAlpha = 0.05f;
    bool                                EnableTranslucency = false;
    bool                                EnableMaterialEvents = false;
    bool                                EnableShadows = true;
    float                               AmbientIntensity = 0.05f;
    bool                                EnableLightProbe = false;
    float                               LightProbeDiffuseScale = 1.f;
    float                               LightProbeSpecularScale = 1.f;
    float                               CsmExponent = 4.f;
    bool                                DisplayShadowMap = false;
    bool                                UseThirdPersonCamera = false;
    bool                                EnableAnimations = false;
    std::shared_ptr<Material>           SelectedMaterial;
    std::shared_ptr<SceneGraphNode>     SelectedNode;
    std::shared_ptr<MeshInstance>       SelectedMeshInstance;
    std::string                         ScreenshotFileName;
    std::shared_ptr<SceneCamera>        ActiveSceneCamera;

#if defined(ENABLE_KickStartSDK)
    struct KickstartRT_Settings {
        bool            m_enableDebugSubViews = false;

        bool            m_enableReflection = true;
        bool            m_enableTransparentReflection = false;
        bool            m_enableGI = true;
        bool            m_enableAO = true;
        uint32_t        m_enableShadows = 0;
        bool            m_shadowsEnableFirstHitAndEndSearch = false;
        bool            m_enableWorldPosFromDepth = false;
        bool            m_enableDirectLightingSample = true;
        uint32_t        m_debugDisp = 0;
        bool            m_destructGeom = false;
#if KickstartRT_Demo_WITH_NRD
        bool            m_enableCheckerboard = true;
        float           m_maxRayLength = 1000.f;
        uint32_t        m_denoisingMethod = 2;
        uint32_t        m_aoDenoisingMethod = 1;
        uint32_t        m_shadowDenoisingMethod = 1;
#else
        bool            m_enableCheckerboard = false;
        uint32_t        m_denoisingMethod = 0;
        uint32_t        m_aoDenoisingMethod = 0;
        uint32_t        m_shadowDenoisingMethod = 0;
#endif
        bool            m_denoisingReset = false;
        bool            m_enableCameraJitter = false;
        bool            m_enableLateLightInjection = false;
        uint32_t        m_rayOffsetType = 1;
        float           m_rayOffset_WorldPosition_threshold = 1.f / 32.f;
        float           m_rayOffset_WorldPosition_floatScale = 1.f / 65536.f;
        float           m_rayOffset_WorldPosition_intScale = 8192.f;
        float           m_rayOffset_CamDistance_constant = 0.00174f;
        float           m_rayOffset_CamDistance_linear = -0.0001547f;
        float           m_rayOffset_CamDistance_quadratic = 0.0000996f;
        bool            m_enableGlobalRoughness = false;
        float           m_globalRoughness = 0.3f;
        bool            m_enableGlobalMetalness = false;
        float           m_globalMetalness = 1.0f;
        bool            m_useTraceRayInline = true;
        bool            m_performTransfer = false;

        bool            m_forceDirectTileMapping = false;
        uint32_t        m_surfelSampleMode = 0;
        uint32_t        m_surfelMode = 0;
        uint32_t        m_tileResolutionLimit = 64;
        float           m_tileUnitLength = 40.f;
        uint32_t        m_lightInjectionStride = 8;
        std::string     m_ExportShaderColdLoadListFileName;
    };
    KickstartRT_Settings              KS;
#endif
};

#if defined(ENABLE_KickStartSDK)
struct KickstartRT_SDK_Context
{
    struct GeomHandleType {
        std::shared_ptr<donut::engine::MeshGeometry> m_geomPtr;
#ifdef KickstartRT_Demo_WITH_D3D11
        struct D3D11 {
            SDK::D3D11::BVHTask::GeometryTask   m_gTask;
        };
        D3D11 m_11;
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
        struct D3D12 {
            SDK::D3D12::BVHTask::GeometryTask   m_gTask;
        };
        D3D12 m_12;
#endif
#ifdef KickstartRT_Demo_WITH_VK
        struct VK {
            SDK::VK::BVHTask::GeometryTask   m_gTask;
        };
        VK m_VK;
#endif
    };
    struct InstanceHandleType {
        std::shared_ptr<donut::engine::MeshInstance>    m_insPtr;
        GeomHandleType*                                 m_geomHandle = nullptr;
#ifdef KickstartRT_Demo_WITH_D3D11
        struct D3D11 {
            SDK::D3D11::BVHTask::InstanceTask   m_iTask;
        };
        D3D11 m_11;
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
        struct D3D12 {
            SDK::D3D12::BVHTask::InstanceTask   m_iTask;
        };
        D3D12 m_12;
#endif
#ifdef KickstartRT_Demo_WITH_VK
        struct VK {
            SDK::VK::BVHTask::InstanceTask      m_iTask;
        };
        VK m_VK;
#endif
    };
    
    struct DenoisingHandle {
#ifdef KickstartRT_Demo_WITH_D3D11
        SDK::D3D11::DenoisingContextHandle m_11 = SDK::D3D11::DenoisingContextHandle::Null;
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
        SDK::D3D12::DenoisingContextHandle m_12 = SDK::D3D12::DenoisingContextHandle::Null;
#endif
#ifdef KickstartRT_Demo_WITH_VK
        SDK::VK::DenoisingContextHandle m_VK = SDK::VK::DenoisingContextHandle::Null;
#endif
    };
    using GeomHandle = std::unique_ptr<GeomHandleType>;
    using InstanceHandle = std::unique_ptr<InstanceHandleType>;

    struct TaskContainer {
#ifdef KickstartRT_Demo_WITH_D3D11
        SDK::D3D11::TaskContainer* m_11 = nullptr;
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
        SDK::D3D12::TaskContainer* m_12 = nullptr;
#endif
#ifdef KickstartRT_Demo_WITH_VK
        SDK::VK::TaskContainer* m_VK = nullptr;
#endif
    };

#ifdef KickstartRT_Demo_WITH_D3D11
    struct D3D11 {
        SDK::D3D11::ExecuteContext*         m_executeContext = nullptr;
        uint64_t                            m_interopFenceValue = 0;
        nvrhi::RefCountPtr<ID3D11Fence>     m_interopFence;

        ~D3D11()
        {
            if (m_executeContext != nullptr) {
                SDK::D3D11::ExecuteContext::Destruct(m_executeContext);
                m_executeContext = nullptr;
            }
            m_interopFence->Release();
        };
    };
    std::unique_ptr<D3D11>   m_11;
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
    struct D3D12 {
        nvrhi::RefCountPtr<ID3D12Device5>               m_dev5;
        SDK::D3D12::ExecuteContext*                     m_executeContext = nullptr;

        std::vector<std::pair<SDK::D3D12::GPUTaskHandle, uint32_t>>     m_tasksInFlight;
        static constexpr uint kMaxRenderAheadFrames         = 3u; // This value is inflated on purpose, donut runs in lockstep with GPU without queuing up frames, it's increased to simulate higher workloads
        static constexpr uint kMaxTaskContainersPerFrame    = 3u;
        static constexpr uint kMinRequiredWorkingsets       = kMaxRenderAheadFrames * kMaxTaskContainersPerFrame;

        ~D3D12()
        {
            if (m_executeContext != nullptr) {
                SDK::D3D12::ExecuteContext::Destruct(m_executeContext);
                m_executeContext = nullptr;
            }
        };
    };
    std::unique_ptr<D3D12>   m_12;
#endif
#ifdef KickstartRT_Demo_WITH_VK
    struct VK {
        SDK::VK::ExecuteContext* 						                m_executeContext = nullptr;
        std::deque< std::pair<SDK::VK::GPUTaskHandle, uint32_t>>  m_tasksInFlight;
        static constexpr uint kMaxRenderAheadFrames                     = 3u; // This value is inflated on purpose, donut runs in lockstep with GPU without queuing up frames, it's increased to simulate higher workloads
        static constexpr uint kMaxTaskContainersPerFrame                = 3u;
        static constexpr uint kMinRequiredWorkingsets                   = kMaxRenderAheadFrames * kMaxTaskContainersPerFrame;

        ~VK()
        {
            if (m_executeContext != nullptr) {
                SDK::VK::ExecuteContext::Destruct(m_executeContext);
                m_executeContext = nullptr;
            }
        };
    };
    std::unique_ptr<VK>   m_vk;
#endif

    struct DenoisingContexts {
        DenoisingHandle specDiff;
        DenoisingHandle ao;
        DenoisingHandle shadow;
        uint64_t hash = 0;
    };

    struct InstanceState
    {
        bool instanceProp_DirectLightInjectionTarget = true;
        bool instanceProp_LightTransferSource = false;
        bool instanceProp_LightTransferTarget = false;
        bool instanceProp_VisibleInRT = true;
        bool isDirty = false;
    };

    std::map<donut::engine::MeshInfo*, GeomHandle>      m_geomHandles;
    std::map<donut::engine::MeshInstance*, InstanceHandle>  m_insHandles;
    std::map<donut::engine::MeshInstance*, InstanceState>  m_insStates;
    DenoisingContexts  m_denosingContext;
    TaskContainer      m_tc_preLighting;
    TaskContainer      m_tc;
    TaskContainer      m_tc_postLighting;

public:
    ~KickstartRT_SDK_Context()
    {
#ifdef KickstartRT_Demo_WITH_D3D11
        if (m_11)
            m_11.reset();
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
        if (m_12)
            m_12.reset();
#endif
#ifdef KickstartRT_Demo_WITH_VK
        if (m_vk)
            m_vk.reset();
#endif
    }
};
#endif

enum Layer {
    Opaque,
    Transparent0,
    Count
};

class FeatureDemo : public ApplicationBase
{
private:
    typedef ApplicationBase Super;

    std::shared_ptr<RootFileSystem>     m_RootFs;
	std::vector<std::string>            m_SceneFilesAvailable;
    std::string                         m_CurrentSceneName;
public:
	std::shared_ptr<Scene>				m_Scene;
private:
	std::shared_ptr<ShaderFactory>      m_ShaderFactory;
    std::shared_ptr<DirectionalLight>   m_SunLight;
    std::shared_ptr<CascadedShadowMap>  m_ShadowMap;
    std::shared_ptr<FramebufferFactory> m_ShadowFramebuffer;
    std::shared_ptr<DepthPass>          m_ShadowDepthPass;
    std::shared_ptr<InstancedOpaqueDrawStrategy> m_OpaqueDrawStrategy;
    std::shared_ptr<TransparentDrawStrategy> m_TransparentDrawStrategy;
    std::unique_ptr<RenderTargets>      m_RenderTargets[Layer::Count];
    std::shared_ptr<ForwardShadingPass> m_ForwardPass;
    std::unique_ptr<GBufferFillPass>    m_GBufferPass[Layer::Count];
    std::unique_ptr<DeferredLightingPass> m_DeferredLightingPass;
    std::unique_ptr<SkyPass>            m_SkyPass;
    std::unique_ptr<TemporalAntiAliasingPass> m_TemporalAntiAliasingPass;
    std::unique_ptr<BloomPass>          m_BloomPass;
    std::unique_ptr<ToneMappingPass>    m_ToneMappingPass;
    std::unique_ptr<SsaoPass>           m_SsaoPass;
    std::shared_ptr<LightProbeProcessingPass> m_LightProbePass;
    std::unique_ptr<MaterialIDPass>     m_MaterialIDPass;
    std::unique_ptr<PixelReadbackPass>  m_PixelReadbackPass;

    std::shared_ptr<IView>              m_View;
    std::shared_ptr<IView>              m_ViewPrevious;
    
    nvrhi::CommandListHandle            m_CommandList;
    nvrhi::CommandListHandle            m_CommandListKS_PreLighting;
    nvrhi::CommandListHandle            m_CommandListKS;
    nvrhi::CommandListHandle            m_CommandListKS_Post;
    bool                                m_PreviousViewsValid = false;
    FirstPersonCamera                   m_FirstPersonCamera;
    ThirdPersonCamera                   m_ThirdPersonCamera;
    BindingCache                        m_BindingCache;
    
    float                               m_CameraVerticalFov = 60.f;
    float3                              m_AmbientTop = 0.f;
    float3                              m_AmbientBottom = 0.f;
    uint2                               m_PickPosition = 0u;
    bool                                m_Pick = false;
    
    std::vector<std::shared_ptr<LightProbe>> m_LightProbes;
    nvrhi::TextureHandle                m_LightProbeDiffuseTexture;
    nvrhi::TextureHandle                m_LightProbeSpecularTexture;

    float                               m_WallclockTime = 0.f;
    
    UIData&                             m_ui;

#if defined(ENABLE_KickStartSDK)
public:
    KickstartRT_SDK_Context               m_SDKContext;
private:
    std::unique_ptr<KickStart_Composite>    m_SDKComposite;
#endif

public:

    FeatureDemo(DeviceManager* deviceManager, UIData& ui, const std::string& sceneName)
        : Super(deviceManager)
        , m_ui(ui)
        , m_BindingCache(deviceManager->GetDevice())
    { 
        std::shared_ptr<NativeFileSystem> nativeFS = std::make_shared<NativeFileSystem>();

        std::filesystem::path mediaPath = app::GetDirectoryWithExecutable().parent_path()/ "media";
        std::filesystem::path frameworkShaderPath = app::GetDirectoryWithExecutable() / "shaders" / "framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
        
        m_RootFs = std::make_shared<RootFileSystem>();
        m_RootFs->mount("/media", mediaPath);
        m_RootFs->mount("/shaders/donut", frameworkShaderPath);
        m_RootFs->mount("/native", nativeFS);

        std::filesystem::path scenePath = "/media/glTF-Sample-Models/2.0";
        m_SceneFilesAvailable = FindScenes(*m_RootFs, scenePath);

        if (sceneName.empty() && m_SceneFilesAvailable.empty())
        {
            log::fatal("No scene file found in media folder '%s'\n"
                "Please make sure that folder contains valid scene files.", scenePath.generic_string().c_str());
        }
        
        m_TextureCache = std::make_shared<TextureCache>(GetDevice(), m_RootFs, nullptr);

        m_ShaderFactory = std::make_shared<ShaderFactory>(GetDevice(), m_RootFs, "/shaders");
        m_CommonPasses = std::make_shared<CommonRenderPasses>(GetDevice(), m_ShaderFactory);

        m_OpaqueDrawStrategy = std::make_shared<InstancedOpaqueDrawStrategy>();
        m_TransparentDrawStrategy = std::make_shared<TransparentDrawStrategy>();

        m_ShadowMap = std::make_shared<CascadedShadowMap>(GetDevice(), 2048, 4, 0, nvrhi::Format::D24S8);
        m_ShadowMap->SetupProxyViews();
        
        m_ShadowFramebuffer = std::make_shared<FramebufferFactory>(GetDevice());
        m_ShadowFramebuffer->DepthTarget = m_ShadowMap->GetTexture();
        
        DepthPass::CreateParameters shadowDepthParams;
        shadowDepthParams.slopeScaledDepthBias = 4.f;
        shadowDepthParams.depthBias = 100;
        m_ShadowDepthPass = std::make_shared<DepthPass>(GetDevice(), m_CommonPasses);
        m_ShadowDepthPass->Init(*m_ShaderFactory, shadowDepthParams);

        m_CommandList = GetDevice()->createCommandList();
        m_CommandListKS_PreLighting = GetDevice()->createCommandList();
        m_CommandListKS = GetDevice()->createCommandList();
        m_CommandListKS_Post = GetDevice()->createCommandList();

        m_FirstPersonCamera.SetMoveSpeed(3.0f);
        m_ThirdPersonCamera.SetMoveSpeed(3.0f);
        
        SetAsynchronousLoadingEnabled(true);

        if (sceneName.empty())
            SetCurrentSceneName(app::FindPreferredScene(m_SceneFilesAvailable, "Sponza.gltf"));
        else
            SetCurrentSceneName("/native/" + sceneName);

        CreateLightProbes(4);

#if defined(ENABLE_KickStartSDK)

        m_ui.KS.m_useTraceRayInline &= GetDevice()->queryFeatureSupport(nvrhi::Feature::RayQuery);

#ifdef KickstartRT_Demo_WITH_D3D11
		if (GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11) {
			SDK::D3D11::ExecuteContext_InitSettings settings = {};
			settings.D3D11Device = reinterpret_cast<ID3D11Device*>(GetDevice()->getNativeObject(nvrhi::ObjectTypes::D3D11_Device).pointer);
			settings.DXGIAdapter = reinterpret_cast<IDXGIAdapter1*>(GetDeviceManager()->GetDXGIAdapter1());
			settings.usingCommandQueue = SDK::D3D11::ExecuteContext_InitSettings::UsingCommandQueue::Direct;
            settings.supportedWorkingSet = KickstartRT_SDK_Context::D3D12::kMinRequiredWorkingsets;
            settings.uploadHeapSizeForVolatileConstantBuffers = 8u * 64u * 1024u;
            settings.descHeapSize = 8u * 8192u;

            m_SDKContext.m_11 = std::make_unique<KickstartRT_SDK_Context::D3D11>();
            SDK::Status sts;
            for (;;) {
                sts = SDK::D3D11::ExecuteContext::Init(&settings, &m_SDKContext.m_11->m_executeContext);
                if (sts != SDK::Status::OK) {
                    log::fatal("Failed to init KickStartSDK. %d", (uint32_t)sts);
                    break;
                }

                nvrhi::RefCountPtr<ID3D11Device5>   dev5;
                settings.D3D11Device->QueryInterface(IID_PPV_ARGS(&dev5));
                if (!dev5) {
                    log::fatal("Failed to get ID3D11Device5 interface.");
                    sts = SDK::Status::ERROR_INTERNAL;
                    break;
                }
                dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_SDKContext.m_11->m_interopFence));
                break;
            }
            if (sts != SDK::Status::OK) {
                m_SDKContext.m_11.reset();
            }
		}
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
        if (GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12) {
            m_SDKContext.m_12 = std::make_unique<KickstartRT_SDK_Context::D3D12>();

            SDK::D3D12::ExecuteContext_InitSettings settings = {};
            {
                ID3D12Device* dev = reinterpret_cast<ID3D12Device*>(GetDevice()->getNativeObject(nvrhi::ObjectTypes::D3D12_Device).pointer);
                dev->QueryInterface(IID_PPV_ARGS(&m_SDKContext.m_12->m_dev5));
                settings.D3D12Device = m_SDKContext.m_12->m_dev5.Get();
            }
            settings.supportedWorkingsets = KickstartRT_SDK_Context::D3D12::kMinRequiredWorkingsets;
            settings.useInlineRaytracing = m_ui.KS.m_useTraceRayInline;
            settings.uploadHeapSizeForVolatileConstantBuffers = 8u * 64u * 1024u;
            settings.descHeapSize = 8u * 8192u;

            std::vector<uint32_t> shaderColdLoadList;
            {
                std::filesystem::path exePath = donut::app::GetDirectoryWithExecutable();
                exePath /= "ColdLoadShaderList.bin";

                std::ifstream ifs(exePath.string().c_str(), std::ios::binary | std::ios::in);
                if (ifs.is_open() && (!ifs.bad())) {
                    ifs.seekg(0, std::ios_base::end);
                    size_t size = ifs.tellg();
                    ifs.seekg(0, std::ios_base::beg);
                    if (size % sizeof(uint32_t) == 0) {
                        shaderColdLoadList.resize(size / 4);
                        ifs.read((char *)shaderColdLoadList.data(), size);

                        settings.coldLoadShaderList = shaderColdLoadList.data();
                        settings.coldLoadShaderListSize = (uint32_t)shaderColdLoadList.size();
                    }
                }
            }

            SDK::Status sts = SDK::D3D12::ExecuteContext::Init(&settings, &m_SDKContext.m_12->m_executeContext);
            if (sts != SDK::Status::OK) {
                log::fatal("Failed to init KickStartSDK. %d", (uint32_t)sts);
                m_SDKContext.m_12.reset();
            }
        }
#endif
#ifdef KickstartRT_Demo_WITH_VK
        if (GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN) {
            m_SDKContext.m_vk = std::make_unique<KickstartRT_SDK_Context::VK>();

            bool allExtAvailable = true;
            allExtAvailable &= GetDeviceManager()->IsVulkanDeviceExtensionEnabled(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            allExtAvailable &= GetDeviceManager()->IsVulkanDeviceExtensionEnabled(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
            allExtAvailable &= GetDeviceManager()->IsVulkanDeviceExtensionEnabled(VK_KHR_RAY_QUERY_EXTENSION_NAME);
            allExtAvailable &= GetDeviceManager()->IsVulkanDeviceExtensionEnabled(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            allExtAvailable &= GetDeviceManager()->IsVulkanDeviceExtensionEnabled(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
            if (!allExtAvailable) {
                log::fatal("Some of vulkan extension was not supported.");
                m_SDKContext.m_vk.reset();
            }
            if (m_SDKContext.m_vk) {
                SDK::VK::ExecuteContext_InitSettings settings = {};
                settings.instance = reinterpret_cast<VkInstance>(GetDevice()->getNativeObject(nvrhi::ObjectTypes::VK_Instance).pointer);
                settings.physicalDevice = reinterpret_cast<VkPhysicalDevice>(GetDevice()->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice).pointer);
                settings.device = reinterpret_cast<VkDevice>(GetDevice()->getNativeObject(nvrhi::ObjectTypes::VK_Device).pointer);
                settings.supportedWorkingsets = KickstartRT_SDK_Context::VK::kMinRequiredWorkingsets;
                settings.useInlineRaytracing = m_ui.KS.m_useTraceRayInline;

                SDK::Status sts = SDK::VK::ExecuteContext::Init(&settings, &m_SDKContext.m_vk->m_executeContext);
                if (sts != SDK::Status::OK) {
                    log::fatal("Failed to init KickStartSDK. %d", (uint32_t)sts);
                    m_SDKContext.m_vk.reset();
                }
            }
        }
#endif
#endif
    }

	std::shared_ptr<vfs::IFileSystem> GetRootFs() const
    {
		return m_RootFs;
	}

    BaseCamera& GetActiveCamera() const
    {
        return m_ui.UseThirdPersonCamera ? (BaseCamera&)m_ThirdPersonCamera : (BaseCamera&)m_FirstPersonCamera;
    }

	std::vector<std::string> const& GetAvailableScenes() const
	{
		return m_SceneFilesAvailable;
	}

    std::string GetCurrentSceneName() const
    {
        return m_CurrentSceneName;
    }

    void SetCurrentSceneName(const std::string& sceneName)
    {
        if (m_CurrentSceneName == sceneName)
            return;

		m_CurrentSceneName = sceneName;

		BeginLoadingScene(m_RootFs, m_CurrentSceneName);
    }

    void CopyActiveCameraToFirstPerson()
    {
        if (m_ui.ActiveSceneCamera)
        {
            dm::affine3 viewToWorld = m_ui.ActiveSceneCamera->GetViewToWorldMatrix();
            dm::float3 cameraPos = viewToWorld.m_translation;
            m_FirstPersonCamera.LookAt(cameraPos, cameraPos + viewToWorld.m_linear.row2, viewToWorld.m_linear.row1);
        }
        else if (m_ui.UseThirdPersonCamera)
        {
            m_FirstPersonCamera.LookAt(m_ThirdPersonCamera.GetPosition(), m_ThirdPersonCamera.GetPosition() + m_ThirdPersonCamera.GetDir(), m_ThirdPersonCamera.GetUp());
        }
    }

    virtual bool KeyboardUpdate(int key, int scancode, int action, int mods) override
    {
		if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
		{
            m_ui.ShowUI = !m_ui.ShowUI;
            return true;	
		}

		if (key == GLFW_KEY_GRAVE_ACCENT && action == GLFW_PRESS)
        {
			m_ui.ShowConsole = !m_ui.ShowConsole;
			return true;
        }

        if (key == GLFW_KEY_SPACE && action == GLFW_PRESS)
        {
            m_ui.EnableAnimations = !m_ui.EnableAnimations;
            return true;
        }

        if (key == GLFW_KEY_T && action == GLFW_PRESS)
        {
            CopyActiveCameraToFirstPerson();
            if (m_ui.ActiveSceneCamera)
            {
                m_ui.UseThirdPersonCamera = false;
                m_ui.ActiveSceneCamera = nullptr;
            }
            else
            {
                m_ui.UseThirdPersonCamera = !m_ui.UseThirdPersonCamera;
            }
            return true;
        }

        if (!m_ui.ActiveSceneCamera)
            GetActiveCamera().KeyboardUpdate(key, scancode, action, mods);
        return true;
    }

    virtual bool MousePosUpdate(double xpos, double ypos) override
    {
        if (!m_ui.ActiveSceneCamera)
            GetActiveCamera().MousePosUpdate(xpos, ypos);

        m_PickPosition = uint2(static_cast<uint>(xpos), static_cast<uint>(ypos));

        return true;
    }

    virtual bool MouseButtonUpdate(int button, int action, int mods) override
    {
        if (!m_ui.ActiveSceneCamera)
            GetActiveCamera().MouseButtonUpdate(button, action, mods);
        
        if (action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_2)
            m_Pick = true;

        return true;
    }

    virtual bool MouseScrollUpdate(double xoffset, double yoffset) override
    {
        if (!m_ui.ActiveSceneCamera)
            GetActiveCamera().MouseScrollUpdate(xoffset, yoffset);

        return true;
    }

    virtual void Animate(float fElapsedTimeSeconds) override
    { 
        if (!m_ui.ActiveSceneCamera)
            GetActiveCamera().Animate(fElapsedTimeSeconds);

        if(m_ToneMappingPass)
            m_ToneMappingPass->AdvanceFrame(fElapsedTimeSeconds);
        
        if (IsSceneLoaded() && m_ui.EnableAnimations)
        {
            m_WallclockTime += fElapsedTimeSeconds;

            for (const auto& anim : m_Scene->GetSceneGraph()->GetAnimations())
            {
                float duration = anim->GetDuration();
                float integral;
                float animationTime = std::modf(m_WallclockTime / duration, &integral) * duration;
                (void)anim->Apply(animationTime);
            }
        }
    }


    virtual void SceneUnloading() override
    {
        if (m_ForwardPass) m_ForwardPass->ResetBindingCache();
        if (m_DeferredLightingPass) m_DeferredLightingPass->ResetBindingCache();
        for (int i = 0; i < Layer::Count; ++i)
            if (m_GBufferPass[i]) m_GBufferPass[i]->ResetBindingCache();
        if (m_LightProbePass) m_LightProbePass->ResetCaches();
        if (m_ShadowDepthPass) m_ShadowDepthPass->ResetBindingCache();
        m_BindingCache.Clear();
        m_SunLight.reset();
        m_ui.SelectedMaterial = nullptr;
        m_ui.SelectedNode = nullptr;

        for (auto probe : m_LightProbes)
        {
            probe->enabled = false;
        }

#if defined(ENABLE_KickStartSDK)
        // request to destruct all geom and instance.
        // Remove all the current geometries after GPU - CPU sync.
        GetDevice()->waitForIdle();
#if defined(KickstartRT_Demo_WITH_D3D12)
        if (m_SDKContext.m_12) {
            for (auto it = m_SDKContext.m_12->m_tasksInFlight.begin(); it != m_SDKContext.m_12->m_tasksInFlight.end();) {
                auto sts = m_SDKContext.m_12->m_executeContext->MarkGPUTaskAsCompleted(it->first);
                if (sts != SDK::Status::OK) {
                    log::fatal("KickStartRTX: FinishGPUTask() failed. : %d", (uint32_t)sts);
                }
                it = m_SDKContext.m_12->m_tasksInFlight.erase(it);
            }
            m_SDKContext.m_12->m_executeContext->DestroyAllInstanceHandles();
            m_SDKContext.m_12->m_executeContext->DestroyAllGeometryHandles();
            m_SDKContext.m_12->m_executeContext->ReleaseDeviceResourcesImmediately();
            m_SDKContext.m_insHandles.clear();
            m_SDKContext.m_geomHandles.clear();
            m_SDKContext.m_insStates.clear();
        }
#endif
#if defined(KickstartRT_Demo_WITH_VK)
        if (m_SDKContext.m_vk) {
            for (auto it = m_SDKContext.m_vk->m_tasksInFlight.begin(); it != m_SDKContext.m_vk->m_tasksInFlight.end();) {
                auto sts = m_SDKContext.m_vk->m_executeContext->MarkGPUTaskAsCompleted(it->first);
                if (sts != SDK::Status::OK) {
                    log::fatal("KickStartRTX: FinishGPUTask() failed. : %d", (uint32_t)sts);
                }
                it = m_SDKContext.m_vk->m_tasksInFlight.erase(it);
            }
            m_SDKContext.m_vk->m_executeContext->DestroyAllInstanceHandles();
            m_SDKContext.m_vk->m_executeContext->DestroyAllGeometryHandles();
            m_SDKContext.m_vk->m_executeContext->ReleaseDeviceResourcesImmediately();
            m_SDKContext.m_insHandles.clear();
            m_SDKContext.m_geomHandles.clear();
            m_SDKContext.m_insStates.clear();
        }
#endif
#if defined(KickstartRT_Demo_WITH_D3D11)
        if (m_SDKContext.m_11) {
            m_SDKContext.m_11->m_executeContext->DestroyAllInstanceHandles();
            m_SDKContext.m_11->m_executeContext->DestroyAllGeometryHandles();
            m_SDKContext.m_11->m_executeContext->ReleaseDeviceResourcesImmediately();
            m_SDKContext.m_insHandles.clear();
            m_SDKContext.m_geomHandles.clear();
            m_SDKContext.m_insStates.clear();
        }
#endif
#endif
    }

    virtual bool LoadScene(std::shared_ptr<IFileSystem> fs, const std::filesystem::path& fileName) override
    {
        using namespace std::chrono;

        Scene* scene = new Scene(GetDevice(), *m_ShaderFactory, fs, m_TextureCache, nullptr, nullptr);

        auto startTime = high_resolution_clock::now();

        if (scene->Load(fileName))
        {
            m_Scene = std::unique_ptr<Scene>(scene);

            auto endTime = high_resolution_clock::now();
            auto duration = duration_cast<milliseconds>(endTime - startTime).count();
            log::info("Scene loading time: %llu ms", duration);

            return true;
        }
        
        return false;
    }
    
    virtual void SceneLoaded() override
    {
        Super::SceneLoaded();

        bool sharedAcrossDevice = false;
#if defined(ENABLE_KickStartSDK)
#ifdef KickstartRT_Demo_WITH_D3D11
        if (m_SDKContext.m_11) {
            sharedAcrossDevice = true;
        }
#endif
#endif
        
        m_Scene->FinishedLoading(GetFrameIndex(), sharedAcrossDevice);

        m_WallclockTime = 0.f;
        m_PreviousViewsValid = false;

        for (auto light : m_Scene->GetSceneGraph()->GetLights())
        {
            if (light->GetLightType() == LightType_Directional)
            {
                m_SunLight = std::static_pointer_cast<DirectionalLight>(light);
                break;
            }
        }

        if (!m_SunLight)
        {
            m_SunLight = std::make_shared<DirectionalLight>();
            m_SunLight->angularSize = 0.53f;
            m_SunLight->irradiance = 1.f;

            auto node = std::make_shared<SceneGraphNode>();
            node->SetLeaf(m_SunLight);
            m_SunLight->SetDirection(dm::double3(0.1, -0.9, 0.1));
            m_SunLight->SetName("Sun");
            m_Scene->GetSceneGraph()->Attach(m_Scene->GetSceneGraph()->GetRootNode(), node);
        }
        
        auto cameras = m_Scene->GetSceneGraph()->GetCameras();
        if (!cameras.empty())
        {
            m_ui.ActiveSceneCamera = cameras[0];
        }
        else
        {
            m_ui.ActiveSceneCamera.reset();

            m_FirstPersonCamera.LookAt(
                float3(0.f, 1.8f, 0.f),
                float3(1.f, 1.8f, 0.f));
            m_CameraVerticalFov = 60.f;
        }
        
        m_ThirdPersonCamera.SetRotation(dm::radians(135.f), dm::radians(20.f));
        PointThirdPersonCameraAt(m_Scene->GetSceneGraph()->GetRootNode());
#if 0
        m_ui.UseThirdPersonCamera = string_utils::ends_with(m_CurrentSceneName, ".gltf")
            || string_utils::ends_with(m_CurrentSceneName, ".glb");
#endif

        CopyActiveCameraToFirstPerson();

        if (g_PrintSceneGraph)
            PrintSceneGraph(m_Scene->GetSceneGraph()->GetRootNode());
    }

    void PointThirdPersonCameraAt(const std::shared_ptr<SceneGraphNode>& node)
    {
        dm::box3 bounds = node->GetGlobalBoundingBox();
        m_ThirdPersonCamera.SetTargetPosition(bounds.center());
        float radius = length(bounds.diagonal()) * 0.5f;
        float distance = radius / sinf(dm::radians(m_CameraVerticalFov * 0.5f));
        m_ThirdPersonCamera.SetDistance(distance);
        m_ThirdPersonCamera.Animate(0.f);
    }

    bool IsStereo()
    {
        return m_ui.Stereo;
    }

    std::shared_ptr<TextureCache> GetTextureCache()
    {
        return m_TextureCache;
    }

    std::shared_ptr<Scene> GetScene()
    {
        return m_Scene;
    }

    bool SetupView(bool reverseDepth)
    {
        float2 renderTargetSize = float2(m_RenderTargets[Layer::Opaque]->GetSize());

        if (m_TemporalAntiAliasingPass)
            m_TemporalAntiAliasingPass->SetJitter(m_ui.TemporalAntiAliasingJitter);


        float2 pixelOffset = m_ui.AntiAliasingMode == AntiAliasingMode::TEMPORAL && m_TemporalAntiAliasingPass
            ? m_TemporalAntiAliasingPass->GetCurrentPixelOffset()
            : float2(0.f);

#if defined(ENABLE_KickStartSDK)
        pixelOffset = (m_ui.AntiAliasingMode == AntiAliasingMode::TEMPORAL || m_ui.KS.m_enableCameraJitter) && m_TemporalAntiAliasingPass
            ? m_TemporalAntiAliasingPass->GetCurrentPixelOffset()
            : float2(0.f);
#endif
        
        std::shared_ptr<StereoPlanarView> stereoView = std::dynamic_pointer_cast<StereoPlanarView, IView>(m_View);
        std::shared_ptr<PlanarView> planarView = std::dynamic_pointer_cast<PlanarView, IView>(m_View);

        dm::affine3 viewMatrix;
        float verticalFov = dm::radians(m_CameraVerticalFov);
        float zNear = reverseDepth ? 0.01f : 0.01f;
        float zFar = 1000.0f;
        if (m_ui.ActiveSceneCamera)
        {
            auto perspectiveCamera = std::dynamic_pointer_cast<PerspectiveCamera>(m_ui.ActiveSceneCamera);
            if (perspectiveCamera)
            {
                zNear = perspectiveCamera->zNear;
                zFar = perspectiveCamera->zFar.has_value() ? *perspectiveCamera->zFar : 0.01f;
                verticalFov = perspectiveCamera->verticalFov;
            }

            viewMatrix = m_ui.ActiveSceneCamera->GetWorldToViewMatrix();
        }
        else
        {
            viewMatrix = GetActiveCamera().GetWorldToViewMatrix();
        }

        bool topologyChanged = false;

        if (IsStereo())
        {
            if (!stereoView)
            {
                m_View = stereoView = std::make_shared<StereoPlanarView>();
                m_ViewPrevious = std::make_shared<StereoPlanarView>();
                topologyChanged = true;
            }

            stereoView->LeftView.SetViewport(nvrhi::Viewport(renderTargetSize.x * 0.5f, renderTargetSize.y));
            stereoView->LeftView.SetPixelOffset(pixelOffset);

            stereoView->RightView.SetViewport(nvrhi::Viewport(renderTargetSize.x * 0.5f, renderTargetSize.x, 0.f, renderTargetSize.y, 0.f, 1.f));
            stereoView->RightView.SetPixelOffset(pixelOffset);

            {
                float4x4 projection = perspProjD3DStyleReverse(verticalFov, renderTargetSize.x / renderTargetSize.y * 0.5f, zNear);

                affine3 leftView = viewMatrix;
                stereoView->LeftView.SetMatrices(leftView, projection);

                affine3 rightView = leftView;
                rightView.m_translation -= float3(0.2f, 0, 0);
                stereoView->RightView.SetMatrices(rightView, projection);
            }

            stereoView->LeftView.UpdateCache();
            stereoView->RightView.UpdateCache();

            m_ThirdPersonCamera.SetView(stereoView->LeftView);

            if (topologyChanged)
            {
                *std::static_pointer_cast<StereoPlanarView>(m_ViewPrevious) = *std::static_pointer_cast<StereoPlanarView>(m_View);
            }
        }
        else
        {
            if (!planarView)
            {
                m_View = planarView = std::make_shared<PlanarView>();
                m_ViewPrevious = std::make_shared<PlanarView>();
                topologyChanged = true;
            }

            /// float4x4 projection = perspProjD3DStyleReverse(verticalFov, renderTargetSize.x / renderTargetSize.y, zNear);
            float4x4 projection = reverseDepth ?
                perspProjD3DStyleReverse(verticalFov, renderTargetSize.x / renderTargetSize.y, zNear) : 
                perspProjD3DStyle(verticalFov, renderTargetSize.x / renderTargetSize.y, zNear, zFar);

            planarView->SetViewport(nvrhi::Viewport(renderTargetSize.x, renderTargetSize.y));
            planarView->SetPixelOffset(pixelOffset);

            planarView->SetMatrices(viewMatrix, projection);
            planarView->UpdateCache();

            m_ThirdPersonCamera.SetView(*planarView);

            if (topologyChanged)
            {
                *std::static_pointer_cast<PlanarView>(m_ViewPrevious) = *std::static_pointer_cast<PlanarView>(m_View);
            }
        }
        
        return topologyChanged;
    }

    void CreateRenderPasses(bool& exposureResetRequired)
    {
        uint32_t motionVectorStencilMask = 0x01;
        
        ForwardShadingPass::CreateParameters ForwardParams;
        ForwardParams.trackLiveness = false;
        m_ForwardPass = std::make_unique<ForwardShadingPass>(GetDevice(), m_CommonPasses);
        m_ForwardPass->Init(*m_ShaderFactory, ForwardParams);
        
        for (int i = 0; i < Layer::Count; ++i)
        {
            GBufferFillPass::CreateParameters GBufferParams;
            GBufferParams.enableMotionVectors = true;
            GBufferParams.stencilWriteMask = motionVectorStencilMask;
            m_GBufferPass[i] = std::make_unique<GBufferFillPass>(GetDevice(), m_CommonPasses);
            m_GBufferPass[i]->Init(*m_ShaderFactory, GBufferParams);
        }

        GBufferFillPass::CreateParameters GBufferParams;
        GBufferParams.enableMotionVectors = false;
        GBufferParams.stencilWriteMask = motionVectorStencilMask;
        m_MaterialIDPass = std::make_unique<MaterialIDPass>(GetDevice(), m_CommonPasses);
        m_MaterialIDPass->Init(*m_ShaderFactory, GBufferParams);

        m_PixelReadbackPass = std::make_unique<PixelReadbackPass>(GetDevice(), m_ShaderFactory, m_RenderTargets[Layer::Opaque]->MaterialIDs, nvrhi::Format::RGBA32_UINT);

        m_DeferredLightingPass = std::make_unique<DeferredLightingPass>(GetDevice(), m_CommonPasses);
        m_DeferredLightingPass->Init(m_ShaderFactory);

        m_SkyPass = std::make_unique<SkyPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets[Layer::Opaque]->ForwardFramebuffer, *m_View);
        
        {
            TemporalAntiAliasingPass::CreateParameters taaParams;
            taaParams.sourceDepth = m_RenderTargets[Layer::Opaque]->Depth;
            taaParams.motionVectors = m_RenderTargets[Layer::Opaque]->MotionVectors;
            taaParams.unresolvedColor = m_RenderTargets[Layer::Opaque]->HdrColor;
            taaParams.resolvedColor = m_RenderTargets[Layer::Opaque]->ResolvedColor;
            taaParams.feedback1 = m_RenderTargets[Layer::Opaque]->TemporalFeedback1;
            taaParams.feedback2 = m_RenderTargets[Layer::Opaque]->TemporalFeedback2;
            taaParams.motionVectorStencilMask = motionVectorStencilMask;
            taaParams.useCatmullRomFilter = true;

            m_TemporalAntiAliasingPass = std::make_unique<TemporalAntiAliasingPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, *m_View, taaParams);
        }

        if (m_RenderTargets[Layer::Opaque]->GetSampleCount() == 1)
        {
            m_SsaoPass = std::make_unique<SsaoPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets[Layer::Opaque]->Depth, m_RenderTargets[Layer::Opaque]->GBufferNormals, m_RenderTargets[Layer::Opaque]->AmbientOcclusion);
        }

        m_LightProbePass = std::make_shared<LightProbeProcessingPass>(GetDevice(), m_ShaderFactory, m_CommonPasses);

        nvrhi::BufferHandle exposureBuffer = nullptr;
        if (m_ToneMappingPass)
            exposureBuffer = m_ToneMappingPass->GetExposureBuffer();
        else
            exposureResetRequired = true;

        ToneMappingPass::CreateParameters toneMappingParams;
        toneMappingParams.exposureBufferOverride = exposureBuffer;
        m_ToneMappingPass = std::make_unique<ToneMappingPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets[Layer::Opaque]->LdrFramebuffer, *m_View, toneMappingParams);

        m_BloomPass = std::make_unique<BloomPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets[Layer::Opaque]->ResolvedFramebuffer, *m_View);

#if defined(ENABLE_KickStartSDK)
        {
            m_SDKComposite = std::make_unique<KickStart_Composite>(
                GetDevice(),
                m_ShaderFactory,
                m_CommonPasses,
                m_RenderTargets[Layer::Opaque]->HdrFramebuffer,
                m_RenderTargets[Layer::Opaque]->GBufferDiffuse,
                m_RenderTargets[Layer::Opaque]->GBufferRTReflectionsFinal,
                m_RenderTargets[Layer::Opaque]->GBufferRTGIFinal,
                m_RenderTargets[Layer::Opaque]->GBufferRTAOFinal,
                m_RenderTargets[Layer::Opaque]->GBufferRTShadowsFinal
                );
            if (!m_SDKComposite) {
                log::fatal("Failed to initialize SDK composite pass.");
            }
        }
#endif

        m_PreviousViewsValid = false;
    }

    virtual void RenderSplashScreen(nvrhi::IFramebuffer* framebuffer) override
    {
        nvrhi::ITexture* framebufferTexture = framebuffer->getDesc().colorAttachments[0].texture;
        m_CommandList->open();
        m_CommandList->clearTextureFloat(framebufferTexture, nvrhi::AllSubresources, nvrhi::Color(0.f));
        m_CommandList->close();
        GetDevice()->executeCommandList(m_CommandList);
        GetDeviceManager()->SetVsyncEnabled(true);
    }

#if defined(ENABLE_KickStartSDK)
    template<class TSDK_X_Lighting_LightInfo>
    uint _SetupLightInfos(TSDK_X_Lighting_LightInfo* lightInfos/* T = SDK::X::Task::LightInfo*/, uint maxLightNum) {
       
        constexpr bool kEnableDirectional   = true;
        constexpr bool kEnableSpot          = true;
        constexpr bool kEnablePoint         = true;

        const uint32_t kMaxLightNum         = min((uint32_t)m_Scene->GetSceneGraph()->GetLights().size(), maxLightNum);
        uint numLights = 0;
        for (uint32_t i = 0; i < kMaxLightNum; ++i) {
            const auto& light = m_Scene->GetSceneGraph()->GetLights()[i];
            if (light->GetLightType() == LightType_Directional && kEnableDirectional)
            {
                auto dir = std::static_pointer_cast<DirectionalLight>(light);
            
                TSDK_X_Lighting_LightInfo& info = lightInfos[numLights++];
            
                info.type = TSDK_X_Lighting_LightInfo::Type::Directional;
                info.dir.angularExtent = dm::radians(dir->angularSize);
                info.dir.intensity = 1.f;
                info.dir.dir = {
                    (float)-dir->GetDirection().x, (float)-dir->GetDirection().y, (float)-dir->GetDirection().z,
                };
            }
            if (light->GetLightType() == LightType_Spot && kEnableSpot)
            {
                auto spot = std::static_pointer_cast<SpotLight>(light);

                TSDK_X_Lighting_LightInfo& info = lightInfos[numLights++];

                info.type = TSDK_X_Lighting_LightInfo::Type::Spot;
                info.spot.radius = spot->radius;
                info.spot.intensity = spot->intensity;
                info.spot.apexAngle = dm::radians(spot->outerAngle);
                info.spot.range = spot->range;
                info.spot.dir = {
                    (float)spot->GetDirection().x, (float)spot->GetDirection().y, (float)spot->GetDirection().z,
                };
                info.spot.pos = { (float)spot->GetPosition().x, (float)spot->GetPosition().y, (float)spot->GetPosition().z };
            }
            if (light->GetLightType() == LightType_Point && kEnablePoint)
            {
                auto point = std::static_pointer_cast<PointLight>(light);
            
                TSDK_X_Lighting_LightInfo &info = lightInfos[numLights++];
            
                info.type = TSDK_X_Lighting_LightInfo::Type::Point;
                info.point.intensity = point->intensity;
                info.point.radius = point->radius;
                info.point.range = point->range;
                info.point.pos = { (float)point->GetPosition().x, (float)point->GetPosition().y, (float)point->GetPosition().z };
            }
        }

        return numLights;
    }

#ifdef KickstartRT_Demo_WITH_D3D11

    SDK::D3D11::RenderTask::ShaderResourceTex GetShaderResourceTexD3D11(nvrhi::TextureHandle texHandle) {
        auto& desc = texHandle->getDesc();
        assert(desc.dimension == nvrhi::TextureDimension::Texture2D);

        SDK::D3D11::RenderTask::ShaderResourceTex tex;
        tex.resource = reinterpret_cast<ID3D11Resource*>(texHandle->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource).pointer);
        tex.srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        tex.srvDesc.Format = nvrhi::d3d11::convertFormat(desc.format);
        tex.srvDesc.Texture2D.MipLevels = 1;
        tex.srvDesc.Texture2D.MostDetailedMip = 0;
        return tex;
    }

    SDK::D3D11::RenderTask::UnorderedAccessTex GetUnorderedAccessTexD3D11(nvrhi::TextureHandle texHandle) {
        auto& desc = texHandle->getDesc();
        assert(desc.dimension == nvrhi::TextureDimension::Texture2D);

        SDK::D3D11::RenderTask::UnorderedAccessTex tex;
        tex.resource = reinterpret_cast<ID3D11Resource*>(texHandle->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource).pointer);
        tex.uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        tex.uavDesc.Format = nvrhi::d3d11::convertFormat(desc.format);
        tex.uavDesc.Texture2D.MipSlice = 0;
        return tex;
    }

    SDK::D3D11::RenderTask::CombinedAccessTex GetCombinedAccessTexD3D11(nvrhi::TextureHandle texHandle) {
        auto& desc = texHandle->getDesc();
        assert(desc.dimension == nvrhi::TextureDimension::Texture2D);

        SDK::D3D11::RenderTask::CombinedAccessTex tex;
        tex.resource = reinterpret_cast<ID3D11Resource*>(texHandle->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource).pointer);
        tex.srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        tex.srvDesc.Format = nvrhi::d3d11::convertFormat(desc.format);
        tex.srvDesc.Texture2D.MipLevels = 1;
        tex.srvDesc.Texture2D.MostDetailedMip = 0;

        tex.uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        tex.uavDesc.Format = nvrhi::d3d11::convertFormat(desc.format);
        tex.uavDesc.Texture2D.MipSlice = 0;
        return tex;
    }

    uint32_t SetupLightInfosD3D11(SDK::D3D11::RenderTask::LightInfo* lightInfos, uint32_t maxLightNum) {
        return _SetupLightInfos<SDK::D3D11::RenderTask::LightInfo>(lightInfos, maxLightNum);
    }
#endif
#ifdef KickstartRT_Demo_WITH_D3D12

    SDK::D3D12::RenderTask::ShaderResourceTex GetShaderResourceTexD3D12(nvrhi::TextureHandle texHandle) {
        auto& desc = texHandle->getDesc();
        assert(desc.dimension == nvrhi::TextureDimension::Texture2D);

        SDK::D3D12::RenderTask::ShaderResourceTex tex;
        tex.resource = reinterpret_cast<ID3D12Resource*>(texHandle->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource).pointer);
        tex.srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        tex.srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tex.srvDesc.Format = nvrhi::d3d12::convertFormat(desc.format);
        tex.srvDesc.Texture2D.MipLevels = 1;
        tex.srvDesc.Texture2D.MostDetailedMip = 0;
        return tex;
    }

    SDK::D3D12::RenderTask::UnorderedAccessTex GetUnorderedAccessTexD3D12(nvrhi::TextureHandle texHandle) {
        auto& desc = texHandle->getDesc();
        assert(desc.dimension == nvrhi::TextureDimension::Texture2D);

        SDK::D3D12::RenderTask::UnorderedAccessTex tex;
        tex.resource = reinterpret_cast<ID3D12Resource*>(texHandle->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource).pointer);
        tex.uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        tex.uavDesc.Format = nvrhi::d3d12::convertFormat(desc.format);
        tex.uavDesc.Texture2D.MipSlice = 0;
        return tex;
    }

    SDK::D3D12::RenderTask::CombinedAccessTex GetCombinedAccessTexD3D12(nvrhi::TextureHandle texHandle) {
        auto& desc = texHandle->getDesc();
        assert(desc.dimension == nvrhi::TextureDimension::Texture2D);

        SDK::D3D12::RenderTask::CombinedAccessTex tex;
        tex.resource = reinterpret_cast<ID3D12Resource*>(texHandle->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource).pointer);
        tex.srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        tex.srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tex.srvDesc.Format = nvrhi::d3d12::convertFormat(desc.format);
        tex.srvDesc.Texture2D.MipLevels = 1;
        tex.srvDesc.Texture2D.MostDetailedMip = 0;

        tex.uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        tex.uavDesc.Format = nvrhi::d3d12::convertFormat(desc.format);
        tex.uavDesc.Texture2D.MipSlice = 0;
        return tex;
    }

    uint32_t SetupLightInfosD3D12(SDK::D3D12::RenderTask::LightInfo* lightInfos, uint32_t maxLightNum) {
        return _SetupLightInfos<SDK::D3D12::RenderTask::LightInfo>(lightInfos, maxLightNum);
    }
#endif
#ifdef KickstartRT_Demo_WITH_VK

    SDK::VK::RenderTask::ShaderResourceTex GetShaderResourceTexVK(nvrhi::TextureHandle texHandle) {
        auto& desc = texHandle->getDesc();
        assert(desc.dimension == nvrhi::TextureDimension::Texture2D);

        const nvrhi::FormatInfo& formatInfo = nvrhi::getFormatInfo(desc.format);

        SDK::VK::RenderTask::ShaderResourceTex tex;
        tex.image = reinterpret_cast<VkImage>(texHandle->getNativeObject(nvrhi::ObjectTypes::VK_Image).pointer);
        tex.imageViewType = VK_IMAGE_VIEW_TYPE_2D;
        tex.format = (VkFormat)nvrhi::vulkan::convertFormat(desc.format);
        tex.aspectMask = formatInfo.hasDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        tex.mipCount = 1;
        tex.baseMipLevel = 0;
        tex.baseArrayLayer = 0;
        tex.layerCount = 1;
        return tex;
    }

    SDK::VK::RenderTask::UnorderedAccessTex GetUnorderedAccessTexVK(nvrhi::TextureHandle texHandle) {
        auto& desc = texHandle->getDesc();
        assert(desc.dimension == nvrhi::TextureDimension::Texture2D);

        const nvrhi::FormatInfo& formatInfo = nvrhi::getFormatInfo(desc.format);

        SDK::VK::RenderTask::UnorderedAccessTex tex;
        tex.image = reinterpret_cast<VkImage>(texHandle->getNativeObject(nvrhi::ObjectTypes::VK_Image).pointer);
        tex.imageViewType = VK_IMAGE_VIEW_TYPE_2D;
        tex.format = (VkFormat)nvrhi::vulkan::convertFormat(desc.format);
        tex.aspectMask = formatInfo.hasDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        tex.baseMipLevel = 0;
        tex.baseArrayLayer = 0;
        tex.layerCount = 1;
        return tex;
    }

    SDK::VK::RenderTask::CombinedAccessTex GetCombinedAccessTexVK(nvrhi::TextureHandle texHandle) {
        auto& desc = texHandle->getDesc();
        assert(desc.dimension == nvrhi::TextureDimension::Texture2D);

        const nvrhi::FormatInfo& formatInfo = nvrhi::getFormatInfo(desc.format);

        SDK::VK::RenderTask::CombinedAccessTex tex;
        tex.image = reinterpret_cast<VkImage>(texHandle->getNativeObject(nvrhi::ObjectTypes::VK_Image).pointer);
        tex.imageViewType = VK_IMAGE_VIEW_TYPE_2D;
        tex.format = (VkFormat)nvrhi::vulkan::convertFormat(desc.format);
        tex.aspectMask = formatInfo.hasDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        tex.mipCount = 1;
        tex.baseMipLevel = 0;
        tex.baseArrayLayer = 0;
        tex.layerCount = 1;
        return tex;
    }

    uint32_t SetupLightInfosVK(SDK::VK::RenderTask::LightInfo* lightInfos, uint32_t maxLightNum) {
        return _SetupLightInfos<SDK::VK::RenderTask::LightInfo>(lightInfos, maxLightNum);
    }
#endif
#endif

    void PrepareRenderKS_PreLighting(nvrhi::ICommandList* commandList)
    {
        commandList->beginMarker("KS State Transitions");

        for (uint32_t layerIt = 0; layerIt < Layer::Count; ++layerIt) {
            commandList->setTextureState(m_RenderTargets[layerIt]->HdrColor, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->Depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferDiffuse, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferSpecular, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferNormals, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferEmissive, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->MotionVectors, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferWorldPosition, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);

            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferRTAO, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferRTAOFinal, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferRTShadows, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferRTShadowsAux, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferRTShadowsFinal, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        }

        commandList->commitBarriers();
        commandList->endMarker();
    }

    void PrepareRenderRTReflections(nvrhi::ICommandList* commandList)
    {
        commandList->beginMarker("KS State Transitions");


        for (uint32_t layerIt = 0; layerIt < Layer::Count; ++layerIt) {
            commandList->setTextureState(m_RenderTargets[layerIt]->HdrColor, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->Depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferDiffuse, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferSpecular, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferNormals, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferEmissive, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->MotionVectors, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferWorldPosition, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);

            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferRTReflections, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferRTReflectionsFinal, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferRTGI, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferRTGIFinal, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        }

        commandList->commitBarriers();
        commandList->endMarker();
    }

    void PrepareRenderKS_PostLighting(nvrhi::ICommandList* commandList)
    {
        commandList->beginMarker("KS State Transitions");

        for (uint32_t layerIt = 0; layerIt < Layer::Count; ++layerIt) {
            commandList->setTextureState(m_RenderTargets[layerIt]->HdrColor, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->Depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferDiffuse, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferSpecular, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferNormals, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferEmissive, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->MotionVectors, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferWorldPosition, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);

            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferRTAO, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferRTAOFinal, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferRTShadows, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferRTShadowsAux, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_RenderTargets[layerIt]->GBufferRTShadowsFinal, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        }

        commandList->commitBarriers();
        commandList->endMarker();
    }


    void RenderRTReflections()
    {
#if defined(ENABLE_KickStartSDK)
        SDK::Status sts;

#ifdef KickstartRT_Demo_WITH_D3D11
        auto& tc_pre_11(m_SDKContext.m_tc_preLighting.m_11);
        auto& tc11(m_SDKContext.m_tc.m_11);
        auto& tc_post_11(m_SDKContext.m_tc_postLighting.m_11);
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
        auto& tc_pre_12(m_SDKContext.m_tc_preLighting.m_12);
        auto& tc12(m_SDKContext.m_tc.m_12);
        auto& tc_post_12(m_SDKContext.m_tc_postLighting.m_12);
#endif
#ifdef KickstartRT_Demo_WITH_VK
        auto& tc_pre_VK(m_SDKContext.m_tc_preLighting.m_VK);
        auto& tcVK(m_SDKContext.m_tc.m_VK);
        auto& tc_post_VK(m_SDKContext.m_tc_postLighting.m_VK);
#endif
#ifdef KickstartRT_Demo_WITH_D3D11
        if (m_SDKContext.m_11) {
            if (tc_pre_11 != nullptr || tc11 != nullptr || tc_post_11 != nullptr) {
                assert(false);
            }
			tc_pre_11 = m_SDKContext.m_11->m_executeContext->CreateTaskContainer();
			if (tc_pre_11 == nullptr) {
				log::fatal("Failed to create task container.");
			}
			tc11 = m_SDKContext.m_11->m_executeContext->CreateTaskContainer();
			if (tc11 == nullptr) {
				log::fatal("Failed to create task container.");
			}
			tc_post_11 = m_SDKContext.m_11->m_executeContext->CreateTaskContainer();
			if (tc_post_11 == nullptr) {
				log::fatal("Failed to create task container.");
			}
        }
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
        if (m_SDKContext.m_12) {
            if (tc_pre_12 != nullptr || tc12 != nullptr || tc_post_12 != nullptr) {
                assert(false);
            }
            tc_pre_12 = m_SDKContext.m_12->m_executeContext->CreateTaskContainer();
			if (tc_pre_12 == nullptr) {
				log::fatal("Failed to create task container.");
			}
			tc12 = m_SDKContext.m_12->m_executeContext->CreateTaskContainer();
			if (tc12 == nullptr) {
				log::fatal("Failed to create task container.");
			}
			tc_post_12 = m_SDKContext.m_12->m_executeContext->CreateTaskContainer();
			if (tc_post_12 == nullptr) {
				log::fatal("Failed to create task container.");
			}
        }
#endif
#ifdef KickstartRT_Demo_WITH_VK
        if (m_SDKContext.m_vk) {
            if (tc_pre_VK != nullptr || tcVK != nullptr || tc_post_VK != nullptr) {
                assert(false);
            }
            tc_pre_VK = m_SDKContext.m_vk->m_executeContext->CreateTaskContainer();
            if (tc_pre_VK == nullptr) {
                log::fatal("Failed to create task container.");
            }
            tcVK = m_SDKContext.m_vk->m_executeContext->CreateTaskContainer();
            if (tcVK == nullptr) {
                log::fatal("Failed to create task container.");
            }
            tc_post_VK = m_SDKContext.m_vk->m_executeContext->CreateTaskContainer();
            if (tc_post_VK == nullptr) {
                log::fatal("Failed to create task container.");
            }
        }
#endif

        // gometry and BVH processing.
        {
            // Check skinned mesh instance first to find out which is a skinned geometry.
            static std::set<MeshInfo*>     skinnedMeshSet;
            {
                if (m_ui.KS.m_destructGeom)
                    skinnedMeshSet.clear();

                if (skinnedMeshSet.size() == 0) {
                    auto& skinnedMeshInstances(m_Scene->GetSceneGraph()->GetSkinnedMeshInstances());
                    for (auto&& mi : skinnedMeshInstances) {
                        auto* meshInfo = mi->GetMesh().get();
                        skinnedMeshSet.insert(meshInfo);
                    }
                }
            }

            if (m_ui.KS.m_destructGeom) {
                // destruct all geom once.
                {
#ifdef KickstartRT_Demo_WITH_D3D11
                    std::vector<SDK::D3D11::InstanceHandle> insArr11;
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                    std::vector<SDK::D3D12::InstanceHandle> insArr12;
#endif
#ifdef KickstartRT_Demo_WITH_VK
                    std::vector<SDK::VK::InstanceHandle> insArrVK;
#endif
                    for (auto&& ins : m_SDKContext.m_insHandles) {
#ifdef KickstartRT_Demo_WITH_D3D11
                        insArr11.push_back(ins.second->m_11.m_iTask.handle);
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                        insArr12.push_back(ins.second->m_12.m_iTask.handle);
#endif
#ifdef KickstartRT_Demo_WITH_VK
                        insArrVK.push_back(ins.second->m_VK.m_iTask.handle);
#endif
                    }
#ifdef KickstartRT_Demo_WITH_D3D11
                    if (m_SDKContext.m_11 && insArr11.size() > 0) {
                        sts = m_SDKContext.m_11->m_executeContext->DestroyInstanceHandles(insArr11.data(), (uint32_t)insArr11.size());
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: DestroyInstances() failed. : %d", (uint32_t)sts);
                        }
                    }
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                    if (m_SDKContext.m_12 && insArr12.size() > 0) {
                        sts = m_SDKContext.m_12->m_executeContext->DestroyInstanceHandles(insArr12.data(), (uint32_t)insArr12.size());
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: DestroyInstances() failed. : %d", (uint32_t)sts);
                        }
                    }
#endif
#ifdef KickstartRT_Demo_WITH_VK
                    if (m_SDKContext.m_vk && insArrVK.size() > 0) {
                        sts = m_SDKContext.m_vk->m_executeContext->DestroyInstanceHandles(insArrVK.data(), (uint32_t)insArrVK.size());
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: DestroyInstances() failed. : %d", (uint32_t)sts);
                        }
                    }
#endif
#ifdef KickstartRT_Demo_WITH_D3D11
                    std::vector<SDK::D3D11::GeometryHandle> geoArr11;
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                    std::vector<SDK::D3D12::GeometryHandle> geoArr12;
#endif
#ifdef KickstartRT_Demo_WITH_VK
                    std::vector<SDK::VK::GeometryHandle> geoArrVK;
#endif
                    for (auto&& geo : m_SDKContext.m_geomHandles) {
#ifdef KickstartRT_Demo_WITH_D3D11
                        geoArr11.push_back(geo.second->m_11.m_gTask.handle);
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                        geoArr12.push_back(geo.second->m_12.m_gTask.handle);
#endif
#ifdef KickstartRT_Demo_WITH_VK
                        geoArrVK.push_back(geo.second->m_VK.m_gTask.handle);
#endif
                    }
#ifdef KickstartRT_Demo_WITH_D3D11
                    if (geoArr11.size() > 0 && m_SDKContext.m_11) {
                        sts = m_SDKContext.m_11->m_executeContext->DestroyGeometryHandles(geoArr11.data(), (uint32_t)geoArr11.size());
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: RemoveGeometries() failed. : %d", (uint32_t)sts);
                        }
                    }
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                    if (geoArr12.size() > 0 && m_SDKContext.m_12) {
                        sts = m_SDKContext.m_12->m_executeContext->DestroyGeometryHandles(geoArr12.data(), (uint32_t)geoArr12.size());
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: RemoveGeometries() failed. : %d", (uint32_t)sts);
                        }
                    }
#endif
#ifdef KickstartRT_Demo_WITH_VK
                    if (geoArrVK.size() > 0 && m_SDKContext.m_vk) {
                        sts = m_SDKContext.m_vk->m_executeContext->DestroyGeometryHandles(geoArrVK.data(), (uint32_t)geoArrVK.size());
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: RemoveGeometries() failed. : %d", (uint32_t)sts);
                        }
                    }
#endif
                }

                m_SDKContext.m_insHandles.clear();
                m_SDKContext.m_geomHandles.clear();

                m_ui.KS.m_destructGeom = false;
            }

            auto ShouldIncludeMeshGeometry = [](const MeshGeometry& mesh)
            {
                // This is meant to filter out transmissive objects, as they would normally not be registered in the RT BVH
                return mesh.material->domain == MaterialDomain::Opaque || mesh.material->domain == MaterialDomain::AlphaTested;
            };

            {
                auto& meshes(m_Scene->GetSceneGraph()->GetMeshes());

                for (auto&& itr : meshes) {
                    MeshInfo* ptr = itr.get();

                    bool isSkinnedMesh = false;
                    if (skinnedMeshSet.find(ptr) != skinnedMeshSet.end()) {
                        isSkinnedMesh = true;
                    }

                    auto ghItr = m_SDKContext.m_geomHandles.find(ptr);
                    if (ghItr != m_SDKContext.m_geomHandles.end()) {
                        // already registerd.
                        continue;
                    }

#ifdef KickstartRT_Demo_WITH_D3D11
                    SDK::D3D11::BVHTask::GeometryInput input11;

                    ID3D11Buffer* indexBuf11 = reinterpret_cast<ID3D11Buffer*>(ptr->buffers->indexBuffer->getNativeObject(nvrhi::ObjectTypes::D3D11_Buffer).pointer);
                    ID3D11Buffer* vertexBuf11 = reinterpret_cast<ID3D11Buffer*>(ptr->buffers->vertexBuffer->getNativeObject(nvrhi::ObjectTypes::D3D11_Buffer).pointer);
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                    SDK::D3D12::BVHTask::GeometryInput input12;

                    ID3D12Resource* indexBuf12 = reinterpret_cast<ID3D12Resource*>(ptr->buffers->indexBuffer->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource).pointer);
                    ID3D12Resource* vertexBuf12 = reinterpret_cast<ID3D12Resource*>(ptr->buffers->vertexBuffer->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource).pointer);
#endif
#ifdef KickstartRT_Demo_WITH_VK
                    SDK::VK::BVHTask::GeometryInput inputVK;

                    VkBuffer indexBufVK = reinterpret_cast<VkBuffer>(ptr->buffers->indexBuffer->getNativeObject(nvrhi::ObjectTypes::VK_Buffer).pointer);
                    VkBuffer vertexBufVK = reinterpret_cast<VkBuffer>(ptr->buffers->vertexBuffer->getNativeObject(nvrhi::ObjectTypes::VK_Buffer).pointer);
#endif
                    const auto& vrange = ptr->buffers->getVertexBufferRange(VertexAttribute::Position);

#ifdef KickstartRT_Demo_WITH_D3D11
                    if (tc_pre_11 != nullptr) {
                        input11.allowUpdate = isSkinnedMesh;
                        input11.type = decltype(input11)::Type::TrianglesIndexed;
                        input11.surfelType = (decltype(input11)::SurfelType)m_ui.KS.m_surfelMode;
                        input11.allowLightTransferTarget = true;

                        input11.forceDirectTileMapping = m_ui.KS.m_forceDirectTileMapping;
                        input11.tileUnitLength = m_ui.KS.m_tileUnitLength;
                        input11.tileResolutionLimit = m_ui.KS.m_tileResolutionLimit;
                    }
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                    if (tc_pre_12 != nullptr) {
                        input12.allowUpdate = isSkinnedMesh;
                        input12.type = decltype(input12)::Type::TrianglesIndexed;
                        input12.surfelType = (decltype(input12)::SurfelType)m_ui.KS.m_surfelMode;
                        input12.allowLightTransferTarget = true;

                        input12.forceDirectTileMapping = m_ui.KS.m_forceDirectTileMapping;
                        input12.tileUnitLength = m_ui.KS.m_tileUnitLength;
                        input12.tileResolutionLimit = m_ui.KS.m_tileResolutionLimit;
                    }
#endif
#ifdef KickstartRT_Demo_WITH_VK
                    if (tc_pre_VK != nullptr) {
                        inputVK.allowUpdate = isSkinnedMesh;
                        inputVK.type = decltype(inputVK)::Type::TrianglesIndexed;
                        inputVK.surfelType = (decltype(inputVK)::SurfelType)m_ui.KS.m_surfelMode;
                        inputVK.allowLightTransferTarget = true;

                        inputVK.forceDirectTileMapping = m_ui.KS.m_forceDirectTileMapping;
                        inputVK.tileUnitLength = m_ui.KS.m_tileUnitLength;
                        inputVK.tileResolutionLimit = m_ui.KS.m_tileResolutionLimit;
                    }
#endif

                    for (auto&& geom : ptr->geometries) {
                        MeshGeometry* gPtr = geom.get();

                        if (!ShouldIncludeMeshGeometry(*gPtr))
                            continue;

                        size_t numIdcs = gPtr->numIndices;
                        size_t startVertexLocation = (size_t)ptr->vertexOffset + gPtr->vertexOffsetInMesh;
                        size_t startIndexLocation = (size_t)ptr->indexOffset + gPtr->indexOffsetInMesh;

#ifdef KickstartRT_Demo_WITH_D3D11
						if (tc_pre_11 != nullptr) {
                            decltype(input11)::GeometryComponent cmp;

                            cmp.indexBuffer.resource = indexBuf11;
                            cmp.indexBuffer.format = DXGI_FORMAT_R32_UINT;
                            cmp.indexBuffer.offsetInBytes = startIndexLocation * sizeof(uint32_t);
                            cmp.indexBuffer.count = (uint32_t)numIdcs;

                            cmp.vertexBuffer.resource = vertexBuf11;
                            cmp.vertexBuffer.format = DXGI_FORMAT_R32G32B32_FLOAT;
                            cmp.vertexBuffer.offsetInBytes = vrange.byteOffset + startVertexLocation * sizeof(float) * 3;
                            cmp.vertexBuffer.strideInBytes = sizeof(float) * 3;
                            cmp.vertexBuffer.count = gPtr->numVertices;

                            cmp.useTransform = false;

                            input11.components.push_back(cmp);
						}
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
						if (tc_pre_12 != nullptr) {
                            decltype(input12)::GeometryComponent cmp;

							cmp.indexBuffer.resource = indexBuf12;
                            cmp.indexBuffer.format = DXGI_FORMAT_R32_UINT;
                            cmp.indexBuffer.offsetInBytes = startIndexLocation * sizeof(uint32_t);
                            cmp.indexBuffer.count = (uint32_t)numIdcs;

                            cmp.vertexBuffer.resource = vertexBuf12;
                            cmp.vertexBuffer.format = DXGI_FORMAT_R32G32B32_FLOAT;
                            cmp.vertexBuffer.offsetInBytes = vrange.byteOffset + startVertexLocation * sizeof(float) * 3;
                            cmp.vertexBuffer.strideInBytes = sizeof(float) * 3;
                            cmp.vertexBuffer.count = gPtr->numVertices;

                            cmp.useTransform = false;

                            input12.components.push_back(cmp);
                        }
#endif
#ifdef KickstartRT_Demo_WITH_VK
						if (tc_pre_VK != nullptr) {
                            decltype(inputVK)::GeometryComponent cmp;

							cmp.indexBuffer.typedBuffer = indexBufVK;
                            cmp.indexBuffer.format = VK_FORMAT_R32_UINT;
                            cmp.indexBuffer.offsetInBytes = startIndexLocation * sizeof(uint32_t);
                            cmp.indexBuffer.count = (uint32_t)numIdcs;

                            cmp.vertexBuffer.typedBuffer = vertexBufVK;
                            cmp.vertexBuffer.format = VK_FORMAT_R32G32B32_SFLOAT;
                            cmp.vertexBuffer.offsetInBytes = vrange.byteOffset + startVertexLocation * sizeof(float) * 3;
                            cmp.vertexBuffer.strideInBytes = sizeof(float) * 3;
                            cmp.vertexBuffer.count = gPtr->numVertices;

                            cmp.useTransform = false;

                            inputVK.components.push_back(cmp);
						}
#endif
                    }

                    // register geom task.
#ifdef KickstartRT_Demo_WITH_D3D11
                    if (tc_pre_11 != nullptr) {
                        if (input11.components.size() > 0) {
                            KickstartRT_SDK_Context::GeomHandle gh = std::make_unique<KickstartRT_SDK_Context::GeomHandleType>();
                            gh->m_11.m_gTask.taskOperation = SDK::D3D11::BVHTask::TaskOperation::Register;
                            gh->m_11.m_gTask.handle = m_SDKContext.m_11->m_executeContext->CreateGeometryHandle();
                            gh->m_11.m_gTask.input = input11;

                            sts = tc_pre_11->ScheduleBVHTask(&gh->m_11.m_gTask);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleBVHTasks() failed. : %d", (uint32_t)sts);
                            }

                            m_SDKContext.m_geomHandles.insert({ ptr, std::move(gh) });
                        }
                    }
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                    if (tc_pre_12 != nullptr) {
                        if (input12.components.size() > 0) {
                            KickstartRT_SDK_Context::GeomHandle gh = std::make_unique<KickstartRT_SDK_Context::GeomHandleType>();
                            gh->m_12.m_gTask.taskOperation = SDK::D3D12::BVHTask::TaskOperation::Register;
                            gh->m_12.m_gTask.handle = m_SDKContext.m_12->m_executeContext->CreateGeometryHandle();
                            gh->m_12.m_gTask.input = input12;

                            sts = tc_pre_12->ScheduleBVHTask(&gh->m_12.m_gTask);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleBVHTasks() failed. : %d", (uint32_t)sts);
                            }

                            m_SDKContext.m_geomHandles.insert({ ptr, std::move(gh) });
                        }
                    }
#endif
#ifdef KickstartRT_Demo_WITH_VK
                    if (tc_pre_VK != nullptr) {
                        if (inputVK.components.size() > 0) {
                            KickstartRT_SDK_Context::GeomHandle gh = std::make_unique<KickstartRT_SDK_Context::GeomHandleType>();
                            gh->m_VK.m_gTask.taskOperation = SDK::VK::BVHTask::TaskOperation::Register;
                            gh->m_VK.m_gTask.handle = m_SDKContext.m_vk->m_executeContext->CreateGeometryHandle();
                            gh->m_VK.m_gTask.input = inputVK;

                            sts = tc_pre_VK->ScheduleBVHTask(&gh->m_VK.m_gTask);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleBVHTasks() failed. : %d", (uint32_t)sts);
                            }

                            m_SDKContext.m_geomHandles.insert({ ptr, std::move(gh) });
                        }
                    }
#endif
                }
            }
            {
                auto& instances(m_Scene->GetSceneGraph()->GetMeshInstances());

                std::vector<KickstartRT_SDK_Context::InstanceHandle> addedIns;

                for (auto&& itr : instances) {
                    MeshInstance* ptr = itr.get();

                    auto ihItr = m_SDKContext.m_insHandles.find(ptr);
                    if (ihItr != m_SDKContext.m_insHandles.end()) {
                        // already registered.
                        continue;
                    }

                    MeshInfo* meshPtr = ptr->GetMesh().get();
                    auto ghItr = m_SDKContext.m_geomHandles.find(meshPtr);
                    if (ghItr == m_SDKContext.m_geomHandles.end()) {
                        log::fatal("KickStartRTX: Failed to find geometry handle when registering an instance.");
                    }

                    KickstartRT_SDK_Context::InstanceHandle ih = std::make_unique<KickstartRT_SDK_Context::InstanceHandleType>();

                    SceneGraphNode* node = ptr->GetNode();

                    if (m_SDKContext.m_insStates.find(ptr) == m_SDKContext.m_insStates.end())
                    {
                        m_SDKContext.m_insStates.insert({ ptr, {} });
                    }

                    auto itState = m_SDKContext.m_insStates.find(ptr);
                    assert(itState != m_SDKContext.m_insStates.end());

#ifdef KickstartRT_Demo_WITH_D3D11
					if (tc_pre_11 != nullptr) {
                        ih->m_11.m_iTask.handle = m_SDKContext.m_11->m_executeContext->CreateInstanceHandle();
                        ih->m_11.m_iTask.taskOperation = SDK::D3D11::BVHTask::TaskOperation::Register;

                        auto& input(ih->m_11.m_iTask.input);
                        input.geomHandle = ghItr->second->m_11.m_gTask.handle;
                        ih->m_geomHandle = ghItr->second.get();
                        {
                            SDK::Math::Float_4x4 mWrk;
                            math::affineToColumnMajor(ptr->GetNode()->GetLocalToWorldTransformFloat(), mWrk.f);
                            mWrk = mWrk.Transpose();
                            input.transform.CopyFrom4x4(mWrk.f);

                            SDK::D3D11::BVHTask::InstanceInclusionMask instanceInclusionMask = (SDK::D3D11::BVHTask::InstanceInclusionMask)0;
                            if (itState->second.instanceProp_DirectLightInjectionTarget)
                                instanceInclusionMask = (SDK::D3D11::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::D3D11::BVHTask::InstanceInclusionMask::DirectLightInjectionTarget);
                            if (itState->second.instanceProp_LightTransferSource)
                                instanceInclusionMask = (SDK::D3D11::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::D3D11::BVHTask::InstanceInclusionMask::LightTransferSource);
                            if (itState->second.instanceProp_VisibleInRT)
                                instanceInclusionMask = (SDK::D3D11::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::D3D11::BVHTask::InstanceInclusionMask::VisibleInRT);

                            input.instanceInclusionMask = instanceInclusionMask;
                        }
					}
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
					if (tc_pre_12 != nullptr) {
						ih->m_12.m_iTask.handle = m_SDKContext.m_12->m_executeContext->CreateInstanceHandle();
						ih->m_12.m_iTask.taskOperation = SDK::D3D12::BVHTask::TaskOperation::Register;

						auto& input(ih->m_12.m_iTask.input);
						input.geomHandle = ghItr->second->m_12.m_gTask.handle;
                        ih->m_geomHandle = ghItr->second.get();
                        {
							SDK::Math::Float_4x4 mWrk;
							math::affineToColumnMajor(ptr->GetNode()->GetLocalToWorldTransformFloat(), mWrk.f);
							mWrk = mWrk.Transpose();
							input.transform.CopyFrom4x4(mWrk.f);

                            SDK::D3D12::BVHTask::InstanceInclusionMask instanceInclusionMask = (SDK::D3D12::BVHTask::InstanceInclusionMask)0;
                            if (itState->second.instanceProp_DirectLightInjectionTarget)
                                instanceInclusionMask = (SDK::D3D12::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::D3D12::BVHTask::InstanceInclusionMask::DirectLightInjectionTarget);
                            if (itState->second.instanceProp_LightTransferSource)
                                instanceInclusionMask = (SDK::D3D12::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::D3D12::BVHTask::InstanceInclusionMask::LightTransferSource);
                            if (itState->second.instanceProp_VisibleInRT)
                                instanceInclusionMask = (SDK::D3D12::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::D3D12::BVHTask::InstanceInclusionMask::VisibleInRT);

                            input.instanceInclusionMask = instanceInclusionMask;
						}
					}
#endif
#ifdef KickstartRT_Demo_WITH_VK
                    if (tc_pre_VK != nullptr) {
                        ih->m_VK.m_iTask.handle = m_SDKContext.m_vk->m_executeContext->CreateInstanceHandle();
                        ih->m_VK.m_iTask.taskOperation = SDK::VK::BVHTask::TaskOperation::Register;

                        auto& input(ih->m_VK.m_iTask.input);
                        input.geomHandle = ghItr->second->m_VK.m_gTask.handle;
                        ih->m_geomHandle = ghItr->second.get();
                        {
                            SDK::Math::Float_4x4 mWrk;
                            math::affineToColumnMajor(ptr->GetNode()->GetLocalToWorldTransformFloat(), mWrk.f);
                            mWrk = mWrk.Transpose();
                            input.transform.CopyFrom4x4(mWrk.f);

                            SDK::VK::BVHTask::InstanceInclusionMask instanceInclusionMask = (SDK::VK::BVHTask::InstanceInclusionMask)0;
                            if (itState->second.instanceProp_DirectLightInjectionTarget)
                                instanceInclusionMask = (SDK::VK::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::VK::BVHTask::InstanceInclusionMask::DirectLightInjectionTarget);
                            if (itState->second.instanceProp_LightTransferSource)
                                instanceInclusionMask = (SDK::VK::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::VK::BVHTask::InstanceInclusionMask::LightTransferSource);
                            if (itState->second.instanceProp_VisibleInRT)
                                instanceInclusionMask = (SDK::VK::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::VK::BVHTask::InstanceInclusionMask::VisibleInRT);

                            input.instanceInclusionMask = instanceInclusionMask;
                        }
                    }
#endif
                    ih->m_insPtr = itr;
                    addedIns.push_back(std::move(ih));
                }

                if (addedIns.size() > 0) {
#ifdef KickstartRT_Demo_WITH_D3D11
                    if (tc_pre_11 != nullptr) {
                        std::vector<SDK::D3D11::BVHTask::Task*> taskArr;
						for (auto&& ai : addedIns) {
							taskArr.push_back(&ai->m_11.m_iTask);
                        }
                        sts = tc_pre_11->ScheduleBVHTasks(taskArr.data(), (uint32_t)taskArr.size());
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: ScheduleBVHTasks() failed. : %d", (uint32_t)sts);
                        }
                    }
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                    if (tc_pre_12 != nullptr) {
                        std::vector<SDK::D3D12::BVHTask::Task*> taskArr;
                        for (auto&& ai : addedIns) {
                            taskArr.push_back(&ai->m_12.m_iTask);
                        }
                        sts = tc_pre_12->ScheduleBVHTasks(taskArr.data(), (uint32_t)taskArr.size());
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: ScheduleBVHTasks() failed. : %d", (uint32_t)sts);
                        }
                    }
#endif
#ifdef KickstartRT_Demo_WITH_VK
                    if (tc_pre_VK != nullptr) {
                        std::vector<SDK::VK::BVHTask::Task*> taskArr;
                        for (auto&& ai : addedIns) {
                            taskArr.push_back(&ai->m_VK.m_iTask);
                        }
                        sts = tc_pre_VK->ScheduleBVHTasks(taskArr.data(), (uint32_t)taskArr.size());
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: ScheduleBVHTasks() failed. : %d", (uint32_t)sts);
                        }
                    }
#endif
                    for (auto&& ai : addedIns) {
                        MeshInstance* iPtr = ai->m_insPtr.get();
                        m_SDKContext.m_insHandles.insert({ iPtr, std::move(ai) });
                        assert(m_SDKContext.m_insStates.find(iPtr) != m_SDKContext.m_insStates.end());
                    }
                }

                {
#ifdef KickstartRT_Demo_WITH_D3D11
                    std::vector<SDK::D3D11::BVHTask::Task*>  bvhTaskPtr11;
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                    std::vector<SDK::D3D12::BVHTask::Task*>  bvhTaskPtr12;
#endif
#ifdef KickstartRT_Demo_WITH_VK
                    std::vector<SDK::VK::BVHTask::Task*>  bvhTaskPtrVK;
#endif

                    for (auto&& itr : m_SDKContext.m_insHandles)
                    {
                        auto& SDKIns(itr.second);
                        auto& meshInstance(SDKIns->m_insPtr);
                        SceneGraphNode* node = meshInstance->GetNode();

                        auto itState = m_SDKContext.m_insStates.find(itr.first);
                        assert(itState != m_SDKContext.m_insStates.end());

                        if (itState->second.isDirty || node->GetDirtyFlags() != SceneGraphNode::DirtyFlags::None) {

                            itState->second.isDirty = false;

                            SDK::Math::Float_4x4 mWrk;
                            math::affineToColumnMajor(node->GetLocalToWorldTransformFloat(), mWrk.f);
                            mWrk = mWrk.Transpose();
#ifdef KickstartRT_Demo_WITH_D3D11
                            {
                                auto& it(SDKIns->m_11.m_iTask);
                                it.taskOperation = SDK::D3D11::BVHTask::TaskOperation::Update;
                                it.input.transform.CopyFrom4x4(mWrk.f);

                                SDK::D3D11::BVHTask::InstanceInclusionMask instanceInclusionMask = (SDK::D3D11::BVHTask::InstanceInclusionMask)0;
                                if (itState->second.instanceProp_DirectLightInjectionTarget)
                                    instanceInclusionMask = (SDK::D3D11::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::D3D11::BVHTask::InstanceInclusionMask::DirectLightInjectionTarget);
                                if (itState->second.instanceProp_LightTransferSource)
                                    instanceInclusionMask = (SDK::D3D11::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::D3D11::BVHTask::InstanceInclusionMask::LightTransferSource);
                                if (itState->second.instanceProp_VisibleInRT)
                                    instanceInclusionMask = (SDK::D3D11::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::D3D11::BVHTask::InstanceInclusionMask::VisibleInRT);

                                it.input.instanceInclusionMask = instanceInclusionMask;

                                bvhTaskPtr11.push_back(&it);
                            }
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                            {
                                auto& it(SDKIns->m_12.m_iTask);
                                it.taskOperation = SDK::D3D12::BVHTask::TaskOperation::Update;
                                it.input.transform.CopyFrom4x4(mWrk.f);
                                SDK::D3D12::BVHTask::InstanceInclusionMask instanceInclusionMask = (SDK::D3D12::BVHTask::InstanceInclusionMask)0;
                                if (itState->second.instanceProp_DirectLightInjectionTarget)
                                    instanceInclusionMask = (SDK::D3D12::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::D3D12::BVHTask::InstanceInclusionMask::DirectLightInjectionTarget);
                                if (itState->second.instanceProp_LightTransferSource)
                                    instanceInclusionMask = (SDK::D3D12::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::D3D12::BVHTask::InstanceInclusionMask::LightTransferSource);
                                if (itState->second.instanceProp_VisibleInRT)
                                    instanceInclusionMask = (SDK::D3D12::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::D3D12::BVHTask::InstanceInclusionMask::VisibleInRT);

                                it.input.instanceInclusionMask = instanceInclusionMask;

                                bvhTaskPtr12.push_back(&it);
                            }
#endif
#ifdef KickstartRT_Demo_WITH_VK
                            {
                                auto& it(SDKIns->m_VK.m_iTask);
                                it.taskOperation = SDK::VK::BVHTask::TaskOperation::Update;
                                it.input.transform.CopyFrom4x4(mWrk.f);

                                SDK::VK::BVHTask::InstanceInclusionMask instanceInclusionMask = (SDK::VK::BVHTask::InstanceInclusionMask)0;
                                if (itState->second.instanceProp_DirectLightInjectionTarget)
                                    instanceInclusionMask = (SDK::VK::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::VK::BVHTask::InstanceInclusionMask::DirectLightInjectionTarget);
                                if (itState->second.instanceProp_LightTransferSource)
                                    instanceInclusionMask = (SDK::VK::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::VK::BVHTask::InstanceInclusionMask::LightTransferSource);
                                if (itState->second.instanceProp_VisibleInRT)
                                    instanceInclusionMask = (SDK::VK::BVHTask::InstanceInclusionMask)((uint32_t)instanceInclusionMask | (uint32_t)SDK::VK::BVHTask::InstanceInclusionMask::VisibleInRT);

                                it.input.instanceInclusionMask = instanceInclusionMask;

                                bvhTaskPtrVK.push_back(&it);
                            }
#endif
                        }

                        {
                            auto skinnedInstance = std::dynamic_pointer_cast<SkinnedMeshInstance>(meshInstance);
                            if (skinnedInstance)
                            {
                                auto* gh = SDKIns->m_geomHandle;

#ifdef KickstartRT_Demo_WITH_D3D11
                                gh->m_11.m_gTask.taskOperation = SDK::D3D11::BVHTask::TaskOperation::Update;
                                bvhTaskPtr11.push_back(&gh->m_11.m_gTask);
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                                gh->m_12.m_gTask.taskOperation = SDK::D3D12::BVHTask::TaskOperation::Update;
                                bvhTaskPtr12.push_back(&gh->m_12.m_gTask);
#endif
#ifdef KickstartRT_Demo_WITH_VK
                                gh->m_VK.m_gTask.taskOperation = SDK::VK::BVHTask::TaskOperation::Update;
                                bvhTaskPtrVK.push_back(&gh->m_VK.m_gTask);
#endif
                            }
                        }

                    }

#ifdef KickstartRT_Demo_WITH_D3D11
                    if (tc_pre_11 != nullptr) {
                        sts = tc_pre_11->ScheduleBVHTasks(bvhTaskPtr11.data(), (uint32_t)bvhTaskPtr11.size());
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickstartRT: ScheduleBVHTasks for update failed. : %d", (uint32_t)sts);
                        }
                    }
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                    if (tc_pre_12 != nullptr) {
                        sts = tc_pre_12->ScheduleBVHTasks(bvhTaskPtr12.data(), (uint32_t)bvhTaskPtr12.size());
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickstartRT: ScheduleBVHTasks for update failed. : %d", (uint32_t)sts);
                        }
                    }
#endif
#ifdef KickstartRT_Demo_WITH_VK
                    if (tc_pre_VK != nullptr) {
                        sts = tc_pre_VK->ScheduleBVHTasks(bvhTaskPtrVK.data(), (uint32_t)bvhTaskPtrVK.size());
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickstartRT: ScheduleBVHTasks for update failed. : %d", (uint32_t)sts);
                        }
                    }
#endif
                }
            }

            {
#ifdef KickstartRT_Demo_WITH_D3D11
                if (tc_pre_11 != nullptr) {
                    SDK::D3D11::BVHTask::BVHBuildTask bvhTask;
                    sts = tc_pre_11->ScheduleBVHTask(&bvhTask);
                    if (sts != SDK::Status::OK) {
                        log::fatal("KickStartRTX: ScheduleBVHTasks() failed. : %d", (uint32_t)sts);
                    }
                }
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                if (tc_pre_12 != nullptr) {
                    SDK::D3D12::BVHTask::BVHBuildTask bvhTask;
                    sts = tc_pre_12->ScheduleBVHTask(&bvhTask);
                    if (sts != SDK::Status::OK) {
                        log::fatal("KickStartRTX: ScheduleBVHTasks() failed. : %d", (uint32_t)sts);
                    }
                }
#endif
#ifdef KickstartRT_Demo_WITH_VK
                if (tc_pre_VK != nullptr) {
                    SDK::VK::BVHTask::BVHBuildTask bvhTask;
                    sts = tc_pre_VK->ScheduleBVHTask(&bvhTask);
                    if (sts != SDK::Status::OK) {
                        log::fatal("KickStartRTX: ScheduleBVHTasks() failed. : %d", (uint32_t)sts);
                    }
                }
#endif
            }
        }

        // Prepare denoising contexts.
        const bool enableReflectionDenoising = m_ui.KS.m_denoisingMethod != 0 && (m_ui.KS.m_enableReflection || m_ui.KS.m_enableGI);
        const bool enableAODenoising = m_ui.KS.m_aoDenoisingMethod != 0 && m_ui.KS.m_enableAO;
        const bool enableShadowDenoising = m_ui.KS.m_shadowDenoisingMethod != 0 && m_ui.KS.m_enableShadows;
        const bool enableDenoising = enableReflectionDenoising || enableAODenoising || enableShadowDenoising;
        const bool enableCheckerboard = m_ui.KS.m_enableCheckerboard && m_ui.KS.m_debugDisp == 0;

        nvrhi::TextureHandle GBufferRTReflections[Layer::Count] = { 0, };
        GBufferRTReflections[Layer::Opaque] = enableReflectionDenoising ? m_RenderTargets[Layer::Opaque]->GBufferRTReflections : m_RenderTargets[Layer::Opaque]->GBufferRTReflectionsFinal;
        nvrhi::TextureHandle GBufferRTGI[Layer::Count] = { 0, };
        GBufferRTGI[Layer::Opaque] = enableReflectionDenoising ? m_RenderTargets[Layer::Opaque]->GBufferRTGI : m_RenderTargets[Layer::Opaque]->GBufferRTGIFinal;
        nvrhi::TextureHandle GBufferRTAO[Layer::Count] = { 0, };
        GBufferRTAO[Layer::Opaque] = enableAODenoising ? m_RenderTargets[Layer::Opaque]->GBufferRTAO : m_RenderTargets[Layer::Opaque]->GBufferRTAOFinal;
        nvrhi::TextureHandle GBufferRTShadows[Layer::Count] = { 0, };
        GBufferRTShadows[Layer::Opaque] = enableShadowDenoising ? m_RenderTargets[Layer::Opaque]->GBufferRTShadows : m_RenderTargets[Layer::Opaque]->GBufferRTShadowsFinal;

        {

            // Spin up denoising contexts. (For simplicitly we keep a single hash around for all of them.)
            {
                uint64_t hash = 0;
                if (enableDenoising) {
                    nvrhi::hash_combine(hash, m_RenderTargets[Layer::Opaque]->GBufferRTReflections.Get());
                    nvrhi::hash_combine(hash, m_RenderTargets[Layer::Opaque]->GBufferRTGI.Get());
                    nvrhi::hash_combine(hash, m_RenderTargets[Layer::Opaque]->GBufferRTAO.Get());
                    nvrhi::hash_combine(hash, m_ui.KS.m_enableReflection);
                    nvrhi::hash_combine(hash, m_ui.KS.m_enableGI);
                    nvrhi::hash_combine(hash, m_ui.KS.m_enableAO);
                    nvrhi::hash_combine(hash, m_ui.KS.m_enableShadows);
                    nvrhi::hash_combine(hash, m_ui.KS.m_denoisingMethod);
                    nvrhi::hash_combine(hash, m_ui.KS.m_aoDenoisingMethod);
                    nvrhi::hash_combine(hash, m_ui.KS.m_shadowDenoisingMethod);
                }
                if (m_SDKContext.m_denosingContext.hash != hash && m_SDKContext.m_denosingContext.hash != 0) {
                    // Destruct the current denoising context handles.

                    m_SDKContext.m_denosingContext.hash = 0;

#ifdef KickstartRT_Demo_WITH_D3D11
                    if (m_SDKContext.m_11) {
                        if (m_SDKContext.m_denosingContext.specDiff.m_11 != SDK::D3D11::DenoisingContextHandle::Null) {
                            sts = m_SDKContext.m_11->m_executeContext->DestroyDenoisingContextHandle(m_SDKContext.m_denosingContext.specDiff.m_11);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: DestroyDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }
                        if (m_SDKContext.m_denosingContext.ao.m_11 != SDK::D3D11::DenoisingContextHandle::Null) {
                            sts = m_SDKContext.m_11->m_executeContext->DestroyDenoisingContextHandle(m_SDKContext.m_denosingContext.ao.m_11);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: DestroyDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }
                        if (m_SDKContext.m_denosingContext.shadow.m_11 != SDK::D3D11::DenoisingContextHandle::Null) {
                            sts = m_SDKContext.m_11->m_executeContext->DestroyDenoisingContextHandle(m_SDKContext.m_denosingContext.shadow.m_11);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: DestroyDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }
                        m_SDKContext.m_denosingContext.specDiff.m_11 = SDK::D3D11::DenoisingContextHandle::Null;
                        m_SDKContext.m_denosingContext.ao.m_11 = SDK::D3D11::DenoisingContextHandle::Null;
                        m_SDKContext.m_denosingContext.shadow.m_11 = SDK::D3D11::DenoisingContextHandle::Null;
                    }
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                    if (m_SDKContext.m_12) {
                        if (m_SDKContext.m_denosingContext.specDiff.m_12 != SDK::D3D12::DenoisingContextHandle::Null) {
                            sts = m_SDKContext.m_12->m_executeContext->DestroyDenoisingContextHandle(m_SDKContext.m_denosingContext.specDiff.m_12);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: DestroyDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }
                        if (m_SDKContext.m_denosingContext.ao.m_12 != SDK::D3D12::DenoisingContextHandle::Null) {
                            sts = m_SDKContext.m_12->m_executeContext->DestroyDenoisingContextHandle(m_SDKContext.m_denosingContext.ao.m_12);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: DestroyDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }
                        if (m_SDKContext.m_denosingContext.shadow.m_12 != SDK::D3D12::DenoisingContextHandle::Null) {
                            sts = m_SDKContext.m_12->m_executeContext->DestroyDenoisingContextHandle(m_SDKContext.m_denosingContext.shadow.m_12);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: DestroyDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }
                        m_SDKContext.m_denosingContext.specDiff.m_12 = SDK::D3D12::DenoisingContextHandle::Null;
                        m_SDKContext.m_denosingContext.ao.m_12 = SDK::D3D12::DenoisingContextHandle::Null;
                        m_SDKContext.m_denosingContext.shadow.m_12 = SDK::D3D12::DenoisingContextHandle::Null;
                    }
#endif
#ifdef KickstartRT_Demo_WITH_VK
                    if (m_SDKContext.m_vk) {
                        if (m_SDKContext.m_denosingContext.specDiff.m_VK != SDK::VK::DenoisingContextHandle::Null) {
                            sts = m_SDKContext.m_vk->m_executeContext->DestroyDenoisingContextHandle(m_SDKContext.m_denosingContext.specDiff.m_VK);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: DestroyDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }
                        if (m_SDKContext.m_denosingContext.ao.m_VK != SDK::VK::DenoisingContextHandle::Null) {
                            sts = m_SDKContext.m_vk->m_executeContext->DestroyDenoisingContextHandle(m_SDKContext.m_denosingContext.ao.m_VK);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: DestroyDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }
                        if (m_SDKContext.m_denosingContext.shadow.m_VK != SDK::VK::DenoisingContextHandle::Null) {
                            sts = m_SDKContext.m_vk->m_executeContext->DestroyDenoisingContextHandle(m_SDKContext.m_denosingContext.shadow.m_VK);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: DestroyDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }
                        m_SDKContext.m_denosingContext.specDiff.m_VK = SDK::VK::DenoisingContextHandle::Null;
                        m_SDKContext.m_denosingContext.ao.m_VK = SDK::VK::DenoisingContextHandle::Null;
                        m_SDKContext.m_denosingContext.shadow.m_VK = SDK::VK::DenoisingContextHandle::Null;
                    }
#endif                
                }

                if (m_SDKContext.m_denosingContext.hash != hash) {

                    m_SDKContext.m_denosingContext.hash = hash;

#ifdef KickstartRT_Demo_WITH_D3D11
                    if (m_SDKContext.m_11) {
                        if (enableReflectionDenoising) {
                            SDK::D3D11::DenoisingContextInput context;
                            context.maxWidth = m_RenderTargets[Layer::Opaque]->GBufferRTReflections->getDesc().width;
                            context.maxHeight = m_RenderTargets[Layer::Opaque]->GBufferRTReflections->getDesc().height;
                            assert(m_ui.KS.m_denoisingMethod == 1 || m_ui.KS.m_denoisingMethod == 2);
                            context.denoisingMethod = m_ui.KS.m_denoisingMethod == 1 ? SDK::D3D11::DenoisingContextInput::DenoisingMethod::NRD_Reblur : SDK::D3D11::DenoisingContextInput::DenoisingMethod::NRD_Relax;
                            if (m_ui.KS.m_enableReflection && m_ui.KS.m_enableGI)
                                context.signalType = SDK::D3D11::DenoisingContextInput::SignalType::SpecularAndDiffuse;
                            else if (m_ui.KS.m_enableGI)
                                context.signalType = SDK::D3D11::DenoisingContextInput::SignalType::Diffuse;
                            else if (m_ui.KS.m_enableReflection)
                                context.signalType = SDK::D3D11::DenoisingContextInput::SignalType::Specular;

                            m_SDKContext.m_denosingContext.specDiff.m_11 = m_SDKContext.m_11->m_executeContext->CreateDenoisingContextHandle(&context);
                            if (m_SDKContext.m_denosingContext.specDiff.m_11 == SDK::D3D11::DenoisingContextHandle::Null) {
                                log::fatal("KickStartRTX: CreateDenoisingContextHandle() failed. : %d", (uint32_t)sts);
                            }
                        }

                        if (enableAODenoising) {
                            SDK::D3D11::DenoisingContextInput context;
                            context.maxWidth = m_RenderTargets[Layer::Opaque]->GBufferRTAO->getDesc().width;
                            context.maxHeight = m_RenderTargets[Layer::Opaque]->GBufferRTAO->getDesc().height;
                            context.denoisingMethod = SDK::D3D11::DenoisingContextInput::DenoisingMethod::NRD_Reblur;
                            context.signalType = SDK::D3D11::DenoisingContextInput::SignalType::DiffuseOcclusion;

                            m_SDKContext.m_denosingContext.ao.m_11 = m_SDKContext.m_11->m_executeContext->CreateDenoisingContextHandle(&context);
                            if (m_SDKContext.m_denosingContext.ao.m_11 == SDK::D3D11::DenoisingContextHandle::Null) {
                                log::fatal("KickStartRTX: CreateDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }

                        if (enableShadowDenoising) {
                            SDK::D3D11::DenoisingContextInput context;
                            context.maxWidth = m_RenderTargets[Layer::Opaque]->GBufferRTShadows->getDesc().width;
                            context.maxHeight = m_RenderTargets[Layer::Opaque]->GBufferRTShadows->getDesc().height;
                            context.denoisingMethod = SDK::D3D11::DenoisingContextInput::DenoisingMethod::NRD_Sigma;
                            context.signalType = m_ui.KS.m_enableShadows == 1 ? SDK::D3D11::DenoisingContextInput::SignalType::Shadow : SDK::D3D11::DenoisingContextInput::SignalType::MultiShadow;

                            m_SDKContext.m_denosingContext.shadow.m_11 = m_SDKContext.m_11->m_executeContext->CreateDenoisingContextHandle(&context);
                            if (m_SDKContext.m_denosingContext.shadow.m_11 == SDK::D3D11::DenoisingContextHandle::Null) {
                                log::fatal("KickStartRTX: CreateDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }
                    }
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                    if (m_SDKContext.m_12) {
                        if (enableReflectionDenoising) {
                            SDK::D3D12::DenoisingContextInput context;
                            context.maxWidth = m_RenderTargets[Layer::Opaque]->GBufferRTReflections->getDesc().width;
                            context.maxHeight = m_RenderTargets[Layer::Opaque]->GBufferRTReflections->getDesc().height;
                            assert(m_ui.KS.m_denoisingMethod == 1 || m_ui.KS.m_denoisingMethod == 2);
                            context.denoisingMethod = m_ui.KS.m_denoisingMethod == 1 ? SDK::D3D12::DenoisingContextInput::DenoisingMethod::NRD_Reblur : SDK::D3D12::DenoisingContextInput::DenoisingMethod::NRD_Relax;
                            if (m_ui.KS.m_enableReflection && m_ui.KS.m_enableGI)
                                context.signalType = SDK::D3D12::DenoisingContextInput::SignalType::SpecularAndDiffuse;
                            else if (m_ui.KS.m_enableGI)
                                context.signalType = SDK::D3D12::DenoisingContextInput::SignalType::Diffuse;
                            else if (m_ui.KS.m_enableReflection)
                                context.signalType = SDK::D3D12::DenoisingContextInput::SignalType::Specular;

                            m_SDKContext.m_denosingContext.specDiff.m_12 = m_SDKContext.m_12->m_executeContext->CreateDenoisingContextHandle(&context);
                            if (m_SDKContext.m_denosingContext.specDiff.m_12 == SDK::D3D12::DenoisingContextHandle::Null) {
                                log::fatal("KickStartRTX: CreateDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }

                        if (enableAODenoising) {
                            SDK::D3D12::DenoisingContextInput context;
                            context.maxWidth = m_RenderTargets[Layer::Opaque]->GBufferRTAO->getDesc().width;
                            context.maxHeight = m_RenderTargets[Layer::Opaque]->GBufferRTAO->getDesc().height;
                            context.denoisingMethod = SDK::D3D12::DenoisingContextInput::DenoisingMethod::NRD_Reblur;
                            context.signalType = SDK::D3D12::DenoisingContextInput::SignalType::DiffuseOcclusion;

                            m_SDKContext.m_denosingContext.ao.m_12 = m_SDKContext.m_12->m_executeContext->CreateDenoisingContextHandle(&context);
                            if (m_SDKContext.m_denosingContext.ao.m_12 == SDK::D3D12::DenoisingContextHandle::Null) {
                                log::fatal("KickStartRTX: CreateDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }

                        if (enableShadowDenoising) {
                            SDK::D3D12::DenoisingContextInput context;
                            context.maxWidth = m_RenderTargets[Layer::Opaque]->GBufferRTShadows->getDesc().width;
                            context.maxHeight = m_RenderTargets[Layer::Opaque]->GBufferRTShadows->getDesc().height;
                            context.denoisingMethod = SDK::D3D12::DenoisingContextInput::DenoisingMethod::NRD_Sigma;
                            context.signalType = m_ui.KS.m_enableShadows == 1 ? SDK::D3D12::DenoisingContextInput::SignalType::Shadow : SDK::D3D12::DenoisingContextInput::SignalType::MultiShadow;

                            m_SDKContext.m_denosingContext.shadow.m_12 = m_SDKContext.m_12->m_executeContext->CreateDenoisingContextHandle(&context);
                            if (m_SDKContext.m_denosingContext.shadow.m_12 == SDK::D3D12::DenoisingContextHandle::Null) {
                                log::fatal("KickStartRTX: CreateDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }
                    }
#endif
#ifdef KickstartRT_Demo_WITH_VK
                    if (m_SDKContext.m_vk) {
                        if (enableReflectionDenoising) {
                            SDK::VK::DenoisingContextInput context;
                            context.maxWidth = m_RenderTargets[Layer::Opaque]->GBufferRTReflections->getDesc().width;
                            context.maxHeight = m_RenderTargets[Layer::Opaque]->GBufferRTReflections->getDesc().height;
                            assert(m_ui.KS.m_denoisingMethod == 1 || m_ui.KS.m_denoisingMethod == 2);
                            context.denoisingMethod = m_ui.KS.m_denoisingMethod == 1 ? SDK::VK::DenoisingContextInput::DenoisingMethod::NRD_Reblur : SDK::VK::DenoisingContextInput::DenoisingMethod::NRD_Relax;
                            if (m_ui.KS.m_enableReflection && m_ui.KS.m_enableGI)
                                context.signalType = SDK::VK::DenoisingContextInput::SignalType::SpecularAndDiffuse;
                            else if (m_ui.KS.m_enableGI)
                                context.signalType = SDK::VK::DenoisingContextInput::SignalType::Diffuse;
                            else if (m_ui.KS.m_enableReflection)
                                context.signalType = SDK::VK::DenoisingContextInput::SignalType::Specular;

                            m_SDKContext.m_denosingContext.specDiff.m_VK = m_SDKContext.m_vk->m_executeContext->CreateDenoisingContextHandle(&context);
                            if (m_SDKContext.m_denosingContext.specDiff.m_VK == SDK::VK::DenoisingContextHandle::Null) {
                                log::fatal("KickStartRTX: CreateDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }

                        if (enableAODenoising) {
                            SDK::VK::DenoisingContextInput context;
                            context.maxWidth = m_RenderTargets[Layer::Opaque]->GBufferRTAO->getDesc().width;
                            context.maxHeight = m_RenderTargets[Layer::Opaque]->GBufferRTAO->getDesc().height;
                            context.denoisingMethod = SDK::VK::DenoisingContextInput::DenoisingMethod::NRD_Reblur;
                            context.signalType = SDK::VK::DenoisingContextInput::SignalType::DiffuseOcclusion;

                            m_SDKContext.m_denosingContext.ao.m_VK = m_SDKContext.m_vk->m_executeContext->CreateDenoisingContextHandle(&context);
                            if (m_SDKContext.m_denosingContext.ao.m_VK == SDK::VK::DenoisingContextHandle::Null) {
                                log::fatal("KickStartRTX: CreateDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }

                        if (enableShadowDenoising) {
                            SDK::VK::DenoisingContextInput context;
                            context.maxWidth = m_RenderTargets[Layer::Opaque]->GBufferRTShadows->getDesc().width;
                            context.maxHeight = m_RenderTargets[Layer::Opaque]->GBufferRTShadows->getDesc().height;
                            context.denoisingMethod = SDK::VK::DenoisingContextInput::DenoisingMethod::NRD_Sigma;
                            context.signalType = m_ui.KS.m_enableShadows == 1 ? SDK::VK::DenoisingContextInput::SignalType::Shadow : SDK::VK::DenoisingContextInput::SignalType::MultiShadow;

                            m_SDKContext.m_denosingContext.shadow.m_VK = m_SDKContext.m_vk->m_executeContext->CreateDenoisingContextHandle(&context);
                            if (m_SDKContext.m_denosingContext.shadow.m_VK == SDK::VK::DenoisingContextHandle::Null) {
                                log::fatal("KickStartRTX: CreateDenoisingContext() failed. : %d", (uint32_t)sts);
                            }
                        }
                    }
#endif
                }
            }
        }

        // DirectLight Injection task.
        {
#ifdef KickstartRT_Demo_WITH_D3D11
            if (m_SDKContext.m_11) {
                SDK::D3D11::RenderTask::DirectLightingInjectionTask inputs;

                inputs.useInlineRT = m_ui.KS.m_useTraceRayInline;
                inputs.injectionResolutionStride = m_ui.KS.m_lightInjectionStride;
                inputs.depth.tex = GetShaderResourceTexD3D11(m_RenderTargets[Layer::Opaque]->GBufferWorldPosition);
                inputs.depth.type = SDK::D3D11::RenderTask::DepthType::RGB_WorldSpace;

                inputs.directLighting = GetShaderResourceTexD3D11(m_RenderTargets[Layer::Opaque]->HdrColor);
                {
                    float2 renderTargetSize = float2(m_RenderTargets[Layer::Opaque]->GetSize());

                    inputs.viewport.topLeftX = 0;
                    inputs.viewport.topLeftY = 0;
                    inputs.viewport.width = (uint32_t)renderTargetSize.x;
                    inputs.viewport.height = (uint32_t)renderTargetSize.y;
                    inputs.viewport.minDepth = 0.0;
                    inputs.viewport.maxDepth = 1.0;
                }
                {
                    auto& invMat = m_View->GetInverseProjectionMatrix();
                    memcpy(inputs.clipToViewMatrix.f, invMat.m_data, sizeof(float) * 16);
                }
                {
                    const dm::affine3& invAf3 = m_View->GetInverseViewMatrix();
                    const dm::float4x4 m = math::affineToHomogeneous(invAf3);
                    memcpy(inputs.viewToWorldMatrix.f, m.m_data, sizeof(float) * 16);
                }

                if (m_ui.KS.m_enableLateLightInjection) {
                    if (tc_post_11 != nullptr) {
                        sts = tc_post_11->ScheduleRenderTask(&inputs);
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                        }
                    }
                }
                else {
                    if (tc11 != nullptr) {
                        sts = tc11->ScheduleRenderTask(&inputs);
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                        }
                    }
				}
            }
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
            if (m_SDKContext.m_12) {
                {
                    SDK::D3D12::RenderTask::DirectLightingInjectionTask inputs;

                    inputs.useInlineRT = m_ui.KS.m_useTraceRayInline;
                    inputs.injectionResolutionStride = m_ui.KS.m_lightInjectionStride;
                    inputs.depth.tex = GetShaderResourceTexD3D12(m_RenderTargets[Layer::Opaque]->GBufferWorldPosition);
                    inputs.depth.type = SDK::D3D12::RenderTask::DepthType::RGB_WorldSpace;

                    inputs.directLighting = GetShaderResourceTexD3D12(m_RenderTargets[Layer::Opaque]->HdrColor);

                    {
                        float2 renderTargetSize = float2(m_RenderTargets[Layer::Opaque]->GetSize());

                        inputs.viewport.topLeftX = 0;
                        inputs.viewport.topLeftY = 0;
                        inputs.viewport.width = (uint32_t)renderTargetSize.x;
                        inputs.viewport.height = (uint32_t)renderTargetSize.y;
                        inputs.viewport.minDepth = 0.0;
                        inputs.viewport.maxDepth = 1.0;
                    }
                    {
                        auto& invMat = m_View->GetInverseProjectionMatrix();
                        memcpy(inputs.clipToViewMatrix.f, invMat.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::affine3& invAf3 = m_View->GetInverseViewMatrix();
                        const dm::float4x4 m = math::affineToHomogeneous(invAf3);
                        memcpy(inputs.viewToWorldMatrix.f, m.m_data, sizeof(float) * 16);
                    }

                    if (m_ui.KS.m_enableLateLightInjection) {
                        if (tc_post_12 != nullptr) {
                            sts = tc_post_12->ScheduleRenderTask(&inputs);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                            }
                        }
                    }
                    else {
                        if (tc12 != nullptr) {
                            sts = tc12->ScheduleRenderTask(&inputs);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                            }
                        }
                    }
                }

                if (m_ui.KS.m_performTransfer)
                {
                    for (auto&& itr : m_SDKContext.m_insStates)
                    {
                        if (!itr.second.instanceProp_LightTransferTarget)
                            continue;

                        auto SDKInsIt = m_SDKContext.m_insHandles.find(itr.first);
                        assert(SDKInsIt != m_SDKContext.m_insHandles.end());

                        SDK::D3D12::RenderTask::DirectLightTransferTask transfer;
                        #ifdef KickstartRT_Demo_WITH_D3D12
                        transfer.target = SDKInsIt->second->m_12.m_iTask.handle;
                        #endif
                        transfer.useInlineRT = m_ui.KS.m_useTraceRayInline;

                        if (tc12 != nullptr) {
                            sts = tc12->ScheduleRenderTask(&transfer);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                            }
                        }
                        break;
                    }

                    m_ui.KS.m_performTransfer = false;
                }
            }
#endif
#ifdef KickstartRT_Demo_WITH_VK
            if (m_SDKContext.m_vk) {
                SDK::VK::RenderTask::DirectLightingInjectionTask inputs;

                inputs.useInlineRT = m_ui.KS.m_useTraceRayInline;
                inputs.injectionResolutionStride = m_ui.KS.m_lightInjectionStride;
                inputs.depth.tex = GetShaderResourceTexVK(m_RenderTargets[Layer::Opaque]->GBufferWorldPosition);
                inputs.depth.type = SDK::VK::RenderTask::DepthType::RGB_WorldSpace;

                inputs.directLighting = GetShaderResourceTexVK(m_RenderTargets[Layer::Opaque]->HdrColor);

                {
                    float2 renderTargetSize = float2(m_RenderTargets[Layer::Opaque]->GetSize());

                    inputs.viewport.topLeftX = 0;
                    inputs.viewport.topLeftY = 0;
                    inputs.viewport.width = (uint32_t)renderTargetSize.x;
                    inputs.viewport.height = (uint32_t)renderTargetSize.y;
                    inputs.viewport.minDepth = 0.0;
                    inputs.viewport.maxDepth = 1.0;
                }
                {
                    auto invMat = m_View->GetInverseProjectionMatrix();
                    memcpy(inputs.clipToViewMatrix.f, invMat.m_data, sizeof(float) * 16);
                }
                {
                    const dm::affine3& invAf3 = m_View->GetInverseViewMatrix();
                    const dm::float4x4 m = math::affineToHomogeneous(invAf3);
                    memcpy(inputs.viewToWorldMatrix.f, m.m_data, sizeof(float) * 16);
                }

                if (m_ui.KS.m_enableLateLightInjection) {
                    if (tc_post_VK != nullptr) {
                        sts = tc_post_VK->ScheduleRenderTask(&inputs);
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                        }
                    }
                }
                else {
                    if (tcVK != nullptr) {
                        sts = tcVK->ScheduleRenderTask(&inputs);
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                        }
                    }
                }
            }
#endif
        }

        // Reflection tasks
        {
#ifdef KickstartRT_Demo_WITH_D3D11
            if (m_SDKContext.m_11) {
                { // Opaque main view

                    SDK::D3D11::RenderTask::TraceTaskCommon rtTaskCommon;
                    if (enableCheckerboard) {
                        rtTaskCommon.halfResolutionMode = GetFrameIndex() % 2 == 0 ? SDK::D3D11::RenderTask::HalfResolutionMode::CHECKERBOARD : SDK::D3D11::RenderTask::HalfResolutionMode::CHECKERBOARD_INVERTED;
                    }
                    rtTaskCommon.useInlineRT = m_ui.KS.m_useTraceRayInline;
                    rtTaskCommon.enableBilinearSampling = m_ui.KS.m_surfelSampleMode == 1;

                    if (m_ui.KS.m_enableDirectLightingSample) {
                        rtTaskCommon.directLighting = GetShaderResourceTexD3D11(m_RenderTargets[Layer::Opaque]->HdrColor);
                    }

                    if (m_ui.KS.m_enableWorldPosFromDepth) {
                        rtTaskCommon.depth.tex = GetShaderResourceTexD3D11(m_RenderTargets[Layer::Opaque]->Depth);
                        rtTaskCommon.depth.type = SDK::D3D11::RenderTask::DepthType::R_ClipSpace;
                    }
                    else {
                        rtTaskCommon.depth.tex = GetShaderResourceTexD3D11(m_RenderTargets[Layer::Opaque]->GBufferWorldPosition);
                        rtTaskCommon.depth.type = SDK::D3D11::RenderTask::DepthType::RGB_WorldSpace;
                    }

                    rtTaskCommon.normal.tex = GetShaderResourceTexD3D11(m_RenderTargets[Layer::Opaque]->GBufferNormals);
                    rtTaskCommon.normal.type = SDK::D3D11::RenderTask::NormalType::RGB_Vector;

                    if (m_ui.KS.m_enableGlobalRoughness) {
                        rtTaskCommon.roughness.globalRoughness = m_ui.KS.m_globalRoughness;
                    }
                    else {
                        rtTaskCommon.roughness.tex = GetShaderResourceTexD3D11(m_RenderTargets[Layer::Opaque]->GBufferNormals);
                        rtTaskCommon.roughness.roughnessMask = { 0.f, 0.f, 0.f, 1.f }; // Alpha channel holds roughness value.
                    }

                    if (m_ui.KS.m_enableGlobalMetalness) {
                        rtTaskCommon.specular.globalMetalness = m_ui.KS.m_globalMetalness;
                    }
                    else {
                        rtTaskCommon.specular.tex = GetShaderResourceTexD3D11(m_RenderTargets[Layer::Opaque]->GBufferSpecular);
                    }

                    {
                        float2 renderTargetSize = float2(m_RenderTargets[Layer::Opaque]->GetSize());

                        rtTaskCommon.viewport.topLeftX = 0;
                        rtTaskCommon.viewport.topLeftY = 0;
                        rtTaskCommon.viewport.width = (uint32_t)renderTargetSize.x;
                        rtTaskCommon.viewport.height = (uint32_t)renderTargetSize.y;
                        rtTaskCommon.viewport.minDepth = 0.0;
                        rtTaskCommon.viewport.maxDepth = 1.0;
                    }

                    const bool bIncludeOffset = false;
                    {
                        const dm::float4x4& invMat = m_View->GetInverseProjectionMatrix(bIncludeOffset);
                        memcpy(rtTaskCommon.clipToViewMatrix.f, invMat.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::affine3& invAf3 = m_View->GetInverseViewMatrix();
                        const dm::float4x4 m = math::affineToHomogeneous(invAf3);
                        memcpy(rtTaskCommon.viewToWorldMatrix.f, m.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::affine3& af3 = m_View->GetViewMatrix();
                        const dm::float4x4 m = math::affineToHomogeneous(af3);
                        memcpy(rtTaskCommon.worldToViewMatrix.f, m.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::float4x4& mat = m_View->GetProjectionMatrix(bIncludeOffset);
                        memcpy(rtTaskCommon.viewToClipMatrix.f, mat.m_data, sizeof(float) * 16);
                    }

                    rtTaskCommon.maxRayLength = m_ui.KS.m_maxRayLength;

                    if (m_ui.KS.m_rayOffsetType == 1) {
                        rtTaskCommon.rayOffset.type = SDK::D3D11::RenderTask::RayOffset::Type::e_WorldPosition;
                        rtTaskCommon.rayOffset.worldPosition.threshold = m_ui.KS.m_rayOffset_WorldPosition_threshold;
                        rtTaskCommon.rayOffset.worldPosition.floatScale = m_ui.KS.m_rayOffset_WorldPosition_floatScale;
                        rtTaskCommon.rayOffset.worldPosition.intScale = m_ui.KS.m_rayOffset_WorldPosition_intScale;
                    }
                    else if (m_ui.KS.m_rayOffsetType == 2) {
                        rtTaskCommon.rayOffset.type = SDK::D3D11::RenderTask::RayOffset::Type::e_CamDistance;
                        rtTaskCommon.rayOffset.camDistance.constant = m_ui.KS.m_rayOffset_CamDistance_constant;
                        rtTaskCommon.rayOffset.camDistance.linear = m_ui.KS.m_rayOffset_CamDistance_linear;
                        rtTaskCommon.rayOffset.camDistance.quadratic = m_ui.KS.m_rayOffset_CamDistance_quadratic;
                    }

                    if (m_ui.KS.m_debugDisp != 0) {
                        SDK::D3D11::RenderTask::TraceSpecularTask rtTask;
                        rtTask.common = rtTaskCommon;
                        rtTask.common.useInlineRT = m_ui.KS.m_useTraceRayInline;
                        rtTask.common.halfResolutionMode = SDK::D3D11::RenderTask::HalfResolutionMode::OFF;
                        rtTask.debugParameters.debugOutputType = (SDK::D3D11::RenderTask::DebugParameters::DebugOutputType)m_ui.KS.m_debugDisp;
                        rtTask.out = GetUnorderedAccessTexD3D11(GBufferRTReflections[Layer::Opaque]);

                        sts = tc11->ScheduleRenderTask(&rtTask);
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                        }
                    }
                    else {
                        if (m_ui.KS.m_enableReflection) {
                            SDK::D3D11::RenderTask::TraceSpecularTask rtTask;
                            rtTask.common = rtTaskCommon;
                            rtTask.common.useInlineRT = m_ui.KS.m_useTraceRayInline;
                            if (enableCheckerboard && enableReflectionDenoising) {
                                rtTask.common.halfResolutionMode = GetFrameIndex() % 2 == 0 ? SDK::D3D11::RenderTask::HalfResolutionMode::CHECKERBOARD : SDK::D3D11::RenderTask::HalfResolutionMode::CHECKERBOARD_INVERTED;
                            }
                            rtTask.out = GetUnorderedAccessTexD3D11(GBufferRTReflections[Layer::Opaque]);

                            sts = tc11->ScheduleRenderTask(&rtTask);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                            }
                        }
                        if (m_ui.KS.m_enableGI) {
                            SDK::D3D11::RenderTask::TraceDiffuseTask rtTask;
                            rtTask.common = rtTaskCommon;
                            if (enableCheckerboard && enableReflectionDenoising) {
                                rtTask.common.halfResolutionMode = GetFrameIndex() % 2 == 0 ? SDK::D3D11::RenderTask::HalfResolutionMode::CHECKERBOARD : SDK::D3D11::RenderTask::HalfResolutionMode::CHECKERBOARD_INVERTED;
                            }
                            rtTask.diffuseBRDFType = SDK::D3D11::RenderTask::DiffuseBRDFType::NormalizedDisney;
                            rtTask.out = GetUnorderedAccessTexD3D11(GBufferRTGI[Layer::Opaque]);

                            sts = tc11->ScheduleRenderTask(&rtTask);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                            }
                        }
                        if (m_ui.KS.m_enableAO) {
                            SDK::D3D11::RenderTask::TraceAmbientOcclusionTask rtTask;
                            rtTask.common = rtTaskCommon;
                            rtTask.out = GetUnorderedAccessTexD3D11(GBufferRTAO[Layer::Opaque]);

                            sts = tc_pre_11->ScheduleRenderTask(&rtTask);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                            }
                        }

                        if (m_ui.KS.m_enableShadows) {

                            if (m_ui.KS.m_enableShadows == 1) {
                                SDK::D3D11::RenderTask::TraceShadowTask rtTask;
                                rtTask.common = rtTaskCommon;
                                rtTask.common.halfResolutionMode = SDK::D3D11::RenderTask::HalfResolutionMode::OFF;
                                rtTask.enableFirstHitAndEndSearch = m_ui.KS.m_shadowsEnableFirstHitAndEndSearch;
                                SetupLightInfosD3D11(&rtTask.lightInfo, 1);

                                rtTask.out = GetUnorderedAccessTexD3D11(GBufferRTShadows[Layer::Opaque]);

                                sts = tc_pre_11->ScheduleRenderTask(&rtTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                            else {
                                SDK::D3D11::RenderTask::TraceMultiShadowTask rtTask;
                                rtTask.common = rtTaskCommon;
                                rtTask.common.halfResolutionMode = SDK::D3D11::RenderTask::HalfResolutionMode::OFF;
                                rtTask.enableFirstHitAndEndSearch = m_ui.KS.m_shadowsEnableFirstHitAndEndSearch;
                                rtTask.numLights = SetupLightInfosD3D11(rtTask.lightInfos, SDK::D3D11::RenderTask::TraceMultiShadowTask::kMaxLightNum);

                                rtTask.out0 = GetUnorderedAccessTexD3D11(GBufferRTShadows[Layer::Opaque]);
                                rtTask.out1 = GetUnorderedAccessTexD3D11(m_RenderTargets[Layer::Opaque]->GBufferRTShadowsAux);

                                sts = tc_pre_11->ScheduleRenderTask(&rtTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                        }
                    }

                    if (enableDenoising)
                    {
                        SDK::D3D11::RenderTask::DenoisingTaskCommon dTaskCommon;

                        dTaskCommon.mode = m_ui.KS.m_denoisingReset ? SDK::D3D11::RenderTask::DenoisingTaskCommon::Mode::DiscardHistory : SDK::D3D11::RenderTask::DenoisingTaskCommon::Mode::Continue;

                        if (enableCheckerboard) {
                            dTaskCommon.halfResolutionMode = GetFrameIndex() % 2 == 0 ? SDK::D3D11::RenderTask::HalfResolutionMode::CHECKERBOARD : SDK::D3D11::RenderTask::HalfResolutionMode::CHECKERBOARD_INVERTED;
                        }

                        dTaskCommon.viewport = rtTaskCommon.viewport;
                        dTaskCommon.depth = rtTaskCommon.depth;
                        dTaskCommon.normal = rtTaskCommon.normal;
                        dTaskCommon.roughness = rtTaskCommon.roughness;
                        {
                            auto& desc = m_RenderTargets[Layer::Opaque]->MotionVectors->getDesc();
                            dTaskCommon.motion.tex = GetShaderResourceTexD3D11(m_RenderTargets[Layer::Opaque]->MotionVectors);
                            dTaskCommon.motion.type = SDK::D3D11::RenderTask::MotionType::RG_ViewSpace;
                            dTaskCommon.motion.scale.f[0] = 1.f / desc.width;
                            dTaskCommon.motion.scale.f[1] = 1.f / desc.height;
                        }
                        const bool bIncludeOffset = false;
                        {
                            const dm::float4x4& invMat = m_View->GetInverseProjectionMatrix(bIncludeOffset);
                            memcpy(dTaskCommon.clipToViewMatrix.f, invMat.m_data, sizeof(float) * 16);
                        }
                        {
                            const dm::float4x4& mat = m_View->GetProjectionMatrix(bIncludeOffset);
                            memcpy(dTaskCommon.viewToClipMatrix.f, mat.m_data, sizeof(float) * 16);
                        }
                        {
                            const dm::float4x4& mat = m_ViewPrevious->GetProjectionMatrix(bIncludeOffset);
                            memcpy(dTaskCommon.viewToClipMatrixPrev.f, mat.m_data, sizeof(float) * 16);
                        }
                        {
                            const dm::affine3& af3 = m_View->GetViewMatrix();
                            const dm::float4x4 m = math::affineToHomogeneous(af3);
                            memcpy(dTaskCommon.worldToViewMatrix.f, m.m_data, sizeof(float) * 16);
                        }
                        {
                            const dm::affine3& af3 = m_ViewPrevious->GetViewMatrix();
                            const dm::float4x4 m = math::affineToHomogeneous(af3);
                            memcpy(dTaskCommon.worldToViewMatrixPrev.f, m.m_data, sizeof(float) * 16);
                        }
                        {
                            dTaskCommon.cameraJitter.f[0] = m_View->GetPixelOffset().x;
                            dTaskCommon.cameraJitter.f[1] = m_View->GetPixelOffset().y;
                        }

                        if (enableReflectionDenoising)
                        {
                            if (m_ui.KS.m_enableReflection && m_ui.KS.m_enableGI) {
                                SDK::D3D11::RenderTask::DenoiseSpecularAndDiffuseTask dTask;
                                dTask.common = dTaskCommon;
                                dTask.context = m_SDKContext.m_denosingContext.specDiff.m_11;
                                dTask.inSpecular = GetShaderResourceTexD3D11(GBufferRTReflections[Layer::Opaque]);
                                dTask.inOutSpecular = GetCombinedAccessTexD3D11(m_RenderTargets[Layer::Opaque]->GBufferRTReflectionsFinal);
                                dTask.inDiffuse = GetShaderResourceTexD3D11(GBufferRTGI[Layer::Opaque]);
                                dTask.inOutDiffuse = GetCombinedAccessTexD3D11(m_RenderTargets[Layer::Opaque]->GBufferRTGIFinal);

                                sts = tc11->ScheduleRenderTask(&dTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                            else if (m_ui.KS.m_enableReflection) {
                                SDK::D3D11::RenderTask::DenoiseSpecularTask dTask;
                                dTask.common = dTaskCommon;
                                dTask.context = m_SDKContext.m_denosingContext.specDiff.m_11;
                                dTask.inSpecular = GetShaderResourceTexD3D11(GBufferRTReflections[Layer::Opaque]);
                                dTask.inOutSpecular = GetCombinedAccessTexD3D11(m_RenderTargets[Layer::Opaque]->GBufferRTReflectionsFinal);

                                sts = tc11->ScheduleRenderTask(&dTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                            else if (m_ui.KS.m_enableGI) {
                                SDK::D3D11::RenderTask::DenoiseDiffuseTask dTask;
                                dTask.common = dTaskCommon;
                                dTask.context = m_SDKContext.m_denosingContext.specDiff.m_11;
                                dTask.inDiffuse = GetShaderResourceTexD3D11(GBufferRTGI[Layer::Opaque]);
                                dTask.inOutDiffuse = GetCombinedAccessTexD3D11(m_RenderTargets[Layer::Opaque]->GBufferRTGIFinal);

                                sts = tc11->ScheduleRenderTask(&dTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                        }

                        if (enableAODenoising)
                        {
                            SDK::D3D11::RenderTask::DenoiseDiffuseOcclusionTask dTask;
                            dTask.context = m_SDKContext.m_denosingContext.ao.m_11;
                            dTask.common = dTaskCommon;
                            dTask.inHitT = GetShaderResourceTexD3D11(GBufferRTAO[Layer::Opaque]);
                            dTask.inOutOcclusion = GetCombinedAccessTexD3D11(m_RenderTargets[Layer::Opaque]->GBufferRTAOFinal);

                            sts = tc_pre_11->ScheduleRenderTask(&dTask);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                            }
                        }

                        if (enableShadowDenoising)
                        {
                            if (m_ui.KS.m_enableShadows == 2)
                            {
                                SDK::D3D11::RenderTask::DenoiseMultiShadowTask dTask;
                                dTask.context = m_SDKContext.m_denosingContext.shadow.m_11;
                                dTask.common = dTaskCommon;
                                dTask.common.halfResolutionMode = SDK::D3D11::RenderTask::HalfResolutionMode::OFF;
                                dTask.inShadow0 = GetShaderResourceTexD3D11(GBufferRTShadows[Layer::Opaque]);
                                dTask.inShadow1 = GetShaderResourceTexD3D11(m_RenderTargets[Layer::Opaque]->GBufferRTShadowsAux);
                                dTask.inOutShadow = GetCombinedAccessTexD3D11(m_RenderTargets[Layer::Opaque]->GBufferRTShadowsFinal);

                                sts = tc_pre_11->ScheduleRenderTask(&dTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                            else {
                                SDK::D3D11::RenderTask::DenoiseShadowTask dTask;
                                dTask.context = m_SDKContext.m_denosingContext.shadow.m_11;
                                dTask.common = dTaskCommon;
                                dTask.common.halfResolutionMode = SDK::D3D11::RenderTask::HalfResolutionMode::OFF;
                                dTask.inShadow = GetShaderResourceTexD3D11(GBufferRTShadows[Layer::Opaque]);
                                dTask.inOutShadow = GetCombinedAccessTexD3D11(m_RenderTargets[Layer::Opaque]->GBufferRTShadowsFinal);

                                sts = tc_pre_11->ScheduleRenderTask(&dTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                        }
                    }
                }

                // Transparent reflection view
                if (m_ui.KS.m_enableTransparentReflection)
                {
                    SDK::D3D11::RenderTask::TraceSpecularTask rtTask;

                    rtTask.common.useInlineRT = m_ui.KS.m_useTraceRayInline;

                    if (m_ui.KS.m_enableDirectLightingSample) {
                        rtTask.common.directLighting = GetShaderResourceTexD3D11(m_RenderTargets[Layer::Opaque]->HdrColor);
                    }

                    rtTask.common.depth.tex = GetShaderResourceTexD3D11(m_RenderTargets[Layer::Transparent0]->GBufferWorldPosition);
                    rtTask.common.depth.type = SDK::D3D11::RenderTask::DepthType::RGB_WorldSpace;

                    rtTask.common.normal.tex = GetShaderResourceTexD3D11(m_RenderTargets[Layer::Transparent0]->GBufferNormals);
                    rtTask.common.normal.type = SDK::D3D11::RenderTask::NormalType::RGB_Vector;

                    rtTask.common.roughness.globalRoughness = 0.f;
                    rtTask.common.specular.globalMetalness = 1.0;

                    {
                        float2 renderTargetSize = float2(m_RenderTargets[Layer::Transparent0]->GetSize());

                        rtTask.common.viewport.topLeftX = 0;
                        rtTask.common.viewport.topLeftY = 0;
                        rtTask.common.viewport.width = (uint32_t)renderTargetSize.x;
                        rtTask.common.viewport.height = (uint32_t)renderTargetSize.y;
                        rtTask.common.viewport.minDepth = 0.0;
                        rtTask.common.viewport.maxDepth = 1.0;
                    }
                    {
                        auto& invMat = m_View->GetInverseProjectionMatrix();
                        memcpy(rtTask.common.clipToViewMatrix.f, invMat.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::affine3& invAf3 = m_View->GetInverseViewMatrix();
                        const dm::float4x4 m = math::affineToHomogeneous(invAf3);
                        memcpy(rtTask.common.viewToWorldMatrix.f, m.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::affine3& af3 = m_View->GetViewMatrix();
                        const dm::float4x4 m = math::affineToHomogeneous(af3);
                        memcpy(rtTask.common.worldToViewMatrix.f, m.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::float4x4& mat = m_View->GetProjectionMatrix();
                        memcpy(rtTask.common.viewToClipMatrix.f, mat.m_data, sizeof(float) * 16);
                    }

                    rtTask.common.maxRayLength = m_ui.KS.m_maxRayLength;

                    if (m_ui.KS.m_rayOffsetType == 1) {
                        rtTask.common.rayOffset.type = SDK::D3D11::RenderTask::RayOffset::Type::e_WorldPosition;
                        rtTask.common.rayOffset.worldPosition.threshold = m_ui.KS.m_rayOffset_WorldPosition_threshold;
                        rtTask.common.rayOffset.worldPosition.floatScale = m_ui.KS.m_rayOffset_WorldPosition_floatScale;
                        rtTask.common.rayOffset.worldPosition.intScale = m_ui.KS.m_rayOffset_WorldPosition_intScale;
                    }
                    else if (m_ui.KS.m_rayOffsetType == 2) {
                        rtTask.common.rayOffset.type = SDK::D3D11::RenderTask::RayOffset::Type::e_CamDistance;
                        rtTask.common.rayOffset.camDistance.constant = m_ui.KS.m_rayOffset_CamDistance_constant;
                        rtTask.common.rayOffset.camDistance.linear = m_ui.KS.m_rayOffset_CamDistance_linear;
                        rtTask.common.rayOffset.camDistance.quadratic = m_ui.KS.m_rayOffset_CamDistance_quadratic;
                    }

                    rtTask.out = GetUnorderedAccessTexD3D11(m_RenderTargets[Layer::Transparent0]->GBufferRTReflections);

                    sts = tc11->ScheduleRenderTask(&rtTask);
                    if (sts != SDK::Status::OK) {
                        log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                    }
                }
            }
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
            if (m_SDKContext.m_12) {
                { // Opaque main view

                    SDK::D3D12::RenderTask::TraceTaskCommon rtTaskCommon;
                    if (enableCheckerboard) {
                        rtTaskCommon.halfResolutionMode = GetFrameIndex() % 2 == 0 ? SDK::D3D12::RenderTask::HalfResolutionMode::CHECKERBOARD : SDK::D3D12::RenderTask::HalfResolutionMode::CHECKERBOARD_INVERTED;
                    }
                    rtTaskCommon.useInlineRT = m_ui.KS.m_useTraceRayInline;
                    rtTaskCommon.enableBilinearSampling = m_ui.KS.m_surfelSampleMode == 1;

                    if (m_ui.KS.m_enableDirectLightingSample) {
                        rtTaskCommon.directLighting = GetShaderResourceTexD3D12(m_RenderTargets[Layer::Opaque]->HdrColor);
                    }

                    if (m_ui.KS.m_enableWorldPosFromDepth) {
                        rtTaskCommon.depth.tex = GetShaderResourceTexD3D12(m_RenderTargets[Layer::Opaque]->Depth);
                        rtTaskCommon.depth.type = SDK::D3D12::RenderTask::DepthType::R_ClipSpace;
                    } else {
                        rtTaskCommon.depth.tex = GetShaderResourceTexD3D12(m_RenderTargets[Layer::Opaque]->GBufferWorldPosition);
                        rtTaskCommon.depth.type = SDK::D3D12::RenderTask::DepthType::RGB_WorldSpace;
                    }

                    rtTaskCommon.normal.tex = GetShaderResourceTexD3D12(m_RenderTargets[Layer::Opaque]->GBufferNormals);
                    rtTaskCommon.normal.type = SDK::D3D12::RenderTask::NormalType::RGB_Vector;

                    if (m_ui.KS.m_enableGlobalRoughness) {
                        rtTaskCommon.roughness.globalRoughness = m_ui.KS.m_globalRoughness;
                    }
                    else {
                        rtTaskCommon.roughness.tex = GetShaderResourceTexD3D12(m_RenderTargets[Layer::Opaque]->GBufferNormals);
                        rtTaskCommon.roughness.roughnessMask = { 0.f, 0.f, 0.f, 1.f }; // Alpha channel holds roughness value.
                    }

                    if (m_ui.KS.m_enableGlobalMetalness) {
                        rtTaskCommon.specular.globalMetalness = m_ui.KS.m_globalMetalness;
                    }
                    else {
                        rtTaskCommon.specular.tex = GetShaderResourceTexD3D12(m_RenderTargets[Layer::Opaque]->GBufferSpecular);
                    }

                    {
                        float2 renderTargetSize = float2(m_RenderTargets[Layer::Opaque]->GetSize());

                        rtTaskCommon.viewport.topLeftX = 0;
                        rtTaskCommon.viewport.topLeftY = 0;
                        rtTaskCommon.viewport.width = (uint32_t)renderTargetSize.x;
                        rtTaskCommon.viewport.height = (uint32_t)renderTargetSize.y;
                        rtTaskCommon.viewport.minDepth = 0.0;
                        rtTaskCommon.viewport.maxDepth = 1.0;
                    }

                    const bool bIncludeOffset = false;
                    {
                        const dm::float4x4& invMat = m_View->GetInverseProjectionMatrix(bIncludeOffset);
                        memcpy(rtTaskCommon.clipToViewMatrix.f, invMat.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::affine3& invAf3 = m_View->GetInverseViewMatrix();
                        const dm::float4x4 m = math::affineToHomogeneous(invAf3);
                        memcpy(rtTaskCommon.viewToWorldMatrix.f, m.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::affine3& af3 = m_View->GetViewMatrix();
                        const dm::float4x4 m = math::affineToHomogeneous(af3);
                        memcpy(rtTaskCommon.worldToViewMatrix.f, m.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::float4x4& mat = m_View->GetProjectionMatrix(bIncludeOffset);
                        memcpy(rtTaskCommon.viewToClipMatrix.f, mat.m_data, sizeof(float) * 16);
                    }

                    rtTaskCommon.maxRayLength = m_ui.KS.m_maxRayLength;

                    if (m_ui.KS.m_rayOffsetType == 1) {
                        rtTaskCommon.rayOffset.type = SDK::D3D12::RenderTask::RayOffset::Type::e_WorldPosition;
                        rtTaskCommon.rayOffset.worldPosition.threshold = m_ui.KS.m_rayOffset_WorldPosition_threshold;
                        rtTaskCommon.rayOffset.worldPosition.floatScale = m_ui.KS.m_rayOffset_WorldPosition_floatScale;
                        rtTaskCommon.rayOffset.worldPosition.intScale = m_ui.KS.m_rayOffset_WorldPosition_intScale;
                    }
                    else if (m_ui.KS.m_rayOffsetType == 2) {
                        rtTaskCommon.rayOffset.type = SDK::D3D12::RenderTask::RayOffset::Type::e_CamDistance;
                        rtTaskCommon.rayOffset.camDistance.constant = m_ui.KS.m_rayOffset_CamDistance_constant;
                        rtTaskCommon.rayOffset.camDistance.linear = m_ui.KS.m_rayOffset_CamDistance_linear;
                        rtTaskCommon.rayOffset.camDistance.quadratic = m_ui.KS.m_rayOffset_CamDistance_quadratic;
                    }

                    if (m_ui.KS.m_debugDisp != 0) {
                        SDK::D3D12::RenderTask::TraceSpecularTask rtTask;
                        rtTask.common = rtTaskCommon;
                        rtTask.common.useInlineRT = m_ui.KS.m_useTraceRayInline;
                        rtTask.common.halfResolutionMode = SDK::D3D12::RenderTask::HalfResolutionMode::OFF;
                        rtTask.debugParameters.debugOutputType = (SDK::D3D12::RenderTask::DebugParameters::DebugOutputType)m_ui.KS.m_debugDisp;
                        rtTask.out = GetUnorderedAccessTexD3D12(GBufferRTReflections[Layer::Opaque]);

                        sts = tc12->ScheduleRenderTask(&rtTask);
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                        }
                    }
                    else {
                        if (m_ui.KS.m_enableReflection) {
                            SDK::D3D12::RenderTask::TraceSpecularTask rtTask;
                            rtTask.common = rtTaskCommon;
                            rtTask.common.useInlineRT = m_ui.KS.m_useTraceRayInline;
                            if (enableCheckerboard && enableReflectionDenoising) {
                                rtTask.common.halfResolutionMode = GetFrameIndex() % 2 == 0 ? SDK::D3D12::RenderTask::HalfResolutionMode::CHECKERBOARD : SDK::D3D12::RenderTask::HalfResolutionMode::CHECKERBOARD_INVERTED;
                            }
                            rtTask.out = GetUnorderedAccessTexD3D12(GBufferRTReflections[Layer::Opaque]);

                            sts = tc12->ScheduleRenderTask(&rtTask);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                            }
                        }
                        if (m_ui.KS.m_enableGI) {
                            SDK::D3D12::RenderTask::TraceDiffuseTask rtTask;
                            rtTask.common = rtTaskCommon;
                            if (enableCheckerboard && enableReflectionDenoising) {
                                rtTask.common.halfResolutionMode = GetFrameIndex() % 2 == 0 ? SDK::D3D12::RenderTask::HalfResolutionMode::CHECKERBOARD : SDK::D3D12::RenderTask::HalfResolutionMode::CHECKERBOARD_INVERTED;
                            }
                            rtTask.diffuseBRDFType = SDK::D3D12::RenderTask::DiffuseBRDFType::NormalizedDisney;
                            rtTask.out = GetUnorderedAccessTexD3D12(GBufferRTGI[Layer::Opaque]);

                            sts = tc12->ScheduleRenderTask(&rtTask);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                            }
                        }
                        if (m_ui.KS.m_enableAO) {
                            SDK::D3D12::RenderTask::TraceAmbientOcclusionTask rtTask;
                            rtTask.common = rtTaskCommon;
                            rtTask.out = GetUnorderedAccessTexD3D12(GBufferRTAO[Layer::Opaque]);

                            sts = tc_pre_12->ScheduleRenderTask(&rtTask);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                            }
                        }

                        if (m_ui.KS.m_enableShadows) {

                            if (m_ui.KS.m_enableShadows == 1) {
                                SDK::D3D12::RenderTask::TraceShadowTask rtTask;
                                rtTask.common = rtTaskCommon;
                                rtTask.common.halfResolutionMode = SDK::D3D12::RenderTask::HalfResolutionMode::OFF;
                                rtTask.enableFirstHitAndEndSearch = m_ui.KS.m_shadowsEnableFirstHitAndEndSearch;
                                SetupLightInfosD3D12(&rtTask.lightInfo, 1);

                                rtTask.out = GetUnorderedAccessTexD3D12(GBufferRTShadows[Layer::Opaque]);

                                sts = tc_pre_12->ScheduleRenderTask(&rtTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                            else {
                                SDK::D3D12::RenderTask::TraceMultiShadowTask rtTask;
                                rtTask.common = rtTaskCommon;
                                rtTask.common.halfResolutionMode = SDK::D3D12::RenderTask::HalfResolutionMode::OFF;
                                rtTask.enableFirstHitAndEndSearch = m_ui.KS.m_shadowsEnableFirstHitAndEndSearch;
                                rtTask.numLights = SetupLightInfosD3D12(rtTask.lightInfos, SDK::D3D12::RenderTask::TraceMultiShadowTask::kMaxLightNum);

                                rtTask.out0 = GetUnorderedAccessTexD3D12(GBufferRTShadows[Layer::Opaque]);
                                rtTask.out1 = GetUnorderedAccessTexD3D12(m_RenderTargets[Layer::Opaque]->GBufferRTShadowsAux);

                                sts = tc_pre_12->ScheduleRenderTask(&rtTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                        }
                    }

                    if (enableDenoising)
                    {
                        SDK::D3D12::RenderTask::DenoisingTaskCommon dTaskCommon;

                        dTaskCommon.mode = m_ui.KS.m_denoisingReset ? SDK::D3D12::RenderTask::DenoisingTaskCommon::Mode::DiscardHistory : SDK::D3D12::RenderTask::DenoisingTaskCommon::Mode::Continue;

                        if (enableCheckerboard) {
                            dTaskCommon.halfResolutionMode = GetFrameIndex() % 2 == 0 ? SDK::D3D12::RenderTask::HalfResolutionMode::CHECKERBOARD : SDK::D3D12::RenderTask::HalfResolutionMode::CHECKERBOARD_INVERTED;
                        }

                        dTaskCommon.viewport = rtTaskCommon.viewport;
                        dTaskCommon.depth = rtTaskCommon.depth;
                        dTaskCommon.normal = rtTaskCommon.normal;
                        dTaskCommon.roughness = rtTaskCommon.roughness;
                        {
                            auto& desc = m_RenderTargets[Layer::Opaque]->MotionVectors->getDesc();
                            dTaskCommon.motion.tex = GetShaderResourceTexD3D12(m_RenderTargets[Layer::Opaque]->MotionVectors);
                            dTaskCommon.motion.type = SDK::D3D12::RenderTask::MotionType::RG_ViewSpace;
                            dTaskCommon.motion.scale.f[0] = 1.f / desc.width;
                            dTaskCommon.motion.scale.f[1] = 1.f / desc.height;
                        }
                        const bool bIncludeOffset = false;
                        {
                            const dm::float4x4& invMat = m_View->GetInverseProjectionMatrix(bIncludeOffset);
                            memcpy(dTaskCommon.clipToViewMatrix.f, invMat.m_data, sizeof(float) * 16);
                        }
                        {
                            const dm::float4x4& mat = m_View->GetProjectionMatrix(bIncludeOffset);
                            memcpy(dTaskCommon.viewToClipMatrix.f, mat.m_data, sizeof(float) * 16);
                        }
                        {
                            const dm::float4x4& mat = m_ViewPrevious->GetProjectionMatrix(bIncludeOffset);
                            memcpy(dTaskCommon.viewToClipMatrixPrev.f, mat.m_data, sizeof(float) * 16);
                        }
                        {
                            const dm::affine3& af3 = m_View->GetViewMatrix();
                            const dm::float4x4 m = math::affineToHomogeneous(af3);
                            memcpy(dTaskCommon.worldToViewMatrix.f, m.m_data, sizeof(float) * 16);
                        }
                        {
                            const dm::affine3& af3 = m_ViewPrevious->GetViewMatrix();
                            const dm::float4x4 m = math::affineToHomogeneous(af3);
                            memcpy(dTaskCommon.worldToViewMatrixPrev.f, m.m_data, sizeof(float) * 16);
                        }
                        {
                            dTaskCommon.cameraJitter.f[0] = m_View->GetPixelOffset().x;
                            dTaskCommon.cameraJitter.f[1] = m_View->GetPixelOffset().y;
                        }

                        if (enableReflectionDenoising)
                        {
                            if (m_ui.KS.m_enableReflection && m_ui.KS.m_enableGI) {
                                SDK::D3D12::RenderTask::DenoiseSpecularAndDiffuseTask dTask;
                                dTask.common        = dTaskCommon;
                                dTask.context       = m_SDKContext.m_denosingContext.specDiff.m_12;
                                dTask.inSpecular    = GetShaderResourceTexD3D12(GBufferRTReflections[Layer::Opaque]);
                                dTask.inOutSpecular = GetCombinedAccessTexD3D12(m_RenderTargets[Layer::Opaque]->GBufferRTReflectionsFinal);
                                dTask.inDiffuse     = GetShaderResourceTexD3D12(GBufferRTGI[Layer::Opaque]);
                                dTask.inOutDiffuse  = GetCombinedAccessTexD3D12(m_RenderTargets[Layer::Opaque]->GBufferRTGIFinal);

                                sts = tc12->ScheduleRenderTask(&dTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                            else if (m_ui.KS.m_enableReflection) {
                                SDK::D3D12::RenderTask::DenoiseSpecularTask dTask;
                                dTask.common            = dTaskCommon;
                                dTask.context           = m_SDKContext.m_denosingContext.specDiff.m_12;
                                dTask.inSpecular        = GetShaderResourceTexD3D12(GBufferRTReflections[Layer::Opaque]);
                                dTask.inOutSpecular     = GetCombinedAccessTexD3D12(m_RenderTargets[Layer::Opaque]->GBufferRTReflectionsFinal);

                                sts = tc12->ScheduleRenderTask(&dTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                            else if (m_ui.KS.m_enableGI) {
                                SDK::D3D12::RenderTask::DenoiseDiffuseTask dTask;
                                dTask.common        = dTaskCommon;
                                dTask.context       = m_SDKContext.m_denosingContext.specDiff.m_12;
                                dTask.inDiffuse     = GetShaderResourceTexD3D12(GBufferRTGI[Layer::Opaque]);
                                dTask.inOutDiffuse  = GetCombinedAccessTexD3D12(m_RenderTargets[Layer::Opaque]->GBufferRTGIFinal);

                                sts = tc12->ScheduleRenderTask(&dTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                        }

                        if (enableAODenoising)
                        {
                            SDK::D3D12::RenderTask::DenoiseDiffuseOcclusionTask dTask;
                            dTask.context           = m_SDKContext.m_denosingContext.ao.m_12;
                            dTask.common            = dTaskCommon;
                            dTask.inHitT            = GetShaderResourceTexD3D12(GBufferRTAO[Layer::Opaque]);
                            dTask.inOutOcclusion    = GetCombinedAccessTexD3D12(m_RenderTargets[Layer::Opaque]->GBufferRTAOFinal);

                            sts = tc_pre_12->ScheduleRenderTask(&dTask);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                            }
                        }

                        if (enableShadowDenoising)
                        {
                            if (m_ui.KS.m_enableShadows == 2)
                            {
                                SDK::D3D12::RenderTask::DenoiseMultiShadowTask dTask;
                                dTask.context       = m_SDKContext.m_denosingContext.shadow.m_12;
                                dTask.common        = dTaskCommon;
                                dTask.common.halfResolutionMode        = SDK::D3D12::RenderTask::HalfResolutionMode::OFF;
                                dTask.inShadow0     = GetShaderResourceTexD3D12(GBufferRTShadows[Layer::Opaque]);
                                dTask.inShadow1     = GetShaderResourceTexD3D12(m_RenderTargets[Layer::Opaque]->GBufferRTShadowsAux);
                                dTask.inOutShadow   = GetCombinedAccessTexD3D12(m_RenderTargets[Layer::Opaque]->GBufferRTShadowsFinal);

                                sts = tc_pre_12->ScheduleRenderTask(&dTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                            else {
                                SDK::D3D12::RenderTask::DenoiseShadowTask dTask;
                                dTask.context       = m_SDKContext.m_denosingContext.shadow.m_12;
                                dTask.common        = dTaskCommon;
                                dTask.common.halfResolutionMode = SDK::D3D12::RenderTask::HalfResolutionMode::OFF;
                                dTask.inShadow      = GetShaderResourceTexD3D12(GBufferRTShadows[Layer::Opaque]);
                                dTask.inOutShadow   = GetCombinedAccessTexD3D12(m_RenderTargets[Layer::Opaque]->GBufferRTShadowsFinal);

                                sts = tc_pre_12->ScheduleRenderTask(&dTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                        }
                    }
                }

                // Transparent reflection view
                if (m_ui.KS.m_enableTransparentReflection)
                { 
                    SDK::D3D12::RenderTask::TraceSpecularTask rtTask;

                    rtTask.common.useInlineRT = m_ui.KS.m_useTraceRayInline;

                    if (m_ui.KS.m_enableDirectLightingSample) {
                        rtTask.common.directLighting = GetShaderResourceTexD3D12(m_RenderTargets[Layer::Opaque]->HdrColor);
                    }

                    rtTask.common.depth.tex = GetShaderResourceTexD3D12(m_RenderTargets[Layer::Transparent0]->GBufferWorldPosition);
                    rtTask.common.depth.type = SDK::D3D12::RenderTask::DepthType::RGB_WorldSpace;

                    rtTask.common.normal.tex = GetShaderResourceTexD3D12(m_RenderTargets[Layer::Transparent0]->GBufferNormals);
                    rtTask.common.normal.type = SDK::D3D12::RenderTask::NormalType::RGB_Vector;

                    rtTask.common.roughness.globalRoughness = 0.f;
                    rtTask.common.specular.globalMetalness = 1.0;

                    {
                        float2 renderTargetSize = float2(m_RenderTargets[Layer::Transparent0]->GetSize());

                        rtTask.common.viewport.topLeftX = 0;
                        rtTask.common.viewport.topLeftY = 0;
                        rtTask.common.viewport.width = (uint32_t)renderTargetSize.x;
                        rtTask.common.viewport.height = (uint32_t)renderTargetSize.y;
                        rtTask.common.viewport.minDepth = 0.0;
                        rtTask.common.viewport.maxDepth = 1.0;
                    }
                    {
                        auto& invMat = m_View->GetInverseProjectionMatrix();
                        memcpy(rtTask.common.clipToViewMatrix.f, invMat.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::affine3& invAf3 = m_View->GetInverseViewMatrix();
                        const dm::float4x4 m = math::affineToHomogeneous(invAf3);
                        memcpy(rtTask.common.viewToWorldMatrix.f, m.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::affine3& af3 = m_View->GetViewMatrix();
                        const dm::float4x4 m = math::affineToHomogeneous(af3);
                        memcpy(rtTask.common.worldToViewMatrix.f, m.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::float4x4& mat = m_View->GetProjectionMatrix();
                        memcpy(rtTask.common.viewToClipMatrix.f, mat.m_data, sizeof(float) * 16);
                    }

                    rtTask.common.maxRayLength = m_ui.KS.m_maxRayLength;

                    if (m_ui.KS.m_rayOffsetType == 1) {
                        rtTask.common.rayOffset.type = SDK::D3D12::RenderTask::RayOffset::Type::e_WorldPosition;
                        rtTask.common.rayOffset.worldPosition.threshold = m_ui.KS.m_rayOffset_WorldPosition_threshold;
                        rtTask.common.rayOffset.worldPosition.floatScale = m_ui.KS.m_rayOffset_WorldPosition_floatScale;
                        rtTask.common.rayOffset.worldPosition.intScale = m_ui.KS.m_rayOffset_WorldPosition_intScale;
                    }
                    else if (m_ui.KS.m_rayOffsetType == 2) {
                        rtTask.common.rayOffset.type = SDK::D3D12::RenderTask::RayOffset::Type::e_CamDistance;
                        rtTask.common.rayOffset.camDistance.constant = m_ui.KS.m_rayOffset_CamDistance_constant;
                        rtTask.common.rayOffset.camDistance.linear = m_ui.KS.m_rayOffset_CamDistance_linear;
                        rtTask.common.rayOffset.camDistance.quadratic = m_ui.KS.m_rayOffset_CamDistance_quadratic;
                    }

                    rtTask.out = GetUnorderedAccessTexD3D12(m_RenderTargets[Layer::Transparent0]->GBufferRTReflections);

                    sts = tc12->ScheduleRenderTask(&rtTask);
                    if (sts != SDK::Status::OK) {
                        log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                    }
                }
            }
#endif
#ifdef KickstartRT_Demo_WITH_VK
            if (m_SDKContext.m_vk) {
                { // Opaque main view

                    SDK::VK::RenderTask::TraceTaskCommon rtTaskCommon;
                    if (enableCheckerboard) {
                        rtTaskCommon.halfResolutionMode = GetFrameIndex() % 2 == 0 ? SDK::VK::RenderTask::HalfResolutionMode::CHECKERBOARD : SDK::VK::RenderTask::HalfResolutionMode::CHECKERBOARD_INVERTED;
                    }
                    rtTaskCommon.useInlineRT = m_ui.KS.m_useTraceRayInline;
                    rtTaskCommon.enableBilinearSampling = m_ui.KS.m_surfelSampleMode == 1;

                    if (m_ui.KS.m_enableDirectLightingSample) {
                        rtTaskCommon.directLighting = GetShaderResourceTexVK(m_RenderTargets[Layer::Opaque]->HdrColor);
                    }

                    if (m_ui.KS.m_enableWorldPosFromDepth) {
                        rtTaskCommon.depth.tex = GetShaderResourceTexVK(m_RenderTargets[Layer::Opaque]->Depth);
                        rtTaskCommon.depth.type = SDK::VK::RenderTask::DepthType::R_ClipSpace;
                    }
                    else {
                        rtTaskCommon.depth.tex = GetShaderResourceTexVK(m_RenderTargets[Layer::Opaque]->GBufferWorldPosition);
                        rtTaskCommon.depth.type = SDK::VK::RenderTask::DepthType::RGB_WorldSpace;
                    }

                    rtTaskCommon.normal.tex = GetShaderResourceTexVK(m_RenderTargets[Layer::Opaque]->GBufferNormals);
                    rtTaskCommon.normal.type = SDK::VK::RenderTask::NormalType::RGB_Vector;

                    if (m_ui.KS.m_enableGlobalRoughness) {
                        rtTaskCommon.roughness.globalRoughness = m_ui.KS.m_globalRoughness;
                    }
                    else {
                        rtTaskCommon.roughness.tex = GetShaderResourceTexVK(m_RenderTargets[Layer::Opaque]->GBufferNormals);
                        rtTaskCommon.roughness.roughnessMask = { 0.f, 0.f, 0.f, 1.f }; // Alpha channel holds roughness value.
                    }

                    if (m_ui.KS.m_enableGlobalMetalness) {
                        rtTaskCommon.specular.globalMetalness = m_ui.KS.m_globalMetalness;
                    }
                    else {
                        rtTaskCommon.specular.tex = GetShaderResourceTexVK(m_RenderTargets[Layer::Opaque]->GBufferSpecular);
                    }

                    {
                        float2 renderTargetSize = float2(m_RenderTargets[Layer::Opaque]->GetSize());

                        rtTaskCommon.viewport.topLeftX = 0;
                        rtTaskCommon.viewport.topLeftY = 0;
                        rtTaskCommon.viewport.width = (uint32_t)renderTargetSize.x;
                        rtTaskCommon.viewport.height = (uint32_t)renderTargetSize.y;
                        rtTaskCommon.viewport.minDepth = 0.0;
                        rtTaskCommon.viewport.maxDepth = 1.0;
                    }

                    const bool bIncludeOffset = false;
                    {
                        const dm::float4x4& invMat = m_View->GetInverseProjectionMatrix(bIncludeOffset);
                        memcpy(rtTaskCommon.clipToViewMatrix.f, invMat.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::affine3& invAf3 = m_View->GetInverseViewMatrix();
                        const dm::float4x4 m = math::affineToHomogeneous(invAf3);
                        memcpy(rtTaskCommon.viewToWorldMatrix.f, m.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::affine3& af3 = m_View->GetViewMatrix();
                        const dm::float4x4 m = math::affineToHomogeneous(af3);
                        memcpy(rtTaskCommon.worldToViewMatrix.f, m.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::float4x4& mat = m_View->GetProjectionMatrix(bIncludeOffset);
                        memcpy(rtTaskCommon.viewToClipMatrix.f, mat.m_data, sizeof(float) * 16);
                    }

                    rtTaskCommon.maxRayLength = m_ui.KS.m_maxRayLength;

                    if (m_ui.KS.m_rayOffsetType == 1) {
                        rtTaskCommon.rayOffset.type = SDK::VK::RenderTask::RayOffset::Type::e_WorldPosition;
                        rtTaskCommon.rayOffset.worldPosition.threshold = m_ui.KS.m_rayOffset_WorldPosition_threshold;
                        rtTaskCommon.rayOffset.worldPosition.floatScale = m_ui.KS.m_rayOffset_WorldPosition_floatScale;
                        rtTaskCommon.rayOffset.worldPosition.intScale = m_ui.KS.m_rayOffset_WorldPosition_intScale;
                    }
                    else if (m_ui.KS.m_rayOffsetType == 2) {
                        rtTaskCommon.rayOffset.type = SDK::VK::RenderTask::RayOffset::Type::e_CamDistance;
                        rtTaskCommon.rayOffset.camDistance.constant = m_ui.KS.m_rayOffset_CamDistance_constant;
                        rtTaskCommon.rayOffset.camDistance.linear = m_ui.KS.m_rayOffset_CamDistance_linear;
                        rtTaskCommon.rayOffset.camDistance.quadratic = m_ui.KS.m_rayOffset_CamDistance_quadratic;
                    }

                    if (m_ui.KS.m_debugDisp != 0) {
                        SDK::VK::RenderTask::TraceSpecularTask rtTask;
                        rtTask.common.useInlineRT = m_ui.KS.m_useTraceRayInline;
                        rtTask.common = rtTaskCommon;
                        rtTask.debugParameters.debugOutputType = (SDK::VK::RenderTask::DebugParameters::DebugOutputType)m_ui.KS.m_debugDisp;
                        rtTask.out = GetUnorderedAccessTexVK(GBufferRTReflections[Layer::Opaque]);

                        sts = tcVK->ScheduleRenderTask(&rtTask);
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                        }
                    }
                    else {
                        if (m_ui.KS.m_enableReflection) {
                            SDK::VK::RenderTask::TraceSpecularTask rtTask;
                            rtTask.common.useInlineRT = m_ui.KS.m_useTraceRayInline;
                            rtTask.common = rtTaskCommon;
                            rtTask.out = GetUnorderedAccessTexVK(GBufferRTReflections[Layer::Opaque]);

                            sts = tcVK->ScheduleRenderTask(&rtTask);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                            }
                        }
                        if (m_ui.KS.m_enableGI) {
                            SDK::VK::RenderTask::TraceDiffuseTask rtTask;
                            rtTask.common = rtTaskCommon;
                            rtTask.diffuseBRDFType = SDK::VK::RenderTask::DiffuseBRDFType::NormalizedDisney;
                            rtTask.out = GetUnorderedAccessTexVK(GBufferRTGI[Layer::Opaque]);

                            sts = tcVK->ScheduleRenderTask(&rtTask);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                            }
                        }
                        if (m_ui.KS.m_enableAO) {
                            SDK::VK::RenderTask::TraceAmbientOcclusionTask rtTask;
                            rtTask.common = rtTaskCommon;
                            rtTask.out = GetUnorderedAccessTexVK(GBufferRTAO[Layer::Opaque]);

                            sts = tc_pre_VK->ScheduleRenderTask(&rtTask);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                            }
                        }

                        if (m_ui.KS.m_enableShadows) {

                            if (m_ui.KS.m_enableShadows == 1) {
                                SDK::VK::RenderTask::TraceShadowTask rtTask;
                                rtTask.common = rtTaskCommon;
                                rtTask.common.halfResolutionMode = SDK::VK::RenderTask::HalfResolutionMode::OFF;
                                rtTask.enableFirstHitAndEndSearch = m_ui.KS.m_shadowsEnableFirstHitAndEndSearch;
                                SetupLightInfosVK(&rtTask.lightInfo, 1);
                                rtTask.out = GetUnorderedAccessTexVK(GBufferRTShadows[Layer::Opaque]);

                                sts = tc_pre_VK->ScheduleRenderTask(&rtTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                            else {
                                SDK::VK::RenderTask::TraceMultiShadowTask rtTask;
                                rtTask.common                       = rtTaskCommon;
                                rtTask.common.halfResolutionMode    = SDK::VK::RenderTask::HalfResolutionMode::OFF;
                                rtTask.enableFirstHitAndEndSearch   = m_ui.KS.m_shadowsEnableFirstHitAndEndSearch;
                                rtTask.numLights                    = SetupLightInfosVK(rtTask.lightInfos, SDK::VK::RenderTask::TraceMultiShadowTask::kMaxLightNum);
                                rtTask.out0                         = GetUnorderedAccessTexVK(GBufferRTShadows[Layer::Opaque]);
                                rtTask.out1                         = GetUnorderedAccessTexVK(m_RenderTargets[Layer::Opaque]->GBufferRTShadowsAux);

                                sts = tc_pre_VK->ScheduleRenderTask(&rtTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                        }
                    }

                    if (enableDenoising)
                    {
                        SDK::VK::RenderTask::DenoisingTaskCommon dTaskCommon;

                        dTaskCommon.mode = m_ui.KS.m_denoisingReset ? SDK::VK::RenderTask::DenoisingTaskCommon::Mode::DiscardHistory : SDK::VK::RenderTask::DenoisingTaskCommon::Mode::Continue;

                        if (enableCheckerboard) {
                            dTaskCommon.halfResolutionMode = GetFrameIndex() % 2 == 0 ? SDK::VK::RenderTask::HalfResolutionMode::CHECKERBOARD : SDK::VK::RenderTask::HalfResolutionMode::CHECKERBOARD_INVERTED;
                        }

                        dTaskCommon.viewport = rtTaskCommon.viewport;
                        dTaskCommon.depth = rtTaskCommon.depth;
                        dTaskCommon.normal = rtTaskCommon.normal;
                        dTaskCommon.roughness = rtTaskCommon.roughness;
                        {
                            auto& desc = m_RenderTargets[Layer::Opaque]->MotionVectors->getDesc();
                            dTaskCommon.motion.tex = GetShaderResourceTexVK(m_RenderTargets[Layer::Opaque]->MotionVectors);
                            dTaskCommon.motion.type = SDK::VK::RenderTask::MotionType::RG_ViewSpace;
                            dTaskCommon.motion.scale.f[0] = 1.f / desc.width;
                            dTaskCommon.motion.scale.f[1] = 1.f / desc.height;
                        }
                        const bool bIncludeOffset = false;
                        {
                            const dm::float4x4& invMat = m_View->GetInverseProjectionMatrix(bIncludeOffset);
                            memcpy(dTaskCommon.clipToViewMatrix.f, invMat.m_data, sizeof(float) * 16);
                        }
                        {
                            const dm::float4x4& mat = m_View->GetProjectionMatrix(bIncludeOffset);
                            memcpy(dTaskCommon.viewToClipMatrix.f, mat.m_data, sizeof(float) * 16);
                        }
                        {
                            const dm::float4x4& mat = m_ViewPrevious->GetProjectionMatrix(bIncludeOffset);
                            memcpy(dTaskCommon.viewToClipMatrixPrev.f, mat.m_data, sizeof(float) * 16);
                        }
                        {
                            const dm::affine3& af3 = m_View->GetViewMatrix();
                            const dm::float4x4 m = math::affineToHomogeneous(af3);
                            memcpy(dTaskCommon.worldToViewMatrix.f, m.m_data, sizeof(float) * 16);
                        }
                        {
                            const dm::affine3& af3 = m_ViewPrevious->GetViewMatrix();
                            const dm::float4x4 m = math::affineToHomogeneous(af3);
                            memcpy(dTaskCommon.worldToViewMatrixPrev.f, m.m_data, sizeof(float) * 16);
                        }
                        {
                            dTaskCommon.cameraJitter.f[0] = m_View->GetPixelOffset().x;
                            dTaskCommon.cameraJitter.f[1] = m_View->GetPixelOffset().y;
                        }

                        if (enableReflectionDenoising)
                        {
                            if (m_ui.KS.m_enableReflection && m_ui.KS.m_enableGI) {
                                SDK::VK::RenderTask::DenoiseSpecularAndDiffuseTask dTask;
                                dTask.common = dTaskCommon;
                                dTask.context = m_SDKContext.m_denosingContext.specDiff.m_VK;

                                dTask.inSpecular        = GetShaderResourceTexVK(GBufferRTReflections[Layer::Opaque]);
                                dTask.inOutSpecular     = GetCombinedAccessTexVK(m_RenderTargets[Layer::Opaque]->GBufferRTReflectionsFinal);
                                dTask.inDiffuse         = GetShaderResourceTexVK(GBufferRTGI[Layer::Opaque]);
                                dTask.inOutDiffuse      = GetCombinedAccessTexVK(m_RenderTargets[Layer::Opaque]->GBufferRTGIFinal);

                                sts = tcVK->ScheduleRenderTask(&dTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                            else if (m_ui.KS.m_enableReflection) {
                                SDK::VK::RenderTask::DenoiseSpecularTask dTask;
                                dTask.common        = dTaskCommon;
                                dTask.context       = m_SDKContext.m_denosingContext.specDiff.m_VK;
                                dTask.inSpecular    = GetShaderResourceTexVK(GBufferRTReflections[Layer::Opaque]);
                                dTask.inOutSpecular = GetCombinedAccessTexVK(m_RenderTargets[Layer::Opaque]->GBufferRTReflectionsFinal);

                                sts = tcVK->ScheduleRenderTask(&dTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                            else if (m_ui.KS.m_enableGI) {
                                SDK::VK::RenderTask::DenoiseDiffuseTask dTask;
                                dTask.common = dTaskCommon;
                                dTask.context = m_SDKContext.m_denosingContext.specDiff.m_VK;

                                dTask.inDiffuse = GetShaderResourceTexVK(GBufferRTGI[Layer::Opaque]);
                                dTask.inOutDiffuse = GetCombinedAccessTexVK(m_RenderTargets[Layer::Opaque]->GBufferRTGIFinal);

                                sts = tcVK->ScheduleRenderTask(&dTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                        }

                        if (enableAODenoising)
                        {
                            SDK::VK::RenderTask::DenoiseDiffuseOcclusionTask dTask;
                            dTask.context = m_SDKContext.m_denosingContext.ao.m_VK;
                            dTask.common = dTaskCommon;
                            dTask.inHitT = GetShaderResourceTexVK(GBufferRTAO[Layer::Opaque]);
                            dTask.inOutOcclusion = GetCombinedAccessTexVK(m_RenderTargets[Layer::Opaque]->GBufferRTAOFinal);

                            sts = tc_pre_VK->ScheduleRenderTask(&dTask);
                            if (sts != SDK::Status::OK) {
                                log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                            }
                        }

                        if (enableShadowDenoising)
                        {
                            if (m_ui.KS.m_enableShadows == 2)
                            {
                                SDK::VK::RenderTask::DenoiseMultiShadowTask dTask;
                                dTask.common        = dTaskCommon;
                                dTask.common.halfResolutionMode = SDK::VK::RenderTask::HalfResolutionMode::OFF;
                                dTask.context       = m_SDKContext.m_denosingContext.shadow.m_VK;
                                dTask.inShadow0     = GetShaderResourceTexVK(GBufferRTShadows[Layer::Opaque]);
                                dTask.inShadow1     = GetShaderResourceTexVK(m_RenderTargets[Layer::Opaque]->GBufferRTShadowsAux);
                                dTask.inOutShadow   = GetCombinedAccessTexVK(m_RenderTargets[Layer::Opaque]->GBufferRTShadowsFinal);

                                sts = tc_pre_VK->ScheduleRenderTask(&dTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }
                            else {
                                SDK::VK::RenderTask::DenoiseShadowTask dTask;
                                dTask.common        = dTaskCommon;
                                dTask.common.halfResolutionMode = SDK::VK::RenderTask::HalfResolutionMode::OFF;
                                dTask.context       = m_SDKContext.m_denosingContext.shadow.m_VK;
                                dTask.inShadow      = GetShaderResourceTexVK(GBufferRTShadows[Layer::Opaque]);
                                dTask.inOutShadow   = GetCombinedAccessTexVK(m_RenderTargets[Layer::Opaque]->GBufferRTShadowsFinal);

                                sts = tc_pre_VK->ScheduleRenderTask(&dTask);
                                if (sts != SDK::Status::OK) {
                                    log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                                }
                            }

                        }
                    }
                }

                // Transparent reflection view
                if (m_ui.KS.m_enableTransparentReflection)
                {
                    SDK::VK::RenderTask::TraceSpecularTask rtTask;
                    rtTask.common.useInlineRT = m_ui.KS.m_useTraceRayInline;

                    if (m_ui.KS.m_enableDirectLightingSample) {
                        rtTask.common.directLighting = GetShaderResourceTexVK(m_RenderTargets[Layer::Opaque]->HdrColor);
                    }

                    rtTask.common.depth.tex = GetShaderResourceTexVK(m_RenderTargets[Layer::Transparent0]->GBufferWorldPosition);
                    rtTask.common.depth.type = SDK::VK::RenderTask::DepthType::RGB_WorldSpace;

                    rtTask.common.normal.tex = GetShaderResourceTexVK(m_RenderTargets[Layer::Transparent0]->GBufferNormals);
                    rtTask.common.normal.type = SDK::VK::RenderTask::NormalType::RGB_Vector;

                    rtTask.common.roughness.globalRoughness = 0.f;
                    rtTask.common.specular.globalMetalness = 1.0;

                    {
                        float2 renderTargetSize = float2(m_RenderTargets[Layer::Transparent0]->GetSize());

                        rtTask.common.viewport.topLeftX = 0;
                        rtTask.common.viewport.topLeftY = 0;
                        rtTask.common.viewport.width = (uint32_t)renderTargetSize.x;
                        rtTask.common.viewport.height = (uint32_t)renderTargetSize.y;
                        rtTask.common.viewport.minDepth = 0.0;
                        rtTask.common.viewport.maxDepth = 1.0;
                    }
                    {
                        const auto& invMat = m_View->GetInverseProjectionMatrix();
                        memcpy(rtTask.common.clipToViewMatrix.f, invMat.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::affine3& invAf3 = m_View->GetInverseViewMatrix();
                        const dm::float4x4 m = math::affineToHomogeneous(invAf3);
                        memcpy(rtTask.common.viewToWorldMatrix.f, m.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::affine3& af3 = m_View->GetViewMatrix();
                        const dm::float4x4 m = math::affineToHomogeneous(af3);
                        memcpy(rtTask.common.worldToViewMatrix.f, m.m_data, sizeof(float) * 16);
                    }
                    {
                        const dm::float4x4& mat = m_View->GetProjectionMatrix();
                        memcpy(rtTask.common.viewToClipMatrix.f, mat.m_data, sizeof(float) * 16);
                    }

                    rtTask.common.maxRayLength = m_ui.KS.m_maxRayLength;

                    if (m_ui.KS.m_rayOffsetType == 1) {
                        rtTask.common.rayOffset.type = SDK::VK::RenderTask::RayOffset::Type::e_WorldPosition;
                        rtTask.common.rayOffset.worldPosition.threshold = m_ui.KS.m_rayOffset_WorldPosition_threshold;
                        rtTask.common.rayOffset.worldPosition.floatScale = m_ui.KS.m_rayOffset_WorldPosition_floatScale;
                        rtTask.common.rayOffset.worldPosition.intScale = m_ui.KS.m_rayOffset_WorldPosition_intScale;
                    }
                    else if (m_ui.KS.m_rayOffsetType == 2) {
                        rtTask.common.rayOffset.type = SDK::VK::RenderTask::RayOffset::Type::e_CamDistance;
                        rtTask.common.rayOffset.camDistance.constant = m_ui.KS.m_rayOffset_CamDistance_constant;
                        rtTask.common.rayOffset.camDistance.linear = m_ui.KS.m_rayOffset_CamDistance_linear;
                        rtTask.common.rayOffset.camDistance.quadratic = m_ui.KS.m_rayOffset_CamDistance_quadratic;
                    }

                    rtTask.out = GetUnorderedAccessTexVK(m_RenderTargets[Layer::Transparent0]->GBufferRTReflections);

                    sts = tcVK->ScheduleRenderTask(&rtTask);
                    if (sts != SDK::Status::OK) {
                        log::fatal("KickStartRTX: ScheduleRenderTask() failed. : %d", (uint32_t)sts);
                    }
                }
            }
#endif

#ifdef KickstartRT_Demo_WITH_D3D11
            // D3D11 need to insert render command in-between of main command list.
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
            if (m_SDKContext.m_12) {

                // Finish tasks that are done by now.
                for (auto it = m_SDKContext.m_12->m_tasksInFlight.begin(); it != m_SDKContext.m_12->m_tasksInFlight.end();) {
                    if (GetFrameIndex() - it->second >= KickstartRT_SDK_Context::D3D12::kMaxRenderAheadFrames) {
                        sts = m_SDKContext.m_12->m_executeContext->MarkGPUTaskAsCompleted(it->first);
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: FinishGPUTask() failed. : %d", (uint32_t)sts);
                        }
                        it = m_SDKContext.m_12->m_tasksInFlight.erase(it);
                    }
                    else {
                        ++it;
                    }
                }

                auto RecordCommandList = [](KickstartRT_SDK_Context* context, SDK::D3D12::TaskContainer* tc, nvrhi::CommandListHandle commandList, uint frameIndex) {
                    SDK::D3D12::BuildGPUTaskInput input = {};
                    input.commandList = reinterpret_cast<ID3D12CommandList*>(commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer);
                    input.geometryTaskFirst = true;

                    SDK::D3D12::GPUTaskHandle retHandle;
                    SDK::Status sts = context->m_12->m_executeContext->BuildGPUTask(&retHandle, tc, &input);
                    if (sts != SDK::Status::OK) {
                        log::fatal("KickStartRTX: BuildGPUTask() failed. : %d", (uint32_t)sts);
                    }

                    context->m_12->m_tasksInFlight.push_back(std::make_pair(retHandle, frameIndex));
                };

                RecordCommandList(&m_SDKContext, tc_pre_12, m_CommandListKS_PreLighting, GetFrameIndex());
                RecordCommandList(&m_SDKContext, tc12, m_CommandListKS, GetFrameIndex());
                RecordCommandList(&m_SDKContext, tc_post_12, m_CommandListKS_Post, GetFrameIndex());

                tc_pre_12 = tc12 = tc_post_12 = nullptr;
            }
#endif
#ifdef KickstartRT_Demo_WITH_VK
            if (m_SDKContext.m_vk) {
                
                // Finish tasks that are done by now.
                for (auto it = m_SDKContext.m_vk->m_tasksInFlight.begin(); it != m_SDKContext.m_vk->m_tasksInFlight.end();) {
                    if (GetFrameIndex() - it->second >= KickstartRT_SDK_Context::VK::kMaxRenderAheadFrames) {
                        sts = m_SDKContext.m_vk->m_executeContext->MarkGPUTaskAsCompleted(it->first);
                        if (sts != SDK::Status::OK) {
                            log::fatal("KickStartRTX: FinishGPUTask() failed. : %d", (uint32_t)sts);
                        }
                        it = m_SDKContext.m_vk->m_tasksInFlight.erase(it);
                    }
                    else {
                        ++it;
                    }
                }

                auto RecordCommandBuffer = [](KickstartRT_SDK_Context* context, SDK::VK::TaskContainer* tc, nvrhi::CommandListHandle commandList, uint frameIndex) {
                    SDK::VK::BuildGPUTaskInput input = {};

                    input.commandBuffer = reinterpret_cast<VkCommandBuffer>(commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer).pointer);

                    SDK::VK::GPUTaskHandle retHandle;
                    SDK::Status sts = context->m_vk->m_executeContext->BuildGPUTask(&retHandle, tc, &input);
                    if (sts != SDK::Status::OK) {
                        log::fatal("KickStartRTX: BuildGPUTask() failed. : %d", (uint32_t)sts);
                    }

                    context->m_vk->m_tasksInFlight.push_back(std::make_pair(retHandle, frameIndex));
                };

                RecordCommandBuffer(&m_SDKContext, tc_pre_VK, m_CommandListKS_PreLighting, GetFrameIndex());
                RecordCommandBuffer(&m_SDKContext, tcVK, m_CommandListKS, GetFrameIndex());
                RecordCommandBuffer(&m_SDKContext, tc_post_VK, m_CommandListKS_Post, GetFrameIndex());

                tc_pre_VK = tcVK = tc_post_VK = nullptr;
            }
#endif

            // Export Shader Cold Load List if needed.
            if (m_ui.KS.m_ExportShaderColdLoadListFileName.length() > 0) {
#ifdef KickstartRT_Demo_WITH_D3D11
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                std::array<uint32_t, 256>       coldListBuf;
                size_t                          retListLen = 0;
                sts = m_SDKContext.m_12->m_executeContext->GetLoadedShaderList(coldListBuf.data(), coldListBuf.size(), &retListLen);
                if (sts != SDK::Status::OK) {
                    log::fatal("KickStartRTX: Failed to get shader hot list. : %d", (uint32_t)sts);
                }
                else if (retListLen > 0) {
                    std::ofstream ofs(m_ui.KS.m_ExportShaderColdLoadListFileName.c_str(), std::ios::binary | std::ios::out);
                    ofs.write(reinterpret_cast<char*>(coldListBuf.data()), sizeof(coldListBuf[0]) * retListLen);
                }

#endif
#ifdef KickstartRT_Demo_WITH_VK
#endif

                m_ui.KS.m_ExportShaderColdLoadListFileName.clear();
            }
        }
#endif
    }

    virtual void RenderScene(nvrhi::IFramebuffer* framebuffer) override
    {
        int windowWidth, windowHeight;
        GetDeviceManager()->GetWindowDimensions(windowWidth, windowHeight);
        nvrhi::Viewport windowViewport = nvrhi::Viewport(float(windowWidth), float(windowHeight));
        nvrhi::Viewport renderViewport = windowViewport;

        m_Scene->RefreshSceneGraph(GetFrameIndex());

        bool exposureResetRequired = false;

        {
            uint width = windowWidth;
            uint height = windowHeight;

            uint sampleCount = 1;
            switch (m_ui.AntiAliasingMode)
            {
            case AntiAliasingMode::MSAA_2X: sampleCount = 2; break;
            case AntiAliasingMode::MSAA_4X: sampleCount = 4; break;
            case AntiAliasingMode::MSAA_8X: sampleCount = 8; break;
            default:;
            }

            bool needNewPasses = false;

            const bool reverseDepth = false;

            for (int i = 0; i < Layer::Count; ++i) {
                if (!m_RenderTargets[i] || m_RenderTargets[i]->IsUpdateRequired(uint2(width, height), sampleCount))
                {
                    bool sharedAcrossDevice = false;
#if defined(ENABLE_KickStartSDK)
#ifdef KickstartRT_Demo_WITH_D3D11
                    if (m_SDKContext.m_11) {
                        sharedAcrossDevice = true;
                    }
#endif
#endif

                    m_RenderTargets[i] = nullptr;
                    m_BindingCache.Clear();
                    m_RenderTargets[i] = std::make_unique<RenderTargets>();
                    m_RenderTargets[i]->Init(GetDevice(), uint2(width, height), sampleCount, true, reverseDepth, sharedAcrossDevice);

                    needNewPasses = true;
                }
            }

            if (SetupView(reverseDepth))
            {
                needNewPasses = true;
            }

            if (m_ui.ShaderReoladRequested)
            {
                m_ShaderFactory->ClearCache();
                needNewPasses = true;
            }

            if (needNewPasses)
            {
                CreateRenderPasses(exposureResetRequired);
            }

            m_ui.ShaderReoladRequested = false;
        }

        // Record KS tasks.
#if defined(ENABLE_KickStartSDK)
        if (GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12 ||
            GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
        {
            // Commands from KickStart SDK need to be run before executing the command list built after RTR render pass.
            // for 12 and vk
            m_CommandListKS_PreLighting->open();
            m_CommandListKS->open();
            m_CommandListKS_Post->open();

#ifdef KickstartRT_Demo_WITH_D3D12
            if (m_SDKContext.m_12) {
                auto nativeCL = reinterpret_cast<ID3D12CommandList*>(m_CommandListKS_PreLighting->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer);
                nativeCL->SetName(L"KS_PreCL");
                nativeCL = reinterpret_cast<ID3D12CommandList*>(m_CommandListKS->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer);
                nativeCL->SetName(L"KS_CL");
                nativeCL = reinterpret_cast<ID3D12CommandList*>(m_CommandListKS_Post->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer);
                nativeCL->SetName(L"KS_PostCL");
            }
#endif
            PrepareRenderKS_PreLighting(m_CommandListKS_PreLighting);
            PrepareRenderRTReflections(m_CommandListKS);
            PrepareRenderKS_PostLighting(m_CommandListKS_Post);
            RenderRTReflections();
        }
        else if (GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11) {
            // build TaskContainer 
            RenderRTReflections();
        }

#ifdef KickstartRT_Demo_WITH_D3D11
        auto RecordCommandList11 = [](ID3D11DeviceContext4* c4, KickstartRT_SDK_Context::D3D11* _11, SDK::D3D11::TaskContainer* tc) {
            c4->Signal(_11->m_interopFence.Get(), ++_11->m_interopFenceValue);
            {
                SDK::D3D11::BuildGPUTaskInput input;

                input.waitFence = _11->m_interopFence.Get();
                input.waitFenceValue = _11->m_interopFenceValue;
                input.signalFence = _11->m_interopFence.Get();
                input.signalFenceValue = ++_11->m_interopFenceValue;

                SDK::D3D11::ExecuteContext* exc11 = _11->m_executeContext;
                auto sts = exc11->InvokeGPUTask(tc, &input);
                if (sts != SDK::Status::OK) {
                    log::fatal("KickStartRTX: InvokeGPUTask() failed. : %d", (uint32_t)sts);
                }
            }
            c4->Wait(_11->m_interopFence.Get(), _11->m_interopFenceValue);
        };
#endif

#endif

        m_CommandList->open();

        {
            bool sharedAcrossDevice = false;
#if defined(ENABLE_KickStartSDK)
#ifdef KickstartRT_Demo_WITH_D3D11
            if (m_SDKContext.m_11) {
                sharedAcrossDevice = true;
            }
#endif
#endif
            m_Scene->RefreshBuffers(m_CommandList, GetFrameIndex(), sharedAcrossDevice);
        }

        nvrhi::ITexture* framebufferTexture = framebuffer->getDesc().colorAttachments[0].texture;
        m_CommandList->clearTextureFloat(framebufferTexture, nvrhi::AllSubresources, nvrhi::Color(0.f));

        m_AmbientTop = m_ui.AmbientIntensity * m_ui.SkyParams.skyColor * m_ui.SkyParams.brightness;
        m_AmbientBottom = m_ui.AmbientIntensity * m_ui.SkyParams.groundColor * m_ui.SkyParams.brightness;
        if (m_ui.EnableShadows)
        {
            m_SunLight->shadowMap = m_ShadowMap;
            box3 sceneBounds = m_Scene->GetSceneGraph()->GetRootNode()->GetGlobalBoundingBox();

            frustum projectionFrustum = m_View->GetProjectionFrustum();
            const float maxShadowDistance = 100.f;

            dm::affine3 viewMatrixInv = m_View->GetChildView(ViewType::PLANAR, 0)->GetInverseViewMatrix();

            float zRange = length(sceneBounds.diagonal()) * 0.5f;
            m_ShadowMap->SetupForPlanarViewStable(*m_SunLight, projectionFrustum, viewMatrixInv, maxShadowDistance, zRange, zRange, m_ui.CsmExponent);

            m_ShadowMap->Clear(m_CommandList);

            DepthPass::Context context;

            RenderCompositeView(m_CommandList,
                &m_ShadowMap->GetView(), nullptr,
                *m_ShadowFramebuffer,
                m_Scene->GetSceneGraph()->GetRootNode(),
                *m_OpaqueDrawStrategy,
                *m_ShadowDepthPass,
                context,
                "ShadowMap",
                m_ui.EnableMaterialEvents);
        }
        else
        {
            m_SunLight->shadowMap = nullptr;
        }

        std::vector<std::shared_ptr<LightProbe>> lightProbes;
        if (m_ui.EnableLightProbe)
        {
            for (auto probe : m_LightProbes)
            {
                if (probe->enabled)
                {
                    probe->diffuseScale = m_ui.LightProbeDiffuseScale;
                    probe->specularScale = m_ui.LightProbeSpecularScale;
                    lightProbes.push_back(probe);
                }
            }
        }

        for (int i = 0; i < Layer::Count; ++i)
            m_RenderTargets[i]->Clear(m_CommandList);

        if (exposureResetRequired)
            m_ToneMappingPass->ResetExposure(m_CommandList, 0.5f);

        ForwardShadingPass::Context forwardContext;

        if (!m_ui.UseDeferredShading || m_ui.EnableTranslucency)
        {
            m_ForwardPass->PrepareLights(forwardContext, m_CommandList, m_Scene->GetSceneGraph()->GetLights(), m_AmbientTop, m_AmbientBottom, lightProbes, nullptr, nullptr);
        }

        assert(m_ui.UseDeferredShading);
        //if (m_ui.UseDeferredShading)
        {
            GBufferFillPass::Context gbufferContext;

            RenderCompositeView(m_CommandList,
                m_View.get(), m_ViewPrevious.get(),
                *m_RenderTargets[Layer::Opaque]->GBufferFramebuffer,
                m_Scene->GetSceneGraph()->GetRootNode(),
                *m_OpaqueDrawStrategy,
                *m_GBufferPass[Layer::Opaque],
                gbufferContext,
                "GBufferFill",
                m_ui.EnableMaterialEvents);

            nvrhi::ITexture* ambientOcclusionTarget = nullptr;
            if (m_ui.EnableSsao && m_SsaoPass)
            {
                m_SsaoPass->Render(m_CommandList, m_ui.SsaoParams, *m_View);
                ambientOcclusionTarget = m_RenderTargets[Layer::Opaque]->AmbientOcclusion;
            }

            m_CommandList->close();
            GetDevice()->executeCommandList(m_CommandList);

#if defined(ENABLE_KickStartSDK)
            // 
            if (GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12 ||
                GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
            {
                m_CommandListKS_PreLighting->close();
                GetDevice()->executeCommandList(m_CommandListKS_PreLighting);
            }
            else if (GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11) {
#ifdef KickstartRT_Demo_WITH_D3D11
                // Insert fence and execute CommandList in 12 layer.
                nvrhi::RefCountPtr<ID3D11DeviceContext4>    cntxt4;
                {
                    ID3D11DeviceContext* cntxt = reinterpret_cast<ID3D11DeviceContext*>(GetDevice()->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext).pointer);
                    cntxt->QueryInterface(IID_PPV_ARGS(&cntxt4));
                }
                RecordCommandList11(cntxt4.Get(), m_SDKContext.m_11.get(), m_SDKContext.m_tc_preLighting.m_11);
                m_SDKContext.m_tc_preLighting.m_11 = nullptr;
#endif
            }
#endif

            m_CommandList->open();

            DeferredLightingPass::Inputs deferredInputs;
            deferredInputs.SetGBuffer(*m_RenderTargets[Layer::Opaque]);
            deferredInputs.ambientOcclusion = m_ui.EnableSsao ? m_RenderTargets[Layer::Opaque]->AmbientOcclusion : nullptr;
            deferredInputs.ambientColorTop = m_AmbientTop;
            deferredInputs.ambientColorBottom = m_AmbientBottom;
#if defined(ENABLE_KickStartSDK)
            deferredInputs.rtShadow = m_ui.KS.m_enableShadows != 0 ? m_RenderTargets[Layer::Opaque]->GBufferRTShadowsFinal : nullptr;
            deferredInputs.rtAmbientOcclusion = m_ui.KS.m_enableAO ? m_RenderTargets[Layer::Opaque]->GBufferRTAOFinal : nullptr;
#else
            deferredInputs.rtShadow = nullptr;
            deferredInputs.rtAmbientOcclusion = nullptr;
#endif
            deferredInputs.lights = &m_Scene->GetSceneGraph()->GetLights();
            deferredInputs.lightProbes = m_ui.EnableLightProbe ? &m_LightProbes : nullptr;
            deferredInputs.output = m_RenderTargets[Layer::Opaque]->HdrColor;

            m_DeferredLightingPass->Render(m_CommandList, *m_View, deferredInputs);
        }
        //else
        //{
        //    RenderCompositeView(m_CommandList,
        //        m_View.get(), m_ViewPrevious.get(),
        //        *m_RenderTargets[Layer::Opaque]->ForwardFramebuffer,
        //        m_Scene->GetSceneGraph()->GetRootNode(),
        //        *m_OpaqueDrawStrategy,
        //        *m_ForwardPass,
        //        forwardContext,
        //        "ForwardOpaque",
        //        m_ui.EnableMaterialEvents);
        //}

#if defined(ENABLE_KickStartSDK)
        if (m_ui.EnableTranslucency && m_ui.KS.m_enableTransparentReflection)
#else
        if (m_ui.EnableTranslucency)
#endif
        {
            // Generate Transparent Gbuffer
            GBufferFillPass::Context gbufferContext;

            RenderCompositeView(m_CommandList,
                m_View.get(), m_ViewPrevious.get(),
                *m_RenderTargets[Layer::Transparent0]->GBufferFramebuffer,
                m_Scene->GetSceneGraph()->GetRootNode(),
                *m_TransparentDrawStrategy,
                *m_GBufferPass[Layer::Transparent0],
                gbufferContext,
                "TransparentGBufferFill",
                m_ui.EnableMaterialEvents);
        }

#if defined(ENABLE_KickStartSDK)
        if (m_ui.AntiAliasingMode == AntiAliasingMode::TEMPORAL || m_ui.KS.m_denoisingMethod != 0)
#else
        if (m_ui.AntiAliasingMode == AntiAliasingMode::TEMPORAL)
#endif
        {
            // if (m_PreviousViewsValid)
            {
                m_TemporalAntiAliasingPass->RenderMotionVectors(m_CommandList, *m_View, *m_ViewPrevious);
            }
        }

#if defined(ENABLE_KickStartSDK)
        {

            m_CommandList->close();
            GetDevice()->executeCommandList(m_CommandList);

#if 0
            if (GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11)
            {
                RenderRTReflections();
            }
#endif

#if defined(ENABLE_KickStartSDK)
            if (GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12 ||
                GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
            {
                m_CommandListKS->close();
                GetDevice()->executeCommandList(m_CommandListKS);
            }
            else if (GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11) {
#ifdef KickstartRT_Demo_WITH_D3D11
                // Insert fence and execute CommandList in 12 layer.
                nvrhi::RefCountPtr<ID3D11DeviceContext4>    cntxt4;
                {
                    ID3D11DeviceContext* cntxt = reinterpret_cast<ID3D11DeviceContext*>(GetDevice()->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext).pointer);
                    cntxt->QueryInterface(IID_PPV_ARGS(&cntxt4));
                }
                RecordCommandList11(cntxt4.Get(), m_SDKContext.m_11.get(), m_SDKContext.m_tc.m_11);
                m_SDKContext.m_tc.m_11 = nullptr;
#endif
            }
#endif

            m_CommandList->open();


#if 0
            {
                const auto& desc = m_RenderTargets[Layer::Opaque]->HdrFramebuffer->RenderTargets[0]->getDesc();
                nvrhi::Viewport viewport = nvrhi::Viewport(0, (float)desc.width, 0, (float)desc.height, 0, 1);

                engine::BlitParameters blitParams;
                blitParams.targetFramebuffer = m_RenderTargets[Layer::Opaque]->HdrFramebuffer->GetFramebuffer(nvrhi::TextureSubresourceSet());
                blitParams.targetViewport = viewport;
                blitParams.sourceTexture = GBufferRTReflections[Layer::Opaque];
                blitParams.sourceArraySlice = 0;

                blitParams.blendState = {
                    true, //bool        blendEnable = false;
                    nvrhi::BlendFactor::One, // srcBlend = BlendFactor::One;
                    nvrhi::BlendFactor::One, //BlendFactor destBlend = BlendFactor::Zero;
                    nvrhi::BlendOp::Add, //    BlendOp     blendOp = BlendOp::Add;
                    nvrhi::BlendFactor::One, //BlendFactor srcBlendAlpha = BlendFactor::One;
                    nvrhi::BlendFactor::Zero, //BlendFactor destBlendAlpha = BlendFactor::Zero;
                    nvrhi::BlendOp::Add, //BlendOp     blendOpAlpha = BlendOp::Add;
                    nvrhi::ColorMask::All //ColorMask   colorWriteMask = ColorMask::All;
                };

                m_CommonPasses->BlitTexture(m_CommandList, blitParams);
            }
#else
            m_SDKComposite->Render(
                GetDevice(),
                m_CommandList,
                m_RenderTargets[Layer::Opaque]->HdrFramebuffer,
                m_RenderTargets[Layer::Opaque]->GBufferDiffuse,
                m_ui.KS.m_enableReflection ? m_RenderTargets[Layer::Opaque]->GBufferRTReflectionsFinal : nullptr,
                m_ui.KS.m_enableGI ? m_RenderTargets[Layer::Opaque]->GBufferRTGIFinal : nullptr,
                m_ui.KS.m_enableAO ? m_RenderTargets[Layer::Opaque]->GBufferRTAOFinal : nullptr,
                m_ui.KS.m_enableShadows != 0 ? m_RenderTargets[Layer::Opaque]->GBufferRTShadowsFinal : nullptr,
                m_ui.KS.m_debugDisp != 0 ? true : false,
                m_ui.KS.m_denoisingMethod == 1 ? true : false); // REBLUR uses YCoCg color space from NRD v3.7.
#endif

        }
#endif

        if (m_Pick)
        {
            m_CommandList->clearTextureUInt(m_RenderTargets[Layer::Opaque]->MaterialIDs, nvrhi::AllSubresources, 0xffff);

            MaterialIDPass::Context materialIdContext;

            RenderCompositeView(m_CommandList,
                m_View.get(), m_ViewPrevious.get(),
                *m_RenderTargets[Layer::Opaque]->MaterialIDFramebuffer,
                m_Scene->GetSceneGraph()->GetRootNode(),
                *m_OpaqueDrawStrategy,
                *m_MaterialIDPass,
                materialIdContext,
                "MaterialID");

            if (m_ui.EnableTranslucency)
            {
                RenderCompositeView(m_CommandList,
                    m_View.get(), m_ViewPrevious.get(),
                    *m_RenderTargets[Layer::Opaque]->MaterialIDFramebuffer,
                    m_Scene->GetSceneGraph()->GetRootNode(),
                    *m_TransparentDrawStrategy,
                    *m_MaterialIDPass,
                    materialIdContext,
                    "MaterialID - Translucent");
            }

            m_PixelReadbackPass->Capture(m_CommandList, m_PickPosition);
        }

        if (m_ui.EnableProceduralSky)
            m_SkyPass->Render(m_CommandList, *m_View, *m_SunLight, m_ui.SkyParams);

        if (m_ui.EnableTranslucency)
        {
            // Need to call PrepareLights again because due to KickStartSDK the m_CommandList was closed, we need to reset all bound resources.
            m_ForwardPass->PrepareLights(
                forwardContext,
                m_CommandList,
                m_Scene->GetSceneGraph()->GetLights(),
                m_AmbientTop,
                m_AmbientBottom,
                lightProbes,
#if defined(ENABLE_KickStartSDK)
                m_ui.KS.m_enableTransparentReflection ? m_RenderTargets[Layer::Transparent0]->Depth : nullptr,
                m_ui.KS.m_enableTransparentReflection ? m_RenderTargets[Layer::Transparent0]->GBufferRTReflections : nullptr);
#else
                nullptr, nullptr);
#endif

            RenderCompositeView(m_CommandList,
                m_View.get(), m_ViewPrevious.get(),
                *m_RenderTargets[Layer::Opaque]->ForwardFramebuffer,
                m_Scene->GetSceneGraph()->GetRootNode(),
                *m_TransparentDrawStrategy,
                *m_ForwardPass,
                forwardContext,
                "ForwardTransparent",
                m_ui.EnableMaterialEvents);
        }

        nvrhi::ITexture* finalHdrColor = m_RenderTargets[Layer::Opaque]->HdrColor;

        if (m_ui.AntiAliasingMode == AntiAliasingMode::TEMPORAL)
        {
            if (m_PreviousViewsValid)
            {
                m_TemporalAntiAliasingPass->RenderMotionVectors(m_CommandList, *m_View, *m_ViewPrevious);
            }

            m_TemporalAntiAliasingPass->TemporalResolve(m_CommandList, m_ui.TemporalAntiAliasingParams, m_PreviousViewsValid, *m_View, m_PreviousViewsValid ? *m_ViewPrevious : *m_View);

            finalHdrColor = m_RenderTargets[Layer::Opaque]->ResolvedColor;

            if (m_ui.EnableBloom)
            {
                m_BloomPass->Render(m_CommandList, m_RenderTargets[Layer::Opaque]->ResolvedFramebuffer, *m_View, m_RenderTargets[Layer::Opaque]->ResolvedColor, m_ui.BloomSigma, m_ui.BloomAlpha);
            }
            m_PreviousViewsValid = true;
        }
        else
        {
            std::shared_ptr<FramebufferFactory> finalHdrFramebuffer = m_RenderTargets[Layer::Opaque]->HdrFramebuffer;

            if (m_RenderTargets[Layer::Opaque]->GetSampleCount() > 1)
            {
                m_CommandList->resolveTexture(m_RenderTargets[Layer::Opaque]->ResolvedColor, nvrhi::AllSubresources, m_RenderTargets[Layer::Opaque]->HdrColor, nvrhi::AllSubresources);
                finalHdrColor = m_RenderTargets[Layer::Opaque]->ResolvedColor;
                finalHdrFramebuffer = m_RenderTargets[Layer::Opaque]->ResolvedFramebuffer;
            }

            if (m_ui.EnableBloom)
            {
                m_BloomPass->Render(m_CommandList, finalHdrFramebuffer, *m_View, finalHdrColor, m_ui.BloomSigma, m_ui.BloomAlpha);
            }

            m_PreviousViewsValid = false;
        }

        auto toneMappingParams = m_ui.ToneMappingParams;
        if (exposureResetRequired)
        {
            toneMappingParams.eyeAdaptationSpeedUp = 0.f;
            toneMappingParams.eyeAdaptationSpeedDown = 0.f;
        }
        m_ToneMappingPass->SimpleRender(m_CommandList, toneMappingParams, *m_View, finalHdrColor);

        m_CommonPasses->BlitTexture(m_CommandList, framebuffer, m_RenderTargets[Layer::Opaque]->LdrColor, &m_BindingCache);

        if (m_ui.DisplayShadowMap)
        {
            for (int cascade = 0; cascade < 4; cascade++)
            {
                nvrhi::Viewport viewport = nvrhi::Viewport(
                    10.f + 266.f * cascade,
                    266.f * (1 + cascade),
                    windowViewport.maxY - 266.f,
                    windowViewport.maxY - 10.f, 0.f, 1.f
                );

                engine::BlitParameters blitParams;
                blitParams.targetFramebuffer = framebuffer;
                blitParams.targetViewport = viewport;
                blitParams.sourceTexture = m_ShadowMap->GetTexture();
                blitParams.sourceArraySlice = cascade;
                m_CommonPasses->BlitTexture(m_CommandList, blitParams, &m_BindingCache);
            }
        }

#if defined(ENABLE_KickStartSDK)
        if (m_ui.KS.m_enableDebugSubViews) {
            float siz[2] = { 1920. / 5, 1080. / 5 };
            nvrhi::Viewport viewport = nvrhi::Viewport(0, siz[0], 1080 - siz[1], 1080, 0, 1);

            engine::BlitParameters blitParams;
            blitParams.targetFramebuffer = framebuffer;
            blitParams.targetViewport = viewport;
            blitParams.sourceTexture = m_RenderTargets[Layer::Opaque]->GBufferNormals;
            blitParams.sourceArraySlice = 0;
            m_CommonPasses->BlitTexture(m_CommandList, blitParams);

            viewport.minX += siz[0];
            viewport.maxX += siz[0];
            blitParams.targetViewport = viewport;
            blitParams.sourceTexture = m_RenderTargets[Layer::Opaque]->GBufferWorldPosition;
            m_CommonPasses->BlitTexture(m_CommandList, blitParams);

            if (m_ui.KS.m_enableReflection) {
                viewport.minX += siz[0];
                viewport.maxX += siz[0];
                blitParams.targetViewport = viewport;
                blitParams.sourceTexture = m_RenderTargets[Layer::Opaque]->GBufferRTReflections;
                m_CommonPasses->BlitTexture(m_CommandList, blitParams);
            }
            if (m_ui.KS.m_enableGI) {
                viewport.minX += siz[0];
                viewport.maxX += siz[0];
                blitParams.targetViewport = viewport;
                blitParams.sourceTexture = m_RenderTargets[Layer::Opaque]->GBufferRTGI;
                m_CommonPasses->BlitTexture(m_CommandList, blitParams);
            }
            if (m_ui.KS.m_enableAO) {
                viewport.minX += siz[0];
                viewport.maxX += siz[0];
                blitParams.targetViewport = viewport;
                blitParams.sourceTexture = m_RenderTargets[Layer::Opaque]->GBufferRTAO;
                m_CommonPasses->BlitTexture(m_CommandList, blitParams);
            }

            if (m_ui.KS.m_enableTransparentReflection)
            {
                engine::BlitParameters blitParams;
                blitParams.targetFramebuffer = framebuffer;

                viewport.minX += siz[0];
                viewport.maxX += siz[0];
                blitParams.targetViewport = viewport;
                blitParams.sourceTexture = m_RenderTargets[Layer::Transparent0]->GBufferNormals;
                blitParams.sourceArraySlice = 0;
                m_CommonPasses->BlitTexture(m_CommandList, blitParams);

                /*if (m_ui.KS.m_enableTransparentReflection)*/ {
                    viewport.minX += siz[0];
                    viewport.maxX += siz[0];
                    blitParams.targetViewport = viewport;
                    blitParams.sourceTexture = m_RenderTargets[Layer::Transparent0]->GBufferRTReflections;
                    m_CommonPasses->BlitTexture(m_CommandList, blitParams);
                }
            }

        }
#endif

        m_CommandList->close();
        GetDevice()->executeCommandList(m_CommandList);

#if defined(ENABLE_KickStartSDK)
        if (GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12 ||
            GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
        {
            m_CommandListKS_Post->close();
            GetDevice()->executeCommandList(m_CommandListKS_Post);
        }
        else if (GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11) {
#ifdef KickstartRT_Demo_WITH_D3D11
            // Insert fence and execute CommandList in 12 layer.
            nvrhi::RefCountPtr<ID3D11DeviceContext4>    cntxt4;
            {
                ID3D11DeviceContext* cntxt = reinterpret_cast<ID3D11DeviceContext*>(GetDevice()->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext).pointer);
                cntxt->QueryInterface(IID_PPV_ARGS(&cntxt4));
            }
            RecordCommandList11(cntxt4.Get(), m_SDKContext.m_11.get(), m_SDKContext.m_tc_postLighting.m_11);
            m_SDKContext.m_tc_postLighting.m_11 = nullptr;
#endif
        }
#endif

        if (!m_ui.ScreenshotFileName.empty())
        {
            SaveTextureToFile(GetDevice(), m_CommonPasses.get(), framebufferTexture, nvrhi::ResourceStates::RenderTarget, m_ui.ScreenshotFileName.c_str());
            m_ui.ScreenshotFileName = "";
        }

        if (m_Pick)
        {
            m_Pick = false;
            GetDevice()->waitForIdle();
            uint4 pixelValue = m_PixelReadbackPass->ReadUInts();
            m_ui.SelectedMaterial = nullptr;
            m_ui.SelectedNode = nullptr;
            m_ui.SelectedMeshInstance = nullptr;

            for (const auto& material : m_Scene->GetSceneGraph()->GetMaterials())
            {
                if (material->materialID == int(pixelValue.x))
                {
                    m_ui.SelectedMaterial = material;
                    break;
                }
            }

            for (const auto& instance : m_Scene->GetSceneGraph()->GetMeshInstances())
            {
                if (instance->GetInstanceIndex() == int(pixelValue.y))
                {
                    m_ui.SelectedNode = instance->GetNodeSharedPtr();
                    m_ui.SelectedMeshInstance = instance;
                    break;
                }
            }

            if (m_ui.SelectedNode)
            {
                log::info("Picked node: %s", m_ui.SelectedNode->GetPath().generic_string().c_str());
                PointThirdPersonCameraAt(m_ui.SelectedNode);
            }
            else
            {
                PointThirdPersonCameraAt(m_Scene->GetSceneGraph()->GetRootNode());
            }
        }

        m_TemporalAntiAliasingPass->AdvanceFrame();
        std::swap(m_View, m_ViewPrevious);

        GetDeviceManager()->SetVsyncEnabled(m_ui.EnableVsync);


#if defined(ENABLE_KickStartSDK)
        if (false) {
            static int i;

            if (++i % 100 == 0) {
                KickstartRT::ResourceAllocations allocationInfo;
#ifdef KickstartRT_Demo_WITH_D3D11
                if (m_SDKContext.m_11)
                    m_SDKContext.m_11->m_executeContext->GetCurrentResourceAllocations(&allocationInfo);
#endif
#ifdef KickstartRT_Demo_WITH_D3D12
                if (m_SDKContext.m_12)
                    m_SDKContext.m_12->m_executeContext->GetCurrentResourceAllocations(&allocationInfo);
#endif
#ifdef KickstartRT_Graphics_API_VK
                if (m_SDKContext.m_vk)
                    m_SDKContext.m_vk->m_executeContext->GetCurrentResourceAllocations(&allocationInfo);
#endif
                size_t totalNum = 0;
                for (size_t num : allocationInfo.m_numResources) {
                    totalNum += num;
                }
                size_t totalBytes = 0;
                for (size_t bytes : allocationInfo.m_totalRequestedBytes) {
                    totalBytes += bytes;
                }
                log::info("KS total allocated resources: num:%d, totalBytes:%d", totalNum, totalBytes);
            }
        }
#endif
    }

    std::shared_ptr<ShaderFactory> GetShaderFactory()
    {
        return m_ShaderFactory;
    }

    std::vector<std::shared_ptr<LightProbe>>& GetLightProbes()
    {
        return m_LightProbes;
    }

    void CreateLightProbes(uint32_t numProbes)
    {
        nvrhi::DeviceHandle device = GetDeviceManager()->GetDevice();

        uint32_t diffuseMapSize = 256;
        uint32_t diffuseMapMipLevels = 1;
        uint32_t specularMapSize = 512;
        uint32_t specularMapMipLevels = 8;

        nvrhi::TextureDesc cubemapDesc;

        cubemapDesc.arraySize = 6 * numProbes;
        cubemapDesc.dimension = nvrhi::TextureDimension::TextureCubeArray;
        cubemapDesc.isRenderTarget = true;
        cubemapDesc.keepInitialState = true;

        cubemapDesc.width = diffuseMapSize;
        cubemapDesc.height = diffuseMapSize;
        cubemapDesc.mipLevels = diffuseMapMipLevels;
        cubemapDesc.format = nvrhi::Format::RGBA16_FLOAT;
        cubemapDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        cubemapDesc.keepInitialState = true;

        m_LightProbeDiffuseTexture = device->createTexture(cubemapDesc);

        cubemapDesc.width = specularMapSize;
        cubemapDesc.height = specularMapSize;
        cubemapDesc.mipLevels = specularMapMipLevels;
        cubemapDesc.format = nvrhi::Format::RGBA16_FLOAT;
        cubemapDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        cubemapDesc.keepInitialState = true;

        m_LightProbeSpecularTexture = device->createTexture(cubemapDesc);

        m_LightProbes.clear();

        for (uint32_t i = 0; i < numProbes; i++)
        {
            std::shared_ptr<LightProbe> probe = std::make_shared<LightProbe>();

            probe->name = std::to_string(i + 1);
            probe->diffuseMap = m_LightProbeDiffuseTexture;
            probe->specularMap = m_LightProbeSpecularTexture;
            probe->diffuseArrayIndex = i;
            probe->specularArrayIndex = i;
            probe->bounds = frustum::empty();
            probe->enabled = false;

            m_LightProbes.push_back(probe);
        }
    }

    void RenderLightProbe(LightProbe& probe)
    {
        nvrhi::DeviceHandle device = GetDeviceManager()->GetDevice();

        uint32_t environmentMapSize = 1024;
        uint32_t environmentMapMipLevels = 8;

        nvrhi::TextureDesc cubemapDesc;
        cubemapDesc.arraySize = 6;
        cubemapDesc.width = environmentMapSize;
        cubemapDesc.height = environmentMapSize;
        cubemapDesc.mipLevels = environmentMapMipLevels;
        cubemapDesc.dimension = nvrhi::TextureDimension::TextureCube;
        cubemapDesc.isRenderTarget = true;
        cubemapDesc.format = nvrhi::Format::RGBA16_FLOAT;
        cubemapDesc.initialState = nvrhi::ResourceStates::RenderTarget;
        cubemapDesc.keepInitialState = true;
        cubemapDesc.clearValue = nvrhi::Color(0.f);
        cubemapDesc.useClearValue = true;

        nvrhi::TextureHandle colorTexture = device->createTexture(cubemapDesc);

        cubemapDesc.mipLevels = 1;
        cubemapDesc.format = nvrhi::Format::D24S8;
        cubemapDesc.isTypeless = true;
        cubemapDesc.initialState = nvrhi::ResourceStates::DepthWrite;

        nvrhi::TextureHandle depthTexture = device->createTexture(cubemapDesc);
        
        std::shared_ptr<FramebufferFactory> framebuffer = std::make_shared<FramebufferFactory>(device);
        framebuffer->RenderTargets = { colorTexture };
        framebuffer->DepthTarget = depthTexture;

        CubemapView view;
        view.SetArrayViewports(environmentMapSize, 0);
        const float nearPlane = 0.1f;
        const float cullDistance = 100.f;
        float3 probePosition = GetActiveCamera().GetPosition();
        if (m_ui.ActiveSceneCamera)
            probePosition = m_ui.ActiveSceneCamera->GetWorldToViewMatrix().m_translation;

        view.SetTransform(dm::translation(-probePosition), nearPlane, cullDistance);
        view.UpdateCache();
        
        std::shared_ptr<SkyPass> skyPass = std::make_shared<SkyPass>(device, m_ShaderFactory, m_CommonPasses, framebuffer, view);

        ForwardShadingPass::CreateParameters ForwardParams;
        ForwardParams.singlePassCubemap = GetDevice()->queryFeatureSupport(nvrhi::Feature::FastGeometryShader);
        std::shared_ptr<ForwardShadingPass> forwardPass = std::make_shared<ForwardShadingPass>(device, m_CommonPasses);
        forwardPass->Init(*m_ShaderFactory, ForwardParams);
        
        nvrhi::CommandListHandle commandList = device->createCommandList();
        commandList->open();
        commandList->clearTextureFloat(colorTexture, nvrhi::AllSubresources, nvrhi::Color(0.f));
        commandList->clearDepthStencilTexture(depthTexture, nvrhi::AllSubresources, true, 0.f, true, 0);

        box3 sceneBounds = m_Scene->GetSceneGraph()->GetRootNode()->GetGlobalBoundingBox();
        float zRange = length(sceneBounds.diagonal()) * 0.5f;
        m_ShadowMap->SetupForCubemapView(*m_SunLight, view.GetViewOrigin(), cullDistance, zRange, zRange, m_ui.CsmExponent);
        m_ShadowMap->Clear(commandList);

        DepthPass::Context shadowContext;

        RenderCompositeView(commandList,
            &m_ShadowMap->GetView(), nullptr,
            *m_ShadowFramebuffer,
            m_Scene->GetSceneGraph()->GetRootNode(),
            *m_OpaqueDrawStrategy,
            *m_ShadowDepthPass,
            shadowContext,
            "ShadowMap");

        ForwardShadingPass::Context forwardContext;

        std::vector<std::shared_ptr<LightProbe>> lightProbes;
        forwardPass->PrepareLights(forwardContext, commandList, m_Scene->GetSceneGraph()->GetLights(), m_AmbientTop, m_AmbientBottom, lightProbes, nullptr, nullptr);

        RenderCompositeView(commandList,
            &view, nullptr,
            *framebuffer,
            m_Scene->GetSceneGraph()->GetRootNode(),
            *m_OpaqueDrawStrategy,
            *forwardPass,
            forwardContext,
            "ForwardOpaque");
        
        skyPass->Render(commandList, view, *m_SunLight, m_ui.SkyParams);

        RenderCompositeView(commandList,
            &view, nullptr,
            *framebuffer,
            m_Scene->GetSceneGraph()->GetRootNode(),
            *m_TransparentDrawStrategy,
            *forwardPass,
            forwardContext,
            "ForwardTransparent");

        m_LightProbePass->GenerateCubemapMips(commandList, colorTexture, 0, 0, environmentMapMipLevels - 1);

        m_LightProbePass->RenderDiffuseMap(commandList, colorTexture, nvrhi::AllSubresources, probe.diffuseMap, probe.diffuseArrayIndex * 6, 0);
        
        uint32_t specularMapMipLevels = probe.specularMap->getDesc().mipLevels;
        for (uint32_t mipLevel = 0; mipLevel < specularMapMipLevels; mipLevel++)
        {
            float roughness = powf(float(mipLevel) / float(specularMapMipLevels - 1), 2.0f);
            m_LightProbePass->RenderSpecularMap(commandList, roughness, colorTexture, nvrhi::AllSubresources, probe.specularMap, probe.specularArrayIndex * 6, mipLevel);
        }
        
        m_LightProbePass->RenderEnvironmentBrdfTexture(commandList);

        commandList->close();
        device->executeCommandList(commandList);
        device->waitForIdle();
        device->runGarbageCollection();

        probe.environmentBrdf = m_LightProbePass->GetEnvironmentBrdfTexture();
        box3 bounds = box3(probePosition, probePosition).grow(10.f);
        probe.bounds = frustum::fromBox(bounds);
        probe.enabled = true;
    }
};

class UIRenderer : public ImGui_Renderer
{
private:
    std::shared_ptr<FeatureDemo> m_app;

	ImFont* m_FontOpenSans = nullptr;
	ImFont* m_FontDroidMono = nullptr;

	std::unique_ptr<ImGui_Console> m_console;
    std::shared_ptr<engine::Light> m_SelectedLight;

	UIData& m_ui;
    nvrhi::CommandListHandle m_CommandList;

public:
    UIRenderer(DeviceManager* deviceManager, std::shared_ptr<FeatureDemo> app, UIData& ui)
        : ImGui_Renderer(deviceManager)
        , m_app(app)
        , m_ui(ui)
    {
        m_CommandList = GetDevice()->createCommandList();

		m_FontOpenSans = this->LoadFont(*(app->GetRootFs()), "/media/fonts/OpenSans/OpenSans-Regular.ttf", 17.f);
		m_FontDroidMono = this->LoadFont(*(app->GetRootFs()), "/media/fonts/DroidSans/DroidSans-Mono.ttf", 14.f);

		ImGui_Console::Options opts;
		opts.font = m_FontDroidMono;
        auto interpreter = std::make_shared<console::Interpreter>();
		// m_console = std::make_unique<ImGui_Console>(interpreter,opts);

        ImGui::GetIO().IniFilename = nullptr;
    }

protected:
    virtual void buildUI(void) override
    {
        if (!m_ui.ShowUI)
            return;

        const auto& io = ImGui::GetIO();

        int width, height;
        GetDeviceManager()->GetWindowDimensions(width, height);

        if (m_app->IsSceneLoading())
        {
            BeginFullScreenWindow();

            char messageBuffer[256];
            const auto& stats = Scene::GetLoadingStats();
            snprintf(messageBuffer, std::size(messageBuffer), "Loading scene %s, please wait...\nObjects: %d/%d, Textures: %d/%d",
                m_app->GetCurrentSceneName().c_str(), stats.ObjectsLoaded.load(), stats.ObjectsTotal.load(), m_app->GetTextureCache()->GetNumberOfLoadedTextures(), m_app->GetTextureCache()->GetNumberOfRequestedTextures());

            DrawScreenCenteredText(messageBuffer);

            EndFullScreenWindow();

            return;
        }

        if (m_ui.ShowConsole && m_console)
        {
            m_console->Render(&m_ui.ShowConsole);
        }

        ImGui::SetNextWindowPos(ImVec2(10.f, 10.f), 0);
        ImGui::Begin("Settings", 0, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("Renderer: %s", GetDeviceManager()->GetRendererString());
        double frameTime = GetDeviceManager()->GetAverageFrameTimeSeconds();
        if (frameTime > 0.0)
            ImGui::Text("%.3f ms/frame (%.1f FPS)", frameTime * 1e3, 1.0 / frameTime);

        const std::string currentScene = m_app->GetCurrentSceneName();
        if (ImGui::BeginCombo("Scene", currentScene.c_str()))
        {
            const std::vector<std::string>& scenes = m_app->GetAvailableScenes();
            for (const std::string& scene : scenes)
            {
                bool is_selected = scene == currentScene;
                if (ImGui::Selectable(scene.c_str(), is_selected))
                    m_app->SetCurrentSceneName(scene);
                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (ImGui::Button("Reload Shaders"))
            m_ui.ShaderReoladRequested = true;

        ImGui::Checkbox("VSync", &m_ui.EnableVsync);
        //ImGui::Checkbox("Deferred Shading", &m_ui.UseDeferredShading);
        //if (m_ui.AntiAliasingMode >= AntiAliasingMode::MSAA_2X)
        //    m_ui.UseDeferredShading = false; // Deferred shading doesn't work with MSAA
        //ImGui::Checkbox("Stereo", &m_ui.Stereo);
        ImGui::Checkbox("Animations", &m_ui.EnableAnimations);

        if (ImGui::BeginCombo("Camera (T)", m_ui.ActiveSceneCamera ? m_ui.ActiveSceneCamera->GetName().c_str()
            : m_ui.UseThirdPersonCamera ? "Third-Person" : "First-Person"))
        {
            if (ImGui::Selectable("First-Person", !m_ui.ActiveSceneCamera && !m_ui.UseThirdPersonCamera))
            {
                m_ui.ActiveSceneCamera.reset();
                m_ui.UseThirdPersonCamera = false;
            }
            if (ImGui::Selectable("Third-Person", !m_ui.ActiveSceneCamera && m_ui.UseThirdPersonCamera))
            {
                m_ui.ActiveSceneCamera.reset();
                m_ui.UseThirdPersonCamera = true;
                m_app->CopyActiveCameraToFirstPerson();
            }
            for (const auto& camera : m_app->GetScene()->GetSceneGraph()->GetCameras())
            {
                if (ImGui::Selectable(camera->GetName().c_str(), m_ui.ActiveSceneCamera == camera))
                {
                    m_ui.ActiveSceneCamera = camera;
                    m_app->CopyActiveCameraToFirstPerson();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Enable Translucency", &m_ui.EnableTranslucency);

#if defined(ENABLE_KickStartSDK)

        ImGui::Separator();
        ImGui::Text("KickstartRT - Features");

        ImGui::Checkbox("Enable Reflections (Opaque)", &m_ui.KS.m_enableReflection);
        ImGui::Checkbox("Enable Reflections (Transparent)", &m_ui.KS.m_enableTransparentReflection);
        ImGui::Checkbox("Enable GI", &m_ui.KS.m_enableGI);
        ImGui::Checkbox("Enable AO", &m_ui.KS.m_enableAO);
        {
            std::array<const char*, 3> shadowMethodStr = { "Disabled" , "Shadow", "MultiShadow" };
            if (ImGui::BeginCombo("RT Shadows", shadowMethodStr[m_ui.KS.m_enableShadows])) {
                if (ImGui::Selectable(shadowMethodStr[0], m_ui.KS.m_enableShadows == 0)) {
                    m_ui.KS.m_enableShadows = 0;
                }
                if (ImGui::Selectable(shadowMethodStr[1], m_ui.KS.m_enableShadows == 1)) {
                    m_ui.KS.m_enableShadows = 1;
                }
                if (ImGui::Selectable(shadowMethodStr[2], m_ui.KS.m_enableShadows == 2)) {
                    m_ui.KS.m_enableShadows = 2;
                }
                ImGui::EndCombo();
            }
        }
        ImGui::Checkbox("Enable Checkerboard", &m_ui.KS.m_enableCheckerboard);

        ImGui::Separator();
        ImGui::Text("KickstartRT - Denoising Features");

        {
            std::array<const char*, 3> denoisingMethodStr = { "Disabled" , "REBLUR", "RELAX" };
            if (ImGui::BeginCombo("Reflections & GI", denoisingMethodStr[m_ui.KS.m_denoisingMethod])) {
                if (ImGui::Selectable(denoisingMethodStr[0], m_ui.KS.m_denoisingMethod == 0)) {
                    m_ui.KS.m_denoisingMethod = 0;
                }
                if (ImGui::Selectable(denoisingMethodStr[1], m_ui.KS.m_denoisingMethod == 1)) {
                    m_ui.KS.m_denoisingMethod = 1;
                }
                if (ImGui::Selectable(denoisingMethodStr[2], m_ui.KS.m_denoisingMethod == 2)) {
                    m_ui.KS.m_denoisingMethod = 2;
                }
                ImGui::EndCombo();
            }
        }
        {
            std::array<const char*, 2> denoisingMethodStr = { "Disabled" , "REBLUR" };
            if (ImGui::BeginCombo("AO", denoisingMethodStr[m_ui.KS.m_aoDenoisingMethod])) {
                if (ImGui::Selectable(denoisingMethodStr[0], m_ui.KS.m_aoDenoisingMethod == 0)) {
                    m_ui.KS.m_aoDenoisingMethod = 0;
                }
                if (ImGui::Selectable(denoisingMethodStr[1], m_ui.KS.m_aoDenoisingMethod == 1)) {
                    m_ui.KS.m_aoDenoisingMethod = 1;
                }
                ImGui::EndCombo();
            }
        }
        {
            std::array<const char*, 2> denoisingMethodStr = { "Disabled" , "SIGMA" };
            if (ImGui::BeginCombo("Shadows", denoisingMethodStr[m_ui.KS.m_shadowDenoisingMethod])) {
                if (ImGui::Selectable(denoisingMethodStr[0], m_ui.KS.m_shadowDenoisingMethod == 0)) {
                    m_ui.KS.m_shadowDenoisingMethod = 0;
                }
                if (ImGui::Selectable(denoisingMethodStr[1], m_ui.KS.m_shadowDenoisingMethod == 1)) {
                    m_ui.KS.m_shadowDenoisingMethod = 1;
                }
                ImGui::EndCombo();
            }
        }

        {
            m_ui.KS.m_denoisingReset = ImGui::Button("Reset Denoising History");
        }

        ImGui::Separator();
        ImGui::Text("KickstartRT - Debug Features");
        {
            ImGui::Checkbox("Enable Debug Sub Views", &m_ui.KS.m_enableDebugSubViews);

            {
                std::array<const char*, 5> debugDispStr = { "Disabled" , "DirectLightingCache", "RandomTileColor", "MeshColor", "HitT_PrimaryRays" };
                size_t dispIdx = m_ui.KS.m_debugDisp == 0 ? m_ui.KS.m_debugDisp : m_ui.KS.m_debugDisp - 99;
                if (ImGui::BeginCombo("Debug Disp", debugDispStr[dispIdx])) {
                    if (ImGui::Selectable(debugDispStr[0], m_ui.KS.m_debugDisp == 0)) {
                        m_ui.KS.m_debugDisp = 0;
                    }
                    if (ImGui::Selectable(debugDispStr[1], m_ui.KS.m_debugDisp == 100)) {
                        m_ui.KS.m_debugDisp = 100;
                    }
                    if (ImGui::Selectable(debugDispStr[2], m_ui.KS.m_debugDisp == 101)) {
                        m_ui.KS.m_debugDisp = 101;
                    }
                    if (ImGui::Selectable(debugDispStr[3], m_ui.KS.m_debugDisp == 102)) {
                        m_ui.KS.m_debugDisp = 102;
                    }
                    if (ImGui::Selectable(debugDispStr[4], m_ui.KS.m_debugDisp == 103)) {
                        m_ui.KS.m_debugDisp = 103;
                    }
                    ImGui::EndCombo();
                }
            }
        }

        ImGui::Checkbox("Enable Global Roughness", &m_ui.KS.m_enableGlobalRoughness);
        if (m_ui.KS.m_enableGlobalRoughness) {
            ImGui::DragFloat("Global Roughness", &m_ui.KS.m_globalRoughness, 0.01f, 0.f, 1.0f, "%.2f");
        }

        ImGui::Checkbox("Enable Global Metalness", &m_ui.KS.m_enableGlobalMetalness);
        if (m_ui.KS.m_enableGlobalMetalness) {
            ImGui::DragFloat("Global Metalness", &m_ui.KS.m_globalMetalness, 0.01f, 0.f, 1.0f, "%.2f");
        }

        ImGui::Separator();
        ImGui::Text("KickstartRT - Direct Lighting Cache");
        {
            std::array<const char*, 2> surfelMode = { "WarpedBarycentricStorage", "MeshColors" };
            if (ImGui::BeginCombo("Surfel Mode", surfelMode[m_ui.KS.m_surfelMode])) {
                if (ImGui::Selectable(surfelMode[0], m_ui.KS.m_surfelMode == 0)) {
                    m_ui.KS.m_surfelMode = 0;
                    m_ui.KS.m_destructGeom = true;
                }
                if (ImGui::Selectable(surfelMode[1], m_ui.KS.m_surfelMode == 1)) {
                    m_ui.KS.m_surfelMode = 1;
                    m_ui.KS.m_destructGeom = true;
                }
                ImGui::EndCombo();
            }
        }

        {
            std::array<const char*, 2> surfelMode = { "Nearest-neighbour", "Bilinear" };
            if (ImGui::BeginCombo("Surfel Sample Mode", surfelMode[m_ui.KS.m_surfelSampleMode])) {
                if (ImGui::Selectable(surfelMode[0], m_ui.KS.m_surfelSampleMode == 0)) {
                    m_ui.KS.m_surfelSampleMode = 0;
                }
                if (ImGui::Selectable(surfelMode[1], m_ui.KS.m_surfelSampleMode == 1,
                    m_ui.KS.m_surfelMode == 1 ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled)) {
                    m_ui.KS.m_surfelSampleMode = 1;
                }
                ImGui::EndCombo();
            }
        }

		if (!m_ui.KS.m_forceDirectTileMapping) {
			if (ImGui::DragFloat("Tile unit length", &m_ui.KS.m_tileUnitLength, 1.0f, 1.f, 100.f, "%.1f")) {
				m_ui.KS.m_destructGeom = true;
			}
			if (ImGui::DragInt("Tile resolution limit", (int*)&m_ui.KS.m_tileResolutionLimit, 2, 16, 128, "%d")) {
				m_ui.KS.m_destructGeom = true;
			}
            if (ImGui::DragInt("Light Injection Stride", (int*)&m_ui.KS.m_lightInjectionStride, 1, 1, 16, "%d")) {
                m_ui.KS.m_destructGeom = true;
            }
		}
        if (m_ui.KS.m_surfelMode == 0) {
            if (m_ui.KS.m_surfelMode == 0 && ImGui::Checkbox("Force Direct Tile Mapping", &m_ui.KS.m_forceDirectTileMapping)) {
                // rebuild geometry.
                m_ui.KS.m_destructGeom = true;
            }
        }

        ImGui::Separator();
        ImGui::Text("KickstartRT - Miscs");
#if 0
		ImGui::Checkbox("Enable Camera Jitter", &m_ui.KS.m_enableCameraJitter);
#endif
		ImGui::Checkbox("Enable Late Light Injection", &m_ui.KS.m_enableLateLightInjection);
		ImGui::Checkbox("Reflections - Enable screen space sampling", &m_ui.KS.m_enableDirectLightingSample);
        ImGui::Checkbox("Shadows - Enable First Hit And End Search", &m_ui.KS.m_shadowsEnableFirstHitAndEndSearch);

        ImGui::Checkbox("Use Trace Ray Inline", &m_ui.KS.m_useTraceRayInline);

        ImGui::Checkbox("Perform Light Cache Transfer", &m_ui.KS.m_performTransfer);
        ImGui::Checkbox("Clear Light Cache", &m_ui.KS.m_destructGeom);

        ImGui::DragFloat("Max Ray Length", &m_ui.KS.m_maxRayLength, 5.f, 0.f, 1000.f);

        ImGui::Separator();
        ImGui::Text("KickstartRT - Ray offset adjustments");

		ImGui::Checkbox("Enable World Pos From Depth", &m_ui.KS.m_enableWorldPosFromDepth);

		{
			std::array<const char*, 3> rayOffsetStr = { "Disabled" , "WorldPosition", "CamDistance" };
			if (ImGui::BeginCombo("RayOffsetType", rayOffsetStr[m_ui.KS.m_rayOffsetType])) {
				if (ImGui::Selectable(rayOffsetStr[0], m_ui.KS.m_rayOffsetType == 0)) {
					m_ui.KS.m_rayOffsetType = 0;
				}
				if (ImGui::Selectable(rayOffsetStr[1], m_ui.KS.m_rayOffsetType == 1)) {
					m_ui.KS.m_rayOffsetType = 1;
				}
				if (ImGui::Selectable(rayOffsetStr[2], m_ui.KS.m_rayOffsetType == 2)) {
					m_ui.KS.m_rayOffsetType = 2;
				}
				ImGui::EndCombo();
			}
		}

		if (m_ui.KS.m_rayOffsetType == 1) {
			ImGui::DragFloat("RayOffset_Threshold", &m_ui.KS.m_rayOffset_WorldPosition_threshold, 1.f / 1024.f, 1.f / 128.f, 1.f / 2.f, "%.6f");
			ImGui::DragFloat("RayOffset_FloatScale", &m_ui.KS.m_rayOffset_WorldPosition_floatScale, 1.f / 65536.f, 1.f / 65536.f, 1.f / 128.f, "%.6f");
			ImGui::DragFloat("RayOffset_IntScale", &m_ui.KS.m_rayOffset_WorldPosition_intScale, 128.f, 256.f, 65536.f, "%.1f");
		}
		else if (m_ui.KS.m_rayOffsetType == 2) {
			ImGui::DragFloat("RayOffset_Constant", &m_ui.KS.m_rayOffset_CamDistance_constant, 0.001f, 0.0f, 0.01f, "%.6f");
			ImGui::DragFloat("RayOffset_Linear", &m_ui.KS.m_rayOffset_CamDistance_linear, 0.0001f, -0.0003f, 0.003f, "%.6f");
			ImGui::DragFloat("RayOffset_Quadratic", &m_ui.KS.m_rayOffset_CamDistance_quadratic, 0.00001f, 0.f, 0.003f, "%.6f");
		}

		ImGui::Separator();

		if (ImGui::Button("Export ColdLoadShader List"))
		{
			std::filesystem::path exePath = donut::app::GetDirectoryWithExecutable();
			exePath /= "ColdLoadShaderList.bin";
			std::string fileName = exePath.string();

			if (FileDialog(false, "bin files\0*.bin\0All files\0*.*\0\0", fileName))
			{
				m_ui.KS.m_ExportShaderColdLoadListFileName = fileName;
			}
		}
		ImGui::Separator();
#endif

        ImGui::Separator();
        ImGui::Text("Lights");

        {
            const auto& lights = m_app->GetScene()->GetSceneGraph()->GetLights();

            if (!lights.empty() && ImGui::CollapsingHeader("Lights"))
            {
                if (ImGui::BeginCombo("Select Light", m_SelectedLight ? m_SelectedLight->GetName().c_str() : "(None)"))
                {
                    for (const auto& light : lights)
                    {
                        bool selected = m_SelectedLight == light;
                        ImGui::Selectable(light->GetName().c_str(), &selected);
                        if (selected)
                        {
                            m_SelectedLight = light;
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

                if (m_SelectedLight)
                {
                    app::LightEditor(*m_SelectedLight);
                }
            }
        }

        ImGui::SliderFloat("Ambient Intensity", &m_ui.AmbientIntensity, 0.f, 1.f);

        ImGui::Checkbox("Enable Procedural Sky", &m_ui.EnableProceduralSky);
        if (m_ui.EnableProceduralSky && ImGui::CollapsingHeader("Sky Parameters"))
        {
            ImGui::SliderFloat("Brightness", &m_ui.SkyParams.brightness, 0.f, 1.f);
            ImGui::SliderFloat("Glow Size", &m_ui.SkyParams.glowSize, 0.f, 90.f);
            ImGui::SliderFloat("Glow Sharpness", &m_ui.SkyParams.glowSharpness, 1.f, 10.f);
            ImGui::SliderFloat("Glow Intensity", &m_ui.SkyParams.glowIntensity, 0.f, 1.f);
            ImGui::SliderFloat("Horizon Size", &m_ui.SkyParams.horizonSize, 0.f, 90.f);
        }

        ImGui::Separator();
        ImGui::Text("Raster features");
        ImGui::Checkbox("Enable SSAO", &m_ui.EnableSsao);
        ImGui::Checkbox("Enable Shadows", &m_ui.EnableShadows);
        ImGui::Checkbox("Enable Bloom", &m_ui.EnableBloom);
        if (m_ui.EnableBloom && ImGui::CollapsingHeader("Bloom Parameters")) {
            ImGui::DragFloat("Bloom Sigma", &m_ui.BloomSigma, 0.01f, 0.1f, 100.f);
            ImGui::DragFloat("Bloom Alpha", &m_ui.BloomAlpha, 0.01f, 0.01f, 1.0f);
        }

#if 0
        ImGui::Combo("AA Mode", (int*)&m_ui.AntiAliasingMode, "None\0TemporalAA\0MSAA 2x\0MSAA 4x\0MSAA 8x\0");
        ImGui::Combo("TAA Camera Jitter", (int*)&m_ui.TemporalAntiAliasingJitter, "MSAA\0Halton\0R2\0White Noise\0");

        ImGui::Checkbox("Enable Light Probe", &m_ui.EnableLightProbe);
        if (m_ui.EnableLightProbe && ImGui::CollapsingHeader("Light Probe"))
        {
            ImGui::DragFloat("Diffuse Scale", &m_ui.LightProbeDiffuseScale, 0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Specular Scale", &m_ui.LightProbeSpecularScale, 0.01f, 0.0f, 10.0f);
        }

        ImGui::Separator();
        ImGui::Checkbox("Temporal AA Clamping", &m_ui.TemporalAntiAliasingParams.enableHistoryClamping);
        ImGui::Checkbox("Material Events", &m_ui.EnableMaterialEvents);
        ImGui::Separator();

        ImGui::TextUnformatted("Render Light Probe: ");
        uint32_t probeIndex = 1;
        for (auto probe : m_app->GetLightProbes())
        {
            ImGui::SameLine();
            if (ImGui::Button(probe->name.c_str()))
            {
                m_app->RenderLightProbe(*probe);
            }
        }
#endif

        if (ImGui::Button("Screenshot"))
        {
            std::string fileName;
            if (FileDialog(false, "BMP files\0*.bmp\0All files\0*.*\0\0", fileName))
            {
                m_ui.ScreenshotFileName = fileName;
            }
        }

        ImGui::End();

        auto material = m_ui.SelectedMaterial;
        if (material)
        {
            ImGui::SetNextWindowPos(ImVec2(float(width) - 10.f, 10.f), 0, ImVec2(1.f, 0.f));
            ImGui::Begin("Material Editor");
            ImGui::Text("Material %d: %s", material->materialID, material->name.c_str());

            MaterialDomain previousDomain = material->domain;
            material->dirty = donut::app::MaterialEditor(material.get(), true);

            if (previousDomain != material->domain)
                m_app->GetScene()->GetSceneGraph()->GetRootNode()->InvalidateContent();
            
            ImGui::End();
        }

#if defined(ENABLE_KickStartSDK)
        auto meshInstance = m_ui.SelectedMeshInstance;
        if (meshInstance)
        {
            ImGui::SetNextWindowPos(ImVec2(float(width) - 10.f, 300.f), 0, ImVec2(1.f, 0.f));
            ImGui::Begin("Instance Editor");

            auto&& instances(m_app->m_Scene->GetSceneGraph()->GetMeshInstances());
            for (size_t i=0; i<instances.size(); ++i) {
                std::string sNum = std::to_string(i);
                std::string sWrk;

                ImGui::Separator();
                ImGui::Text("%d:Name \"%s\"", i, instances[i]->GetName().c_str());

                sWrk = sNum + ":Visibe in Raster ";
                ImGui::Checkbox(sWrk.c_str(), &instances[i]->Visibility());

                ImGui::Text("KickStartRT: InstanceInclusionMask");

                auto it = m_app->m_SDKContext.m_insStates.find(instances[i].get());
                assert(it != m_app->m_SDKContext.m_insStates.end());

                sWrk = sNum + ":Direct Light Injection Target";
                if (ImGui::Checkbox(sWrk.c_str(), &it->second.instanceProp_DirectLightInjectionTarget))
                    it->second.isDirty = true;

                sWrk = sNum + ":Direct Light Transfer Source";
                if (ImGui::Checkbox(sWrk.c_str(), &it->second.instanceProp_LightTransferSource))
                    it->second.isDirty = true;

                sWrk = sNum + ":Direct Light Transfer Target";
                if (ImGui::Checkbox(sWrk.c_str(), &it->second.instanceProp_LightTransferTarget))
                    it->second.isDirty = true;

                sWrk = sNum + ":Visible in RT";
                if (ImGui::Checkbox(sWrk.c_str(), &it->second.instanceProp_VisibleInRT))
                    it->second.isDirty = true;
            }


            ImGui::End();
        }
#endif
        
        if (m_ui.AntiAliasingMode != AntiAliasingMode::NONE && m_ui.AntiAliasingMode != AntiAliasingMode::TEMPORAL)
            m_ui.UseDeferredShading = false;

        if (!m_ui.UseDeferredShading)
            m_ui.EnableSsao = false;
    }
};

bool ProcessCommandLine(int argc, const char* const* argv, DeviceCreationParameters& deviceParams, std::string& sceneName)
{
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-width"))
        {
            deviceParams.backBufferWidth = std::stoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "-height"))
        {
            deviceParams.backBufferHeight = std::stoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "-fullscreen"))
        {
            deviceParams.startFullscreen = true;
        }
        else if (!strcmp(argv[i], "-debug"))
        {
            deviceParams.enableDebugRuntime = true;
            deviceParams.enableNvrhiValidationLayer = true;
        }
        else if (!strcmp(argv[i], "-no-vsync"))
        {
            deviceParams.vsyncEnabled = false;
        }
        else if (!strcmp(argv[i], "-print-graph"))
        {
            g_PrintSceneGraph = true;
        }
        else if (!strcmp(argv[i], "-nv-adapter")) {
            deviceParams.adapterNameSubstring = L"NVIDIA";
        }
        else if (argv[i][0] == '-') {
            // It's not a scene name. Silently ignore it.
        }
        else
        {
            sceneName = argv[i];
        }
    }

    return true;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);

#if defined(ENABLE_KickStartSDK)
//    api = nvrhi::GraphicsAPI::D3D11;
//    api = nvrhi::GraphicsAPI::D3D12;
//    api = nvrhi::GraphicsAPI::VULKAN;
#endif

#else //  _WIN32
int main(int __argc, const char* const* __argv)
{
    nvrhi::GraphicsAPI api = nvrhi::GraphicsAPI::VULKAN;
#endif //  _WIN32

    DeviceCreationParameters deviceParams;
    
    // deviceParams.adapter = VrSystem::GetRequiredAdapter();
    deviceParams.backBufferWidth = 1920;
    deviceParams.backBufferHeight = 1080;
    deviceParams.swapChainSampleCount = 1;
    deviceParams.swapChainBufferCount = 2;
    deviceParams.startFullscreen = false;
    deviceParams.vsyncEnabled = true;

    deviceParams.enableRayTracingExtensions = true;
#if _DEBUG
    deviceParams.enableDebugRuntime = true;
#endif

#if USE_VK
    deviceParams.requiredVulkanDeviceExtensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    deviceParams.requiredVulkanDeviceExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
#endif

    std::string sceneName;
    if (!ProcessCommandLine(__argc, __argv, deviceParams, sceneName))
    {
        log::error("Failed to process the command line.");
        return 1;
    }
    
    DeviceManager* deviceManager = DeviceManager::Create(api);
    const char* apiString = nvrhi::utils::GraphicsAPIToString(deviceManager->GetGraphicsAPI());

    std::string windowTitle = "KickstartRT Demo (" + std::string(apiString) + ")";

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, windowTitle.c_str()))
	{
        log::error("Cannot initialize a %s graphics device with the requested parameters", apiString);
		return 1;
	}

    {
        UIData uiData;

        std::shared_ptr<FeatureDemo> demo = std::make_shared<FeatureDemo>(deviceManager, uiData, sceneName);
        std::shared_ptr<UIRenderer> gui = std::make_shared<UIRenderer>(deviceManager, demo, uiData);

        gui->Init(demo->GetShaderFactory());

        deviceManager->AddRenderPassToBack(demo.get());
        deviceManager->AddRenderPassToBack(gui.get());

        deviceManager->RunMessageLoop();
    }

    deviceManager->Shutdown();
#ifdef _DEBUG
    deviceManager->ReportLiveObjects();
#endif
    delete deviceManager;
	
	return 0;
}
