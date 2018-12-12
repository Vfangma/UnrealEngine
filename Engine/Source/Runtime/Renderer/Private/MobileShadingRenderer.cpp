// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	MobileShadingRenderer.cpp: Scene rendering code for the ES2 feature level.
=============================================================================*/

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "Misc/MemStack.h"
#include "HAL/IConsoleManager.h"
#include "EngineGlobals.h"
#include "RHIDefinitions.h"
#include "RHI.h"
#include "RenderResource.h"
#include "RendererInterface.h"
#include "SceneUtils.h"
#include "UniformBuffer.h"
#include "Engine/BlendableInterface.h"
#include "ShaderParameters.h"
#include "RHIStaticStates.h"
#include "Shader.h"
#include "StaticBoundShaderState.h"
#include "PostProcess/SceneRenderTargets.h"
#include "GlobalShader.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "PostProcess/SceneFilterRendering.h"
#include "FXSystem.h"
#include "PostProcess/RenderingCompositionGraph.h"
#include "PostProcess/PostProcessing.h"
#include "PostProcess/PostProcessMobile.h"
#include "PostProcess/PostProcessUpscale.h"
#include "PostProcess/PostProcessCompositeEditorPrimitives.h"
#include "PostProcess/PostProcessHMD.h"
#include "IHeadMountedDisplay.h"
#include "IXRTrackingSystem.h"
#include "SceneViewExtension.h"
#include "ScreenRendering.h"
#include "PipelineStateCache.h"
#include "ClearQuad.h"
#include "MobileSeparateTranslucencyPass.h"
#include "MobileDistortionPass.h"
#include "VisualizeTexturePresent.h"
#include "VisualizeTexture.h"

uint32 GetShadowQuality();

static TAutoConsoleVariable<int32> CVarMobileAlwaysResolveDepth(
	TEXT("r.Mobile.AlwaysResolveDepth"),
	0,
	TEXT("0: Depth buffer is resolved after opaque pass only when decals or modulated shadows are in use. (Default)\n")
	TEXT("1: Depth buffer is always resolved after opaque pass.\n"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarMobileForceDepthResolve(
	TEXT("r.Mobile.ForceDepthResolve"),
	0,
	TEXT("0: Depth buffer is resolved by switching out render targets. (Default)\n")
	TEXT("1: Depth buffer is resolved by switching out render targets and drawing with the depth texture.\n"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarMobileMoveSubmissionHintAfterTranslucency(
	TEXT("r.Mobile.MoveSubmissionHintAfterTranslucency"),
	1,
	TEXT("0: Submission hint occurs after occlusion query.\n")
	TEXT("1: Submission hint occurs after translucency. (Default)"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

DECLARE_CYCLE_STAT(TEXT("SceneStart"), STAT_CLMM_SceneStart, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("SceneEnd"), STAT_CLMM_SceneEnd, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("InitVIews"), STAT_CLMM_InitVIews, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("BasePass"), STAT_CLMM_BasePass, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("Occlusion"), STAT_CLMM_Occlusion, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("Post"), STAT_CLMM_Post, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("Translucency"), STAT_CLMM_Translucency, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("Shadows"), STAT_CLMM_Shadows, STATGROUP_CommandListMarkers);

FMobileSceneRenderer::FMobileSceneRenderer(const FSceneViewFamily* InViewFamily,FHitProxyConsumer* HitProxyConsumer)
	:	FSceneRenderer(InViewFamily, HitProxyConsumer)
{
	bModulatedShadowsInUse = false;
}

class FMobileDirLightShaderParamsRenderResource : public FRenderResource
{
public:
	using MobileDirLightUniformBufferRef = TUniformBufferRef<FMobileDirectionalLightShaderParameters>;

	virtual void InitRHI() override
	{
		UniformBufferRHI =
			MobileDirLightUniformBufferRef::CreateUniformBufferImmediate(
				FMobileDirectionalLightShaderParameters(),
				UniformBuffer_MultiFrame);
	}

	virtual void ReleaseRHI() override
	{
		UniformBufferRHI.SafeRelease();
	}

	MobileDirLightUniformBufferRef UniformBufferRHI;
};

TUniformBufferRef<FMobileDirectionalLightShaderParameters>& GetNullMobileDirectionalLightShaderParameters()
{
	static TGlobalResource<FMobileDirLightShaderParamsRenderResource>* NullLightParams;
	if (!NullLightParams)
	{
		NullLightParams = new TGlobalResource<FMobileDirLightShaderParamsRenderResource>();
	}
	check(!!NullLightParams->UniformBufferRHI);
	return NullLightParams->UniformBufferRHI;
}

void FMobileSceneRenderer::SortMeshDrawCommands()
{
	// ignoring sort here, mobile needs to sort after shadow init is performed.
	//SortMobileMeshDrawCommands();
}

void FMobileSceneRenderer::SortMobileMeshDrawCommands()
{
	// Sort front to back on all platforms, even HSR benefits from it
	//const bool bWantsFrontToBackSorting = (GHardwareHiddenSurfaceRemoval == false);

	// compute keys for front to back sorting
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		FViewInfo& View = Views[ViewIndex];
		
		FMeshCommandOneFrameArray& BasePassVisibleCommands = View.VisibleMeshDrawCommands[EMeshPass::BasePass];
		MobileBasePass::ComputeBasePassSortKeys(*Scene, View, BasePassVisibleCommands);

		UpdateTranslucentMeshSortKeys(Scene, View, ETranslucencyPass::TPT_StandardTranslucency, View.VisibleMeshDrawCommands[EMeshPass::MobileInverseOpacity]);
	}

	FSceneRenderer::SortMeshDrawCommands();
}

/**
 * Initialize scene's views.
 * Check visibility, sort translucent items, etc.
 */
void FMobileSceneRenderer::InitViews(FRHICommandListImmediate& RHICmdList)
{
	RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_InitVIews));

	SCOPED_DRAW_EVENT(RHICmdList, InitViews);

	SCOPE_CYCLE_COUNTER(STAT_InitViewsTime);

	FILCUpdatePrimTaskData ILCTaskData;
	PreVisibilityFrameSetup(RHICmdList);
	ComputeViewVisibility(RHICmdList, FExclusiveDepthStencil::DepthWrite_StencilWrite);
	PostVisibilityFrameSetup(ILCTaskData);

	const bool bDynamicShadows = ViewFamily.EngineShowFlags.DynamicShadows;

	if (bDynamicShadows && !IsSimpleForwardShadingEnabled(ShaderPlatform))
	{
		// Setup dynamic shadows.
		InitDynamicShadows(RHICmdList);		
	}

	SortMobileMeshDrawCommands();

	// if we kicked off ILC update via task, wait and finalize.
	if (ILCTaskData.TaskRef.IsValid())
	{
		Scene->IndirectLightingCache.FinalizeCacheUpdates(Scene, *this, ILCTaskData);
	}

	// initialize per-view uniform buffer.  Pass in shadow info as necessary.
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		// Initialize the view's RHI resources.
		Views[ViewIndex].InitRHIResources();

		// TODO: remove when old path is removed
		// Create the directional light uniform buffers
		CreateDirectionalLightUniformBuffers(Views[ViewIndex]);
	}
	
	// update buffers used in cached mesh path
	// in case there are multiple views, these buffers will be updated before rendering each view
	if (Views.Num() > 0)
	{
		const FViewInfo& View = Views[0];
		Scene->UniformBuffers.UpdateViewUniformBuffer(View);
		UpdateOpaqueBasePassUniformBuffer(RHICmdList, View);
		UpdateTranslucentBasePassUniformBuffer(RHICmdList, View);
		UpdateDirectionalLightUniformBuffers(RHICmdList, View);

		FMobileDistortionPassUniformParameters DistortionPassParameters;
		SetupMobileDistortionPassUniformBuffer(RHICmdList, View, DistortionPassParameters);
		Scene->UniformBuffers.MobileDistortionPassUniformBuffer.UpdateUniformBufferImmediate(DistortionPassParameters);
	}

	// Now that the indirect lighting cache is updated, we can update the uniform buffers.
	UpdatePrimitiveIndirectLightingCacheBuffers();

	if (!UseMeshDrawCommandPipeline())
	{
		PostInitViewCustomData();
	}
	
	OnStartRender(RHICmdList);
}

void FMobileSceneRenderer::PostInitViewCustomData()
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_UpdateViewCustomData);

	for (FViewInfo& ViewInfo : Views)
	{
		for (const FPrimitiveSceneInfo* PrimitiveSceneInfo : ViewInfo.PrimitivesWithCustomData)
		{
			PrimitiveSceneInfo->Proxy->PostInitViewCustomData(ViewInfo, ViewInfo.GetCustomData(PrimitiveSceneInfo->GetIndex()));
		}
	}
}

/** 
* Renders the view family. 
*/
void FMobileSceneRenderer::Render(FRHICommandListImmediate& RHICmdList)
{
	RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_SceneStart));

	PrepareViewRectsForRendering();

	QUICK_SCOPE_CYCLE_COUNTER(STAT_FMobileSceneRenderer_Render);
	//FRHICommandListExecutor::GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);

	if(!ViewFamily.EngineShowFlags.Rendering)
	{
		return;
	}

	WaitOcclusionTests(RHICmdList);
	FRHICommandListExecutor::GetImmediateCommandList().PollOcclusionQueries();
	RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);


	const ERHIFeatureLevel::Type ViewFeatureLevel = ViewFamily.GetFeatureLevel();

	// Initialize global system textures (pass-through if already initialized).
	GSystemTextures.InitializeTextures(RHICmdList, ViewFeatureLevel);
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

	// Allocate the maximum scene render target space for the current view family.
	SceneContext.Allocate(RHICmdList, this);

	//make sure all the targets we're going to use will be safely writable.
	GRenderTargetPool.TransitionTargetsWritable(RHICmdList);

	// Find the visible primitives.
	InitViews(RHICmdList);

	if (GRHINeedsExtraDeletionLatency || !GRHICommandList.Bypass())
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_FMobileSceneRenderer_PostInitViewsFlushDel);
		// we will probably stall on occlusion queries, so might as well have the RHI thread and GPU work while we wait.
		// Also when doing RHI thread this is the only spot that will process pending deletes
		FRHICommandListExecutor::GetImmediateCommandList().PollOcclusionQueries();
		FRHICommandListExecutor::GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
	}

	GEngine->GetPreRenderDelegate().Broadcast();

	// Dynamic vertex and index buffers need to be committed before rendering.
	FGlobalDynamicVertexBuffer::Get().Commit();
	FGlobalDynamicIndexBuffer::Get().Commit();
	RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);

	// Notify the FX system that the scene is about to be rendered.
	if (Scene->FXSystem && !Views[0].bIsPlanarReflection && ViewFamily.EngineShowFlags.Particles)
	{
		Scene->FXSystem->PreRender(RHICmdList, NULL);
	}
	FRHICommandListExecutor::GetImmediateCommandList().PollOcclusionQueries();
	RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);

	RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Shadows));

	RenderShadowDepthMaps(RHICmdList);
	FRHICommandListExecutor::GetImmediateCommandList().PollOcclusionQueries();
	RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);

	// This might eventually be a problem with multiple views.
	// Using only view 0 to check to do on-chip transform of alpha.
	FViewInfo& View = Views[0];

	// Default view list
	TArray<const FViewInfo*> ViewList;
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++) 
	{
		if (Views[ViewIndex].StereoPass != eSSP_MONOSCOPIC_EYE)
		{
			ViewList.Add(&Views[ViewIndex]);
		}
	}

	const bool bGammaSpace = !IsMobileHDR();
	
	const FIntPoint RenderTargetSize = (ViewFamily.RenderTarget->GetRenderTargetTexture().IsValid()) ? ViewFamily.RenderTarget->GetRenderTargetTexture()->GetSizeXY() : ViewFamily.RenderTarget->GetSizeXY();
	const bool bRequiresUpscale = ((int32)RenderTargetSize.X > FamilySize.X || (int32)RenderTargetSize.Y > FamilySize.Y);

	// ES2 requires that the back buffer and depth match dimensions.
	// For the most part this is not the case when using scene captures. Thus scene captures always render to scene color target.
	const bool bStereoRenderingAndHMD = View.Family->EngineShowFlags.StereoRendering && View.Family->EngineShowFlags.HMDDistortion;
	const bool bRenderToSceneColor = bStereoRenderingAndHMD || bRequiresUpscale || FSceneRenderer::ShouldCompositeEditorPrimitives(View) || View.bIsSceneCapture || View.bIsReflectionCapture;

	if (!bGammaSpace)
	{
		RenderCustomDepthPass(RHICmdList);
	}

	//FTextureRHIParamRef SceneColor = nullptr;

	// #todo-renderpasses clean this up
	bool bIsRenderingSceneColor = false;
	
	if (bGammaSpace && !bRenderToSceneColor)
	{
		FTextureRHIParamRef SceneColor = GetMultiViewSceneColor(SceneContext);
		const FTextureRHIParamRef SceneDepth = (View.bIsMobileMultiViewEnabled) ? SceneContext.MobileMultiViewSceneDepthZ->GetRenderTargetItem().TargetableTexture : static_cast<FTextureRHIRef>(SceneContext.GetSceneDepthTexture());

		FRHIRenderPassInfo RPInfo(SceneColor, ERenderTargetActions::Clear_Store);
		RPInfo.DepthStencilRenderTarget.Action = EDepthStencilTargetActions::ClearDepthStencil_StoreDepthStencil;
		RPInfo.DepthStencilRenderTarget.DepthStencilTarget = SceneDepth;
		RPInfo.DepthStencilRenderTarget.ExclusiveDepthStencil = FExclusiveDepthStencil::DepthWrite_StencilWrite;

		RHICmdList.BeginRenderPass(RPInfo, TEXT("MultiviewRender"));
	}
	else
	{
		if (IsVulkanPlatform(ViewFamily.GetShaderPlatform()) || 
			IsOpenGLPlatform(ViewFamily.GetShaderPlatform()) ||
			IsSwitchPlatform(ViewFamily.GetShaderPlatform()))
		{	
			FTextureRHIParamRef SceneColor = SceneContext.GetSceneColorSurface();
			// decals/translucency need to read/test depth
			// TODO: Vulkan add sub-passes! this is expensive, especially with MSAA
			EDepthStencilTargetActions DepthStoreAction = EDepthStencilTargetActions::ClearDepthStencil_StoreDepthStencil;
						
			FRHIRenderPassInfo RPInfo(
				SceneColor,
				ERenderTargetActions::Clear_Store,
				SceneContext.GetSceneDepthSurface(),
				DepthStoreAction, 
				FExclusiveDepthStencil::DepthWrite_StencilWrite
			);
			RPInfo.NumOcclusionQueries = ComputeNumOcclusionQueriesToBatch();
			RPInfo.bOcclusionQueries = RPInfo.NumOcclusionQueries != 0;
			RHICmdList.BeginRenderPass(RPInfo, TEXT("BasePass"));
		}
		else
		{
			// Begin rendering to scene color
			SceneContext.BeginRenderingSceneColor(RHICmdList, ESimpleRenderTargetMode::EClearColorAndDepth);
			/*SceneColor = SceneContext.GetSceneColorSurface();*/
			bIsRenderingSceneColor = true;
		}
	}

	if (GIsEditor && !View.bIsSceneCapture)
	{
		DrawClearQuad(RHICmdList, Views[0].BackgroundColor);
	}
	
	RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_BasePass));

	RenderMobileBasePass(RHICmdList, ViewList);

	RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Occlusion));
	// Issue occlusion queries
	RenderOcclusion(RHICmdList);
	
	RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Post));

	if (bIsRenderingSceneColor)
	{
		SceneContext.FinishRenderingSceneColor(RHICmdList);
	}

	// #todo-renderpasses convert everything and call the correct FinishRenderingScene/Occlusion/etc
	if (RHICmdList.IsInsideRenderPass())
	{
		// END BasePass
		RHICmdList.EndRenderPass();
	}
	else
	{
		// #todo-renderpasses should never get here. Remove once everything is converted.
		UnbindRenderTargets(RHICmdList);
	}
	
	// this causes decal flicker on Adreno for some reason, FORT-112023
	//FRHICommandListExecutor::GetImmediateCommandList().PollOcclusionQueries();

	RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);

	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_FMobileSceneRenderer_ViewExtensionPostRenderBasePass);
		for (int32 ViewExt = 0; ViewExt < ViewFamily.ViewExtensions.Num(); ++ViewExt)
		{
			for (int32 ViewIndex = 0; ViewIndex < ViewFamily.Views.Num(); ++ViewIndex)
			{
				ViewFamily.ViewExtensions[ViewExt]->PostRenderBasePass_RenderThread(RHICmdList, Views[ViewIndex]);
			}
		}
	}

	// Make a copy of the scene depth if the current hardware doesn't support reading and writing to the same depth buffer
	ConditionalResolveSceneDepth(RHICmdList, View);
	
	if (ViewFamily.EngineShowFlags.Decals && !View.bIsPlanarReflection)
	{
		RenderDecals(RHICmdList);
	}

	// Notify the FX system that opaque primitives have been rendered.
	if (Scene->FXSystem && !Views[0].bIsPlanarReflection && ViewFamily.EngineShowFlags.Particles)
	{
		FMobileSceneTextureUniformParameters MobileSceneTextureParameters;
		SetupMobileSceneTextureUniformParameters(SceneContext, FeatureLevel, true, MobileSceneTextureParameters);
		TUniformBufferRef<FMobileSceneTextureUniformParameters> MobileSceneTextureUniformBuffer = TUniformBufferRef<FMobileSceneTextureUniformParameters>::CreateUniformBufferImmediate(MobileSceneTextureParameters, UniformBuffer_SingleFrame);

		// This is switching to another RT!
		Scene->FXSystem->PostRenderOpaque(RHICmdList, View.ViewUniformBuffer, &FMobileSceneTextureUniformParameters::StaticStructMetadata, MobileSceneTextureUniformBuffer.GetReference());
	}

	if (!View.bIsPlanarReflection)
	{
		RenderModulatedShadowProjections(RHICmdList);
	}
	
	// Draw translucency.
	if (ViewFamily.EngineShowFlags.Translucency)
	{
		SCOPE_CYCLE_COUNTER(STAT_TranslucencyDrawTime);
		RenderTranslucency(RHICmdList, ViewList, !bGammaSpace || bRenderToSceneColor);
		FRHICommandListExecutor::GetImmediateCommandList().PollOcclusionQueries();
		RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
	}

	if (ViewFamily.IsMonoscopicFarFieldEnabled() && ViewFamily.Views.Num() == 3)
	{
		TArray<const FViewInfo*> MonoViewList;
		MonoViewList.Add(&Views[2]);

		RenderMonoscopicFarFieldMask(RHICmdList);
		RenderMobileBasePass(RHICmdList, MonoViewList);
		RenderTranslucency(RHICmdList, MonoViewList, !bGammaSpace || bRenderToSceneColor);
		CompositeMonoscopicFarField(RHICmdList);
	}

	if (!View.bIsMobileMultiViewDirectEnabled)
	{
		CopyMobileMultiViewSceneColor(RHICmdList);
	}

	static const auto CVarMobileMSAA = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MobileMSAA"));
	bool bOnChipPP =
		GSupportsRenderTargetFormat_PF_FloatRGBA &&
		GSupportsShaderFramebufferFetch &&
		ViewFamily.EngineShowFlags.PostProcessing;
	bool bOnChipSunMask = bOnChipPP && (View.bLightShaftUse || GetMobileDepthOfFieldScale(View) > 0.0) && !IsMobileDistortionActive(View);
	bool bOnChipPreResolveMSAA = bOnChipPP && IsMetalMobilePlatform(ViewFamily.GetShaderPlatform()) && (CVarMobileMSAA ? CVarMobileMSAA->GetValueOnAnyThread() > 1 : false);

	if (!bGammaSpace && (bOnChipSunMask || bOnChipPreResolveMSAA))
	{
		// Convert alpha from depth to circle of confusion with sunshaft intensity.
		// This is done before resolve on hardware with framebuffer fetch.
		// This will break when PrePostSourceViewportSize is not full size.
		FIntPoint PrePostSourceViewportSize = SceneContext.GetBufferSizeXY();

		FMemMark Mark(FMemStack::Get());
		FRenderingCompositePassContext CompositeContext(RHICmdList, View);

		FRenderingCompositePass* PostProcessSunMask = CompositeContext.Graph.RegisterPass(new(FMemStack::Get()) FRCPassPostProcessSunMaskES2(PrePostSourceViewportSize, true));
		CompositeContext.Process(PostProcessSunMask, TEXT("OnChipAlphaTransform"));
	}

	bool bKeepDepthContent = false;

	if (!bGammaSpace || bRenderToSceneColor)
	{
		// Resolve the scene color for post processing.
		RHICmdList.CopyToResolveTarget(SceneContext.GetSceneColorSurface(), SceneContext.GetSceneColorTexture(), FResolveRect(0, 0, FamilySize.X, FamilySize.Y));

		// On PowerVR we see flickering of shadows and depths not updating correctly if targets are discarded.
		// See CVarMobileForceDepthResolve use in ConditionalResolveSceneDepth.
		const bool bForceDepthResolve = CVarMobileForceDepthResolve.GetValueOnRenderThread() == 1;
		const bool bSeparateTranslucencyActive = IsMobileSeparateTranslucencyActive(View);

		bKeepDepthContent = bForceDepthResolve || bSeparateTranslucencyActive ||
			(View.bIsSceneCapture && (ViewFamily.SceneCaptureSource == ESceneCaptureSource::SCS_SceneColorHDR || ViewFamily.SceneCaptureSource == ESceneCaptureSource::SCS_SceneColorSceneDepth));
	}

	// Drop depth and stencil before post processing to avoid export.
	if (!bKeepDepthContent)
	{
		RHICmdList.DiscardRenderTargets(true, true, 0);
	}

	if (DoOcclusionQueries(FeatureLevel))
	{
		// Optionally hint submission later to avoid render pass churn but delay query results
		const bool bSubmissionAfterTranslucency = (CVarMobileMoveSubmissionHintAfterTranslucency.GetValueOnRenderThread() == 1);
		if (bSubmissionAfterTranslucency)
		{
			RHICmdList.SubmitCommandsHint();
			FRHICommandListExecutor::GetImmediateCommandList().PollOcclusionQueries();
			RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
		}
	}
	
	RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Post));

	if (ViewFamily.bResolveScene)
	{
		if (!bGammaSpace)
		{
			// Finish rendering for each view, or the full stereo buffer if enabled
			{
				SCOPED_DRAW_EVENT(RHICmdList, PostProcessing);
				SCOPE_CYCLE_COUNTER(STAT_FinishRenderViewTargetTime);
				for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
				{
					SCOPED_CONDITIONAL_DRAW_EVENTF(RHICmdList, EventView, Views.Num() > 1, TEXT("View%d"), ViewIndex);
					GPostProcessing.ProcessES2(RHICmdList, Scene, Views[ViewIndex], bOnChipSunMask);
				}
			}
		}
		else if (bRenderToSceneColor)
		{
			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
			{
				BasicPostProcess(RHICmdList, Views[ViewIndex], bRequiresUpscale, FSceneRenderer::ShouldCompositeEditorPrimitives(Views[ViewIndex]));
			}
		}
	}

	RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_SceneEnd));

	RenderFinish(RHICmdList);

	FRHICommandListExecutor::GetImmediateCommandList().PollOcclusionQueries();
	FRHICommandListExecutor::GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
}

// Perform simple upscale and/or editor primitive composite if the fully-featured post process is not in use.
void FMobileSceneRenderer::BasicPostProcess(FRHICommandListImmediate& RHICmdList, FViewInfo &View, bool bDoUpscale, bool bDoEditorPrimitives)
{
	FRenderingCompositePassContext CompositeContext(RHICmdList, View);
	FPostprocessContext Context(RHICmdList, CompositeContext.Graph, View);

	const bool bBlitRequired = !bDoUpscale && !bDoEditorPrimitives;

	if (bDoUpscale || bBlitRequired)
	{	// blit from sceneRT to view family target, simple bilinear if upscaling otherwise point filtered.
		uint32 UpscaleQuality = bDoUpscale ? 1 : 0;
		FRenderingCompositePass* Node = Context.Graph.RegisterPass(new(FMemStack::Get()) FRCPassPostProcessUpscaleES2(View, UpscaleQuality, false));

		Node->SetInput(ePId_Input0, FRenderingCompositeOutputRef(Context.FinalOutput));
		Node->SetInput(ePId_Input1, FRenderingCompositeOutputRef(Context.FinalOutput));

		Context.FinalOutput = FRenderingCompositeOutputRef(Node);
	}

#if WITH_EDITOR
	// Composite editor primitives if we had any to draw and compositing is enabled
	if (bDoEditorPrimitives)
	{
		FRenderingCompositePass* EditorCompNode = Context.Graph.RegisterPass(new(FMemStack::Get()) FRCPassPostProcessCompositeEditorPrimitives(false));
		EditorCompNode->SetInput(ePId_Input0, FRenderingCompositeOutputRef(Context.FinalOutput));
		//Node->SetInput(ePId_Input1, FRenderingCompositeOutputRef(Context.SceneDepth));
		Context.FinalOutput = FRenderingCompositeOutputRef(EditorCompNode);
	}
#endif

	bool bStereoRenderingAndHMD = View.Family->EngineShowFlags.StereoRendering && View.Family->EngineShowFlags.HMDDistortion;
	if (bStereoRenderingAndHMD)
	{
		const IHeadMountedDisplay* HMD =  GEngine->XRSystem->GetHMDDevice();
		checkf(HMD, TEXT("EngineShowFlags.HMDDistortion can not be true when IXRTrackingSystem::GetHMDDevice returns null"));
		FRenderingCompositePass* Node = Context.Graph.RegisterPass(new FRCPassPostProcessHMD());

		if (Node)
		{
			Node->SetInput(ePId_Input0, FRenderingCompositeOutputRef(Context.FinalOutput));
			Context.FinalOutput = FRenderingCompositeOutputRef(Node);
		}
	}

	// currently created on the heap each frame but View.Family->RenderTarget could keep this object and all would be cleaner
	TRefCountPtr<IPooledRenderTarget> Temp;
	FSceneRenderTargetItem Item;
	Item.TargetableTexture = (FTextureRHIRef&)View.Family->RenderTarget->GetRenderTargetTexture();
	Item.ShaderResourceTexture = (FTextureRHIRef&)View.Family->RenderTarget->GetRenderTargetTexture();

	FPooledRenderTargetDesc Desc;

	Desc.Extent = View.Family->RenderTarget->GetSizeXY();
	// todo: this should come from View.Family->RenderTarget
	Desc.Format = PF_B8G8R8A8;
	Desc.NumMips = 1;

	GRenderTargetPool.CreateUntrackedElement(Desc, Temp, Item);

	Context.FinalOutput.GetOutput()->PooledRenderTarget = Temp;
	Context.FinalOutput.GetOutput()->RenderTargetDesc = Desc;

	CompositeContext.Process(Context.FinalOutput.GetPass(), TEXT("ES2BasicPostProcess"));
}

void FMobileSceneRenderer::RenderOcclusion(FRHICommandListImmediate& RHICmdList)
{
	if (!DoOcclusionQueries(FeatureLevel))
	{
		return;
	}

	BeginOcclusionTests(RHICmdList, true);
	FenceOcclusionTests(RHICmdList);

	// Optionally hint submission later to avoid render pass churn but delay query results
	const bool bSubmissionAfterTranslucency = (CVarMobileMoveSubmissionHintAfterTranslucency.GetValueOnRenderThread() == 1);
	if (!bSubmissionAfterTranslucency)
	{	
		RHICmdList.SubmitCommandsHint();
	}
}

int32 FMobileSceneRenderer::ComputeNumOcclusionQueriesToBatch() const
{
	int32 NumQueriesForBatch = 0;
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		const FViewInfo& View = Views[ViewIndex];
		const FSceneViewState* ViewState = (FSceneViewState*)View.State;
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		if (!ViewState || (!ViewState->HasViewParent() && !ViewState->bIsFrozen))
#endif
		{
			NumQueriesForBatch += View.IndividualOcclusionQueries.GetNumBatchOcclusionQueries();
			NumQueriesForBatch += View.GroupedOcclusionQueries.GetNumBatchOcclusionQueries();
		}
	}
	
	return NumQueriesForBatch;
}

void FMobileSceneRenderer::ConditionalResolveSceneDepth(FRHICommandListImmediate& RHICmdList, const FViewInfo& View)
{
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);
	SceneContext.ResolveSceneDepthToAuxiliaryTexture(RHICmdList);
	
	if (IsSimulatedPlatform(ShaderPlatform)) // mobile emulation on PC
	{
		// resolve MSAA depth for translucency
		SceneContext.ResolveSceneDepthTexture(RHICmdList, FResolveRect(0, 0, FamilySize.X, FamilySize.Y));
	}
	else if ((IsMobileHDR() || IsHTML5Platform())
		&& IsMobilePlatform(ShaderPlatform) 
		&& !IsMetalPlatform(ShaderPlatform) // iOS supports framebuffer fetch
		&& !IsVulkanPlatform(ShaderPlatform) // don't need this on vulkan?
		&& !IsPCPlatform(ShaderPlatform) // exclude mobile emulation on PC
		&& !View.bIsPlanarReflection)	// exclude depth resolve from planar reflection captures, can't do it reliably more than once per frame
	{
		bool bSceneDepthInAlpha = (SceneContext.GetSceneColor()->GetDesc().Format == PF_FloatRGBA);
		bool bOnChipDepthFetch = (GSupportsShaderDepthStencilFetch || (GSupportsShaderFramebufferFetch && bSceneDepthInAlpha));

		const bool bAlwaysResolveDepth = CVarMobileAlwaysResolveDepth.GetValueOnRenderThread() == 1;
		
		static const auto CVarMobileMSAA = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MobileMSAA"));
		const bool bMobileMSAA = (CVarMobileMSAA ? CVarMobileMSAA->GetValueOnAnyThread() > 1 : false);
		
		if (!bOnChipDepthFetch || bAlwaysResolveDepth)
		{
			// Only these features require depth texture
			bool bDecals = ViewFamily.EngineShowFlags.Decals && Scene->Decals.Num();
			bool bModulatedShadows = ViewFamily.EngineShowFlags.DynamicShadows && bModulatedShadowsInUse;

			if (bDecals || bModulatedShadows || bAlwaysResolveDepth || View.bUsesSceneDepth)
			{
				SCOPED_DRAW_EVENT(RHICmdList, ConditionalResolveSceneDepth);

				// WEBGL copies depth from SceneColor alpha to a separate texture
				if (ShaderPlatform == SP_OPENGL_ES2_WEBGL)
				{
					if (bSceneDepthInAlpha)
					{
						CopySceneAlpha(RHICmdList, View);
					}
				}
				else if (!bMobileMSAA)
				{
					// Switch target to force hardware flush current depth to texture
					FTextureRHIRef DummySceneColor = GSystemTextures.BlackDummy->GetRenderTargetItem().TargetableTexture;
					FTextureRHIRef DummyDepthTarget = GSystemTextures.DepthDummy->GetRenderTargetItem().TargetableTexture;

					if (CVarMobileForceDepthResolve.GetValueOnRenderThread() != 0)
					{
						FRHIRenderPassInfo RPInfo(DummySceneColor, ERenderTargetActions::DontLoad_DontStore);
						RPInfo.DepthStencilRenderTarget.Action = EDepthStencilTargetActions::ClearDepthStencil_StoreDepthStencil;
						RPInfo.DepthStencilRenderTarget.DepthStencilTarget = DummyDepthTarget;
						RPInfo.DepthStencilRenderTarget.ExclusiveDepthStencil = FExclusiveDepthStencil::DepthWrite_StencilWrite;
						RHICmdList.BeginRenderPass(RPInfo, TEXT("ResolveDepth"));
					{
						FGraphicsPipelineStateInitializer GraphicsPSOInit;
						RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
						GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
						GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
						GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

						// for devices that do not support framebuffer fetch we rely on undocumented behavior:
						// Depth reading features will have the depth bound as an attachment AND as a sampler this means
						// some driver implementations will ignore our attempts to resolve, here we draw with the depth texture to force a resolve.
						// See UE-37809 for a description of the desired fix.
						// The results of this draw are irrelevant.
						TShaderMapRef<FScreenVS> ScreenVertexShader(View.ShaderMap);
						TShaderMapRef<FScreenPS> PixelShader(View.ShaderMap);

						GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
						GraphicsPSOInit.BoundShaderState.VertexShaderRHI = GETSAFERHISHADER_VERTEX(*ScreenVertexShader);
						GraphicsPSOInit.BoundShaderState.PixelShaderRHI = GETSAFERHISHADER_PIXEL(*PixelShader);
						GraphicsPSOInit.PrimitiveType = PT_TriangleList;

						SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

						ScreenVertexShader->SetParameters(RHICmdList, View.ViewUniformBuffer);
						PixelShader->SetParameters(RHICmdList, TStaticSamplerState<SF_Point>::GetRHI(), SceneContext.GetSceneDepthTexture());
						DrawRectangle(
							RHICmdList,
							0, 0,
							0, 0,
							0, 0,
							1, 1,
							FIntPoint(1, 1),
							FIntPoint(1, 1),
							*ScreenVertexShader,
							EDRF_UseTriangleOptimization);
					}
						RHICmdList.EndRenderPass();
					} // force depth resolve
				}
			}
		}
	}
}

void FMobileSceneRenderer::UpdateOpaqueBasePassUniformBuffer(FRHICommandListImmediate& RHICmdList, const FViewInfo& View)
{
	FMobileBasePassUniformParameters Parameters;
	SetupMobileBasePassUniformParameters(RHICmdList, View, false, Parameters);
	Scene->UniformBuffers.MobileOpaqueBasePassUniformBuffer.UpdateUniformBufferImmediate(Parameters);
}

void FMobileSceneRenderer::UpdateTranslucentBasePassUniformBuffer(FRHICommandListImmediate& RHICmdList, const FViewInfo& View)
{
	FMobileBasePassUniformParameters Parameters;
	SetupMobileBasePassUniformParameters(RHICmdList, View, true, Parameters);
	Scene->UniformBuffers.MobileTranslucentBasePassUniformBuffer.UpdateUniformBufferImmediate(Parameters);
}

void FMobileSceneRenderer::UpdateDirectionalLightUniformBuffers(FRHICommandListImmediate& RHICmdList, const FViewInfo& View)
{
	bool bDynamicShadows = ViewFamily.EngineShowFlags.DynamicShadows;
	// Fill in the other entries based on the lights
	for (int32 ChannelIdx = 0; ChannelIdx < ARRAY_COUNT(Scene->MobileDirectionalLights); ChannelIdx++)
	{
		FMobileDirectionalLightShaderParameters Params;
		SetupMobileDirectionalLightUniformParameters(*Scene, View, VisibleLightInfos, ChannelIdx, bDynamicShadows, Params);
		Scene->UniformBuffers.MobileDirectionalLightUniformBuffers[ChannelIdx + 1].UpdateUniformBufferImmediate(Params);
	}
}

void FMobileSceneRenderer::CreateDirectionalLightUniformBuffers(FViewInfo& View)
{
	bool bDynamicShadows = ViewFamily.EngineShowFlags.DynamicShadows;
	// First array entry is used for primitives with no lighting channel set
	View.MobileDirectionalLightUniformBuffers[0] = TUniformBufferRef<FMobileDirectionalLightShaderParameters>::CreateUniformBufferImmediate(FMobileDirectionalLightShaderParameters(), UniformBuffer_SingleFrame);
	// Fill in the other entries based on the lights
	for (int32 ChannelIdx = 0; ChannelIdx < ARRAY_COUNT(Scene->MobileDirectionalLights); ChannelIdx++)
	{
		FMobileDirectionalLightShaderParameters Params;
		SetupMobileDirectionalLightUniformParameters(*Scene, View, VisibleLightInfos, ChannelIdx, bDynamicShadows, Params);
		View.MobileDirectionalLightUniformBuffers[ChannelIdx + 1] = TUniformBufferRef<FMobileDirectionalLightShaderParameters>::CreateUniformBufferImmediate(Params, UniformBuffer_SingleFrame);
	}
}

class FCopyMobileMultiViewSceneColorPS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FCopyMobileMultiViewSceneColorPS, Global);
public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;;
	}

	FCopyMobileMultiViewSceneColorPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FGlobalShader(Initializer)
	{
		MobileMultiViewSceneColorTexture.Bind(Initializer.ParameterMap, TEXT("MobileMultiViewSceneColorTexture"));
		MobileMultiViewSceneColorTextureSampler.Bind(Initializer.ParameterMap, TEXT("MobileMultiViewSceneColorTextureSampler"));
	}

	FCopyMobileMultiViewSceneColorPS() {}

	void SetParameters(FRHICommandList& RHICmdList, const FUniformBufferRHIParamRef ViewUniformBuffer, FTextureRHIRef InMobileMultiViewSceneColorTexture)
	{
		const FPixelShaderRHIParamRef ShaderRHI = GetPixelShader();
		FGlobalShader::SetParameters<FViewUniformShaderParameters>(RHICmdList, ShaderRHI, ViewUniformBuffer);
		SetTextureParameter(
			RHICmdList,
			ShaderRHI,
			MobileMultiViewSceneColorTexture,
			MobileMultiViewSceneColorTextureSampler,
			TStaticSamplerState<SF_Bilinear>::GetRHI(),
			InMobileMultiViewSceneColorTexture);
	}

	virtual bool Serialize(FArchive& Ar) override
	{
		const bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << MobileMultiViewSceneColorTexture;
		Ar << MobileMultiViewSceneColorTextureSampler;
		return bShaderHasOutdatedParameters;
	}

	FShaderResourceParameter MobileMultiViewSceneColorTexture;
	FShaderResourceParameter MobileMultiViewSceneColorTextureSampler;
};

IMPLEMENT_SHADER_TYPE(, FCopyMobileMultiViewSceneColorPS, TEXT("/Engine/Private/MobileMultiView.usf"), TEXT("MainPS"), SF_Pixel);

void FMobileSceneRenderer::CopyMobileMultiViewSceneColor(FRHICommandListImmediate& RHICmdList)
{
	if (!Views[0].bIsMobileMultiViewEnabled)
	{
		return;
	}

	RHICmdList.DiscardRenderTargets(true, true, 0);

	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

	// Switching from the multi-view scene color render target array to side by side scene color
	FRHIRenderPassInfo RPInfo(ViewFamily.RenderTarget->GetRenderTargetTexture(), ERenderTargetActions::Clear_Store);
	RPInfo.DepthStencilRenderTarget.Action = EDepthStencilTargetActions::ClearDepthStencil_DontStoreDepthStencil;
	RPInfo.DepthStencilRenderTarget.DepthStencilTarget = SceneContext.GetSceneDepthTexture();
	RPInfo.DepthStencilRenderTarget.ExclusiveDepthStencil = FExclusiveDepthStencil::DepthNop_StencilNop;

	TransitionRenderPassTargets(RHICmdList, RPInfo);
	RHICmdList.BeginRenderPass(RPInfo, TEXT("CopyMobileMultiViewColor"));
	{
	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
	GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

	const auto ShaderMap = GetGlobalShaderMap(FeatureLevel);
	TShaderMapRef<FScreenVS> VertexShader(ShaderMap);
	TShaderMapRef<FCopyMobileMultiViewSceneColorPS> PixelShader(ShaderMap);
	extern TGlobalResource<FFilterVertexDeclaration> GFilterVertexDeclaration;

	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = GETSAFERHISHADER_VERTEX(*VertexShader);
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = GETSAFERHISHADER_PIXEL(*PixelShader);
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		const FViewInfo& View = Views[ViewIndex];

		// Multi-view color target is our input texture array
		PixelShader->SetParameters(RHICmdList, View.ViewUniformBuffer, SceneContext.MobileMultiViewSceneColor->GetRenderTargetItem().ShaderResourceTexture);

		RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f, View.ViewRect.Min.X + View.ViewRect.Width(), View.ViewRect.Min.Y + View.ViewRect.Height(), 1.0f);
		const FIntPoint TargetSize(View.ViewRect.Width(), View.ViewRect.Height());

		DrawRectangle(
			RHICmdList,
			0, 0,
			View.ViewRect.Width(), View.ViewRect.Height(),
			0, 0,
			View.ViewRect.Width(), View.ViewRect.Height(),
			TargetSize,
			TargetSize,
			*VertexShader,
			EDRF_UseTriangleOptimization);
	}
	}
	RHICmdList.EndRenderPass();
}
