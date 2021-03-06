// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	DeferredShadingRenderer.cpp: Top level rendering loop for deferred shading
=============================================================================*/

#include "RendererPrivate.h"
#include "Engine.h"
#include "ScenePrivate.h"
#include "ScreenRendering.h"
#include "SceneFilterRendering.h"
#include "VisualizeTexture.h"
#include "CompositionLighting.h"
#include "FXSystem.h"
#include "OneColorShader.h"
#include "CompositionLighting/PostProcessDeferredDecals.h"
#include "LightPropagationVolume.h"
#include "EngineModule.h"

TAutoConsoleVariable<int32> CVarEarlyZPass(
	TEXT("r.EarlyZPass"),
	3,	
	TEXT("Whether to use a depth only pass to initialize Z culling for the base pass. Cannot be changed at runtime.\n")
	TEXT("Note: also look at r.EarlyZPassMovable\n")
	TEXT("  0: off\n")
	TEXT("  1: only if not masked, and only if large on the screen\n")
	TEXT("  2: all opaque (including masked)\n")
	TEXT("  x: use built in heuristic (default is 3)\n"),
	ECVF_Default);

int32 GEarlyZPassMovable = 0;

/** Affects static draw lists so must reload level to propagate. */
static FAutoConsoleVariableRef CVarEarlyZPassMovable(
	TEXT("r.EarlyZPassMovable"),
	GEarlyZPassMovable,
	TEXT("Whether to render movable objects into the depth only pass.  Movable objects are typically not good occluders so this defaults to off.\n")
	TEXT("Note: also look at r.EarlyZPass"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
	);

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
static TAutoConsoleVariable<int32> CVarTestUIBlur(
	TEXT("UI.TestUIBlur"),
	0,
	TEXT("Adds a 20,20 - 400,400 UIBlur rectangle to test the feature quickly.\n")
	TEXT("0: off (default)\n")
	TEXT("1: on\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVisualizeTexturePool(
	TEXT("r.VisualizeTexturePool"),
	0,
	TEXT("Allows to enable the visualize the texture pool (currently only on console).\n")
	TEXT(" 0: off\n")
	TEXT(" 1: on"),
	ECVF_Cheat | ECVF_RenderThreadSafe);
#endif

/*-----------------------------------------------------------------------------
	FDeferredShadingSceneRenderer
-----------------------------------------------------------------------------*/

FDeferredShadingSceneRenderer::FDeferredShadingSceneRenderer(const FSceneViewFamily* InViewFamily,FHitProxyConsumer* HitProxyConsumer)
	: FSceneRenderer(InViewFamily, HitProxyConsumer)
	, EarlyZPassMode(DDM_NonMaskedOnly)
	, TranslucentSelfShadowLayout(0, 0, 0, 0)
	, CachedTranslucentSelfShadowLightId(INDEX_NONE)
{
	if (FPlatformProperties::SupportsWindowedMode())
	{
		// Use a depth only pass if we are using full blown HQ lightmaps
		// Otherwise base pass pixel shaders will be cheap and there will be little benefit to rendering a depth only pass
		if (AllowHighQualityLightmaps(FeatureLevel) || !ViewFamily.EngineShowFlags.Lighting)
		{
			EarlyZPassMode = DDM_None;
		}
	}

	// Shader complexity requires depth only pass to display masked material cost correctly
	if (ViewFamily.EngineShowFlags.ShaderComplexity)
	{
		EarlyZPassMode = DDM_AllOccluders;
	}

	// developer override, good for profiling, can be useful as project setting
	{
		static const auto ICVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.EarlyZPass"));
		const int32 CVarValue = ICVar->GetValueOnGameThread();

		switch(CVarValue)
		{
			case 0: EarlyZPassMode = DDM_None; break;
			case 1: EarlyZPassMode = DDM_NonMaskedOnly; break;
			case 2: EarlyZPassMode = DDM_AllOccluders; break;
		}
	}
}

/** 
* Clears a view. 
*/
void FDeferredShadingSceneRenderer::ClearView(FRHICommandListImmediate& RHICmdList)
{
	// Clear the G Buffer render targets
	const bool bClearBlack = Views[0].Family->EngineShowFlags.ShaderComplexity || Views[0].Family->EngineShowFlags.StationaryLightOverlap;
	GSceneRenderTargets.ClearGBufferTargets(RHICmdList, bClearBlack ? FLinearColor(0, 0, 0, 0) : Views[0].BackgroundColor);
}

namespace
{
	FVector4 ClearQuadVertices[4] = 
	{
		FVector4( -1.0f,  1.0f, 0.0f, 1.0f ),
		FVector4(  1.0f,  1.0f, 0.0f, 1.0f ),
		FVector4( -1.0f, -1.0f, 0.0f, 1.0f ),
		FVector4(  1.0f, -1.0f, 0.0f, 1.0f )
	};
}

extern FGlobalBoundShaderState GClearMRTBoundShaderState[8];

/** 
* Clears view where Z is still at the maximum value (ie no geometry rendered)
*/
void FDeferredShadingSceneRenderer::ClearGBufferAtMaxZ(FRHICommandList& RHICmdList)
{
	// Assumes BeginRenderingSceneColor() has been called before this function
	SCOPED_DRAW_EVENT(ClearGBufferAtMaxZ, DEC_SCENE_ITEMS);

	// Clear the G Buffer render targets
	const bool bClearBlack = Views[0].Family->EngineShowFlags.ShaderComplexity || Views[0].Family->EngineShowFlags.StationaryLightOverlap;
	// Same clear color from RHIClearMRT
	FLinearColor ClearColors[6] = 
		{bClearBlack ? FLinearColor(0,0,0,0) : Views[0].BackgroundColor, FLinearColor(0.5f,0.5f,0.5f,0), FLinearColor(0,0,0,1), FLinearColor(0,0,0,0), FLinearColor(0,1,1,1), FLinearColor(1,1,1,1)};

	uint32 NumActiveRenderTargets = GSceneRenderTargets.GetNumGBufferTargets();
	
	TShaderMapRef<TOneColorVS<true> > VertexShader(GetGlobalShaderMap());
	FOneColorPS* PixelShader = NULL; 

	// Assume for now all code path supports SM4, otherwise render target numbers are changed
	switch(NumActiveRenderTargets)
	{
	case 5:
		{
			TShaderMapRef<TOneColorPixelShaderMRT<5> > MRTPixelShader(GetGlobalShaderMap());
			PixelShader = *MRTPixelShader;
		}
		break;
	case 6:
		{
			TShaderMapRef<TOneColorPixelShaderMRT<6> > MRTPixelShader(GetGlobalShaderMap());
			PixelShader = *MRTPixelShader;
		}
		break;
	default:
	case 1:
		{
			TShaderMapRef<TOneColorPixelShaderMRT<1> > MRTPixelShader(GetGlobalShaderMap());
			PixelShader = *MRTPixelShader;
		}
		break;
	}

	SetGlobalBoundShaderState(RHICmdList, GClearMRTBoundShaderState[NumActiveRenderTargets - 1], GetVertexDeclarationFVector4(), *VertexShader, PixelShader);

	// Opaque rendering, depth test but no depth writes
	RHICmdList.SetRasterizerState(TStaticRasterizerState<FM_Solid, CM_None>::GetRHI());
	RHICmdList.SetBlendState(TStaticBlendStateWriteMask<>::GetRHI());
	// Note, this is a reversed Z depth surface, using CF_GreaterEqual.
	RHICmdList.SetDepthStencilState(TStaticDepthStencilState<false, CF_GreaterEqual>::GetRHI());

	// Clear each viewport by drawing background color at MaxZ depth
	for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
	{
		SCOPED_CONDITIONAL_DRAW_EVENTF(EventView, Views.Num() > 1, DEC_SCENE_ITEMS, TEXT("ClearView%d"), ViewIndex);

		FViewInfo& View = Views[ViewIndex];

		// Set viewport for this view
		RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);

		// Setup PS
		SetShaderValueArray(RHICmdList, PixelShader->GetPixelShader(),PixelShader->ColorParameter, ClearColors, NumActiveRenderTargets);

		// Render quad
		DrawPrimitiveUP(RHICmdList, PT_TriangleStrip, 2, ClearQuadVertices, sizeof(ClearQuadVertices[0]));
	}
}

bool FDeferredShadingSceneRenderer::RenderBasePassStaticDataMasked(FRHICommandList& RHICmdList, FViewInfo& View)
{
	bool bDirty = false;

	{
		// Draw the scene's base pass draw lists.
		FScene::EBasePassDrawListType MaskedDrawType = FScene::EBasePass_Masked;
		{
			SCOPED_DRAW_EVENT(StaticMaskedNoLightmap, DEC_SCENE_ITEMS);
			bDirty |= Scene->BasePassNoLightMapDrawList[MaskedDrawType].DrawVisible(RHICmdList, View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassSimpleDynamicLightingDrawList[MaskedDrawType].DrawVisible(RHICmdList, View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassCachedVolumeIndirectLightingDrawList[MaskedDrawType].DrawVisible(RHICmdList, View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassCachedPointIndirectLightingDrawList[MaskedDrawType].DrawVisible(RHICmdList, View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
		}
		{
			SCOPED_DRAW_EVENT(StaticMaskedLightmapped, DEC_SCENE_ITEMS);
			bDirty |= Scene->BasePassHighQualityLightMapDrawList[MaskedDrawType].DrawVisible(RHICmdList, View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassDistanceFieldShadowMapLightMapDrawList[MaskedDrawType].DrawVisible(RHICmdList, View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassLowQualityLightMapDrawList[MaskedDrawType].DrawVisible(RHICmdList, View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
		}
	}

	return bDirty;
}

FGraphEventRef FDeferredShadingSceneRenderer::RenderBasePassStaticDataMaskedParallel(FViewInfo& View, int32 Width, FGraphEventRef SubmitChain, bool& OutDirty)
{
	// Draw the scene's base pass draw lists.
	FScene::EBasePassDrawListType MaskedDrawType = FScene::EBasePass_Masked;
	SubmitChain = Scene->BasePassNoLightMapDrawList[MaskedDrawType].DrawVisibleParallel(View, View.StaticMeshVisibilityMap, View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);
	SubmitChain = Scene->BasePassSimpleDynamicLightingDrawList[MaskedDrawType].DrawVisibleParallel(View, View.StaticMeshVisibilityMap, View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);
	SubmitChain = Scene->BasePassCachedVolumeIndirectLightingDrawList[MaskedDrawType].DrawVisibleParallel(View, View.StaticMeshVisibilityMap, View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);
	SubmitChain = Scene->BasePassCachedPointIndirectLightingDrawList[MaskedDrawType].DrawVisibleParallel(View, View.StaticMeshVisibilityMap, View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);

	SubmitChain = Scene->BasePassHighQualityLightMapDrawList[MaskedDrawType].DrawVisibleParallel(View, View.StaticMeshVisibilityMap, View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);
	SubmitChain = Scene->BasePassDistanceFieldShadowMapLightMapDrawList[MaskedDrawType].DrawVisibleParallel(View, View.StaticMeshVisibilityMap, View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);
	SubmitChain = Scene->BasePassLowQualityLightMapDrawList[MaskedDrawType].DrawVisibleParallel(View, View.StaticMeshVisibilityMap, View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);

	return SubmitChain;
}

bool FDeferredShadingSceneRenderer::RenderBasePassStaticDataDefault(FRHICommandList& RHICmdList, FViewInfo& View)
{
	bool bDirty = false;

	{
		FScene::EBasePassDrawListType OpaqueDrawType = FScene::EBasePass_Default;
		{
			SCOPED_DRAW_EVENT(StaticOpaqueNoLightmap, DEC_SCENE_ITEMS);
			bDirty |= Scene->BasePassNoLightMapDrawList[OpaqueDrawType].DrawVisible(RHICmdList, View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassSimpleDynamicLightingDrawList[OpaqueDrawType].DrawVisible(RHICmdList, View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassCachedVolumeIndirectLightingDrawList[OpaqueDrawType].DrawVisible(RHICmdList, View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassCachedPointIndirectLightingDrawList[OpaqueDrawType].DrawVisible(RHICmdList, View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
		}
		{
			SCOPED_DRAW_EVENT(StaticOpaqueLightmapped, DEC_SCENE_ITEMS);
			bDirty |= Scene->BasePassHighQualityLightMapDrawList[OpaqueDrawType].DrawVisible(RHICmdList, View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassDistanceFieldShadowMapLightMapDrawList[OpaqueDrawType].DrawVisible(RHICmdList, View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassLowQualityLightMapDrawList[OpaqueDrawType].DrawVisible(RHICmdList, View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
		}
	}

	return bDirty;
}

FGraphEventRef FDeferredShadingSceneRenderer::RenderBasePassStaticDataDefaultParallel(FViewInfo& View, int32 Width, FGraphEventRef SubmitChain, bool& OutDirty)
{
	FScene::EBasePassDrawListType OpaqueDrawType = FScene::EBasePass_Default;
	SubmitChain = Scene->BasePassNoLightMapDrawList[OpaqueDrawType].DrawVisibleParallel(View, View.StaticMeshVisibilityMap, View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);
	SubmitChain = Scene->BasePassSimpleDynamicLightingDrawList[OpaqueDrawType].DrawVisibleParallel(View, View.StaticMeshVisibilityMap, View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);
	SubmitChain = Scene->BasePassCachedVolumeIndirectLightingDrawList[OpaqueDrawType].DrawVisibleParallel(View, View.StaticMeshVisibilityMap, View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);
	SubmitChain = Scene->BasePassCachedPointIndirectLightingDrawList[OpaqueDrawType].DrawVisibleParallel(View, View.StaticMeshVisibilityMap, View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);

	SubmitChain = Scene->BasePassHighQualityLightMapDrawList[OpaqueDrawType].DrawVisibleParallel(View, View.StaticMeshVisibilityMap, View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);
	SubmitChain = Scene->BasePassDistanceFieldShadowMapLightMapDrawList[OpaqueDrawType].DrawVisibleParallel(View, View.StaticMeshVisibilityMap, View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);
	SubmitChain = Scene->BasePassLowQualityLightMapDrawList[OpaqueDrawType].DrawVisibleParallel(View, View.StaticMeshVisibilityMap, View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);

	return SubmitChain;
}

void FDeferredShadingSceneRenderer::SortBasePassStaticData(FVector ViewPosition)
{
	// If we're not using a depth only pass, sort the static draw list buckets roughly front to back, to maximize HiZ culling
	// Note that this is only a very rough sort, since it does not interfere with state sorting, and each list is sorted separately
	if (EarlyZPassMode == DDM_None)
	{
		SCOPE_CYCLE_COUNTER(STAT_SortStaticDrawLists);

		for (int32 DrawType = 0; DrawType < FScene::EBasePass_MAX; DrawType++)
		{
			Scene->BasePassNoLightMapDrawList[DrawType].SortFrontToBack(ViewPosition);
			Scene->BasePassSimpleDynamicLightingDrawList[DrawType].SortFrontToBack(ViewPosition);
			Scene->BasePassCachedVolumeIndirectLightingDrawList[DrawType].SortFrontToBack(ViewPosition);
			Scene->BasePassCachedPointIndirectLightingDrawList[DrawType].SortFrontToBack(ViewPosition);
			Scene->BasePassHighQualityLightMapDrawList[DrawType].SortFrontToBack(ViewPosition);
			Scene->BasePassDistanceFieldShadowMapLightMapDrawList[DrawType].SortFrontToBack(ViewPosition);
			Scene->BasePassLowQualityLightMapDrawList[DrawType].SortFrontToBack(ViewPosition);
		}
	}
}

/**
* Renders the basepass for the static data of a given View.
*
* @return true if anything was rendered to scene color
*/
bool FDeferredShadingSceneRenderer::RenderBasePassStaticData(FRHICommandList& RHICmdList, FViewInfo& View)
{
	bool bDirty = false;

	SCOPE_CYCLE_COUNTER(STAT_StaticDrawListDrawTime);

	// When using a depth-only pass, the default opaque geometry's depths are already
	// in the depth buffer at this point, so rendering masked next will already cull
	// as efficiently as it can, while also increasing the ZCull efficiency when
	// rendering the default opaque geometry afterward.
	if (EarlyZPassMode != DDM_None)
	{
		bDirty |= RenderBasePassStaticDataMasked(RHICmdList, View);
		bDirty |= RenderBasePassStaticDataDefault(RHICmdList, View);
	}
	else
	{
		// Otherwise, in the case where we're not using a depth-only pre-pass, there
		// is an advantage to rendering default opaque first to help cull the more
		// expensive masked geometry.
		bDirty |= RenderBasePassStaticDataDefault(RHICmdList, View);
		bDirty |= RenderBasePassStaticDataMasked(RHICmdList, View);
	}

	return bDirty;
}

FGraphEventRef FDeferredShadingSceneRenderer::RenderBasePassStaticDataParallel(FViewInfo& View, int32 Width, FGraphEventRef SubmitChain, bool& OutDirty)
{
	SCOPE_CYCLE_COUNTER(STAT_StaticDrawListDrawTime);

	// When using a depth-only pass, the default opaque geometry's depths are already
	// in the depth buffer at this point, so rendering masked next will already cull
	// as efficiently as it can, while also increasing the ZCull efficiency when
	// rendering the default opaque geometry afterward.
	if (EarlyZPassMode != DDM_None)
	{
		SubmitChain = RenderBasePassStaticDataMaskedParallel(View, Width, SubmitChain, OutDirty);
		SubmitChain = RenderBasePassStaticDataDefaultParallel(View, Width, SubmitChain, OutDirty);
	}
	else
	{
		// Otherwise, in the case where we're not using a depth-only pre-pass, there
		// is an advantage to rendering default opaque first to help cull the more
		// expensive masked geometry.
		SubmitChain = RenderBasePassStaticDataDefaultParallel(View, Width, SubmitChain, OutDirty);
		SubmitChain = RenderBasePassStaticDataMaskedParallel(View, Width, SubmitChain, OutDirty);
	}

	return SubmitChain;
}

/**
* Renders the basepass for the dynamic data of a given DPG and View.
*
* @return true if anything was rendered to scene color
*/

void FDeferredShadingSceneRenderer::RenderBasePassDynamicData(FRHICommandList& RHICmdList, const FViewInfo& View, bool& bOutDirty)
	{
	bool bDirty = false;

		SCOPE_CYCLE_COUNTER(STAT_DynamicPrimitiveDrawTime);
		SCOPED_DRAW_EVENT(Dynamic, DEC_SCENE_ITEMS);

		if( View.VisibleDynamicPrimitives.Num() > 0 )
		{
			// Draw the dynamic non-occluded primitives using a base pass drawing policy.
			TDynamicPrimitiveDrawer<FBasePassOpaqueDrawingPolicyFactory> Drawer(
			RHICmdList, &View, FBasePassOpaqueDrawingPolicyFactory::ContextType(false, ESceneRenderTargetsMode::DontSet), true);
			for(int32 PrimitiveIndex = 0, Num = View.VisibleDynamicPrimitives.Num();PrimitiveIndex < Num;PrimitiveIndex++)
			{
				const FPrimitiveSceneInfo* PrimitiveSceneInfo = View.VisibleDynamicPrimitives[PrimitiveIndex];
				int32 PrimitiveId = PrimitiveSceneInfo->GetIndex();
				const FPrimitiveViewRelevance& PrimitiveViewRelevance = View.PrimitiveViewRelevanceMap[PrimitiveId];

				const bool bVisible = View.PrimitiveVisibilityMap[PrimitiveId];

				// Only draw the primitive if it's visible
				if( bVisible && 
					// only draw opaque and masked primitives if wireframe is disabled
					(PrimitiveViewRelevance.bOpaqueRelevance || ViewFamily.EngineShowFlags.Wireframe) &&
					PrimitiveViewRelevance.bRenderInMainPass
					)
				{
					FScopeCycleCounter Context(PrimitiveSceneInfo->Proxy->GetStatId());
					Drawer.SetPrimitive(PrimitiveSceneInfo->Proxy);
					PrimitiveSceneInfo->Proxy->DrawDynamicElements(
						&Drawer,
						&View			
						);
				}
			}
			bDirty |= Drawer.IsDirty(); 
		}

	if( !View.Family->EngineShowFlags.CompositeEditorPrimitives )
	{
		
		const bool bNeedToSwitchVerticalAxis = IsES2Platform(GRHIShaderPlatform) && !IsPCPlatform(GRHIShaderPlatform);

		// Draw the base pass for the view's batched mesh elements.
		bDirty = DrawViewElements<FBasePassOpaqueDrawingPolicyFactory>(RHICmdList, View, FBasePassOpaqueDrawingPolicyFactory::ContextType(false, ESceneRenderTargetsMode::DontSet), SDPG_World, true) || bDirty;

		// Draw the view's batched simple elements(lines, sprites, etc).
		bDirty = View.BatchedViewElements.Draw(RHICmdList, bNeedToSwitchVerticalAxis, View.ViewProjectionMatrix, View.ViewRect.Width(), View.ViewRect.Height(), false) || bDirty;
		
		// Draw foreground objects last
		bDirty = DrawViewElements<FBasePassOpaqueDrawingPolicyFactory>(RHICmdList, View, FBasePassOpaqueDrawingPolicyFactory::ContextType(false, ESceneRenderTargetsMode::DontSet), SDPG_Foreground, true) || bDirty;

		// Draw the view's batched simple elements(lines, sprites, etc).
		bDirty = View.TopBatchedViewElements.Draw(RHICmdList, bNeedToSwitchVerticalAxis, View.ViewProjectionMatrix, View.ViewRect.Width(), View.ViewRect.Height(), false) || bDirty;

	}

	// this little bit of code is required because multiple threads might be writing bOutDirty...this you cannot use || bDirty - type things.
	if (bDirty)
	{
		bOutDirty = true;
	}
}


class FRenderBasePassDynamicDataThreadTask
{
	FDeferredShadingSceneRenderer& ThisRenderer;
	FRHICommandList& RHICmdList;
	const FViewInfo& View;
	bool& OutDirty;

public:

	FRenderBasePassDynamicDataThreadTask(
		FDeferredShadingSceneRenderer* InThisRenderer,
		FRHICommandList* InRHICmdList,
		const FViewInfo* InView,
		bool* InOutDirty
		)
		: ThisRenderer(*InThisRenderer)
		, RHICmdList(*InRHICmdList)
		, View(*InView)
		, OutDirty(*InOutDirty)
	{
	}

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FRenderBasePassDynamicDataThreadTask, STATGROUP_TaskGraphTasks);
	}

	ENamedThreads::Type GetDesiredThread()
	{
		return ENamedThreads::AnyThread;
	}

	static ESubsequentsMode::Type GetSubsequentsMode() { return ESubsequentsMode::TrackSubsequents; }

	void DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
	{
		ThisRenderer.RenderBasePassDynamicData(RHICmdList, View, OutDirty);
	}
};

FGraphEventRef FDeferredShadingSceneRenderer::RenderBasePassDynamicDataParallel(FViewInfo& View, FGraphEventRef SubmitChain, bool& bOutDirty)
{
	FRHICommandList* CmdList = new FRHICommandList;

	FGraphEventRef AnyThreadCompletionEvent = TGraphTask<FRenderBasePassDynamicDataThreadTask>::CreateTask(nullptr, ENamedThreads::RenderThread)
		.ConstructAndDispatchWhenReady(this, CmdList, &View, &bOutDirty);

	FGraphEventArray Prereqs;
	Prereqs.Add(AnyThreadCompletionEvent);
	if (SubmitChain.GetReference())
	{
		Prereqs.Add(SubmitChain);
	}

	return TGraphTask<FSubmitCommandlistThreadTask>::CreateTask(&Prereqs, ENamedThreads::RenderThread).ConstructAndDispatchWhenReady(CmdList);
}

static void SetupBasePassView(FRHICommandList& RHICmdList, const FViewInfo& View, bool bShaderComplexity)
{
	if (bShaderComplexity)
	{
		// Additive blending when shader complexity viewmode is enabled.
		RHICmdList.SetBlendState(TStaticBlendState<CW_RGBA,BO_Add,BF_One,BF_One,BO_Add,BF_Zero,BF_One>::GetRHI());
		// Disable depth writes as we have a full depth prepass.
		RHICmdList.SetDepthStencilState(TStaticDepthStencilState<false,CF_GreaterEqual>::GetRHI());
	}
	else
	{
		// Opaque blending for all G buffer targets, depth tests and writes.
		RHICmdList.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA, CW_RGBA, CW_RGBA, CW_RGBA>::GetRHI());
		// Note, this is a reversed Z depth surface, using CF_GreaterEqual.
		RHICmdList.SetDepthStencilState(TStaticDepthStencilState<true,CF_GreaterEqual>::GetRHI());
	}
	RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);
}

class FSubmitCommandlistSetupBasePassView
{
	const FViewInfo& View;
	bool bShaderComplexity;
public:

	FSubmitCommandlistSetupBasePassView(const FViewInfo* InView, bool bInShaderComplexity)
		: View(*InView)
		, bShaderComplexity(bInShaderComplexity)
	{
	}

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FSubmitCommandlistSetupBasePassView, STATGROUP_TaskGraphTasks);
	}

	ENamedThreads::Type GetDesiredThread()
	{
		return ENamedThreads::RenderThread_Local;
	}

	static ESubsequentsMode::Type GetSubsequentsMode() { return ESubsequentsMode::TrackSubsequents; }

	void DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
	{
		FRHICommandList RHICmdList;
		SetupBasePassView(RHICmdList, View, bShaderComplexity);
	}
};

FGraphEventRef FDeferredShadingSceneRenderer::RenderBasePassViewParallel(FRHICommandListImmediate& RHICmdList, FViewInfo& View, int32 Width, FGraphEventRef SubmitChain, bool& OutDirty)
{
	{
		//optimization: this is probably only needed on the first view
		FGraphEventArray Prereqs;
		if (SubmitChain.GetReference())
		{
			Prereqs.Add(SubmitChain);
		}
		SubmitChain = TGraphTask<FSubmitCommandlistSetupBasePassView>::CreateTask(&Prereqs, ENamedThreads::RenderThread).ConstructAndDispatchWhenReady(&View, !!ViewFamily.EngineShowFlags.ShaderComplexity);

	}			

	SubmitChain = RenderBasePassStaticDataParallel(View, Width, SubmitChain, OutDirty);
	SubmitChain = RenderBasePassDynamicDataParallel(View, SubmitChain, OutDirty);

	return SubmitChain;
}

bool FDeferredShadingSceneRenderer::RenderBasePassView(FRHICommandListImmediate& RHICmdList, FViewInfo& View)
{
	bool bDirty = false; 

	SetupBasePassView(RHICmdList, View, ViewFamily.EngineShowFlags.ShaderComplexity);
	bDirty |= RenderBasePassStaticData(RHICmdList, View);
	RenderBasePassDynamicData(RHICmdList, View, bDirty);

	return bDirty;
}

/** Render the TexturePool texture */
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
void FDeferredShadingSceneRenderer::RenderVisualizeTexturePool(FRHICommandListImmediate& RHICmdList)
{
	TRefCountPtr<IPooledRenderTarget> VisualizeTexturePool;

	/** Resolution for the texture pool visualizer texture. */
	enum
	{
		TexturePoolVisualizerSizeX = 280,
		TexturePoolVisualizerSizeY = 140,
	};

	FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(TexturePoolVisualizerSizeX, TexturePoolVisualizerSizeY), PF_B8G8R8A8, TexCreate_None, TexCreate_None, false));
	GRenderTargetPool.FindFreeElement(Desc, VisualizeTexturePool, TEXT("VisualizeTexturePool"));
	
	uint32 Pitch;
	FColor* TextureData = (FColor*)RHICmdList.LockTexture2D((FTexture2DRHIRef&)VisualizeTexturePool->GetRenderTargetItem().ShaderResourceTexture, 0, RLM_WriteOnly, Pitch, false);
	if(TextureData)
	{
		// clear with grey to get reliable background color
		FMemory::Memset(TextureData, 0x88, TexturePoolVisualizerSizeX * TexturePoolVisualizerSizeY * 4);
		RHICmdList.GetTextureMemoryVisualizeData(TextureData, TexturePoolVisualizerSizeX, TexturePoolVisualizerSizeY, Pitch, 4096);
	}

	RHICmdList.UnlockTexture2D((FTexture2DRHIRef&)VisualizeTexturePool->GetRenderTargetItem().ShaderResourceTexture, 0, false);

	FIntPoint RTExtent = GSceneRenderTargets.GetBufferSizeXY();

	FVector2D Tex00 = FVector2D(0, 0);
	FVector2D Tex11 = FVector2D(1, 1);

//todo	VisualizeTexture(*VisualizeTexturePool, ViewFamily.RenderTarget, FIntRect(0, 0, RTExtent.X, RTExtent.Y), RTExtent, 1.0f, 0.0f, 0.0f, Tex00, Tex11, 1.0f, false);
}
#endif


/** 
* Finishes the view family rendering.
*/
void FDeferredShadingSceneRenderer::RenderFinish(FRHICommandListImmediate& RHICmdList)
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	{
		if(CVarVisualizeTexturePool.GetValueOnRenderThread())
		{
			RenderVisualizeTexturePool(RHICmdList);
		}
	}

#endif //!(UE_BUILD_SHIPPING || UE_BUILD_TEST)

	FSceneRenderer::RenderFinish(RHICmdList);

	//grab the new transform out of the proxies for next frame
	if(ViewFamily.EngineShowFlags.MotionBlur || ViewFamily.EngineShowFlags.TemporalAA)
	{
		Scene->MotionBlurInfoData.UpdateMotionBlurCache(Scene);
	}

	// Some RT should be released as early as possible to allow sharing of that memory for other purposes.
	// SceneColor is be released in tone mapping, if not we want to get access to the HDR scene color after this pass so we keep it.
	// This becomes even more important with some limited VRam (XBoxOne).
	GSceneRenderTargets.SetLightAttenuation(0);
}

/** 
* Renders the view family. 
*/
void FDeferredShadingSceneRenderer::Render(FRHICommandListImmediate& RHICmdList)
{
	if(!ViewFamily.EngineShowFlags.Rendering)
	{
		return;
	}

	SCOPED_DRAW_EVENT(Scene,DEC_SCENE_ITEMS);

	// Initialize global system textures (pass-through if already initialized).
	GSystemTextures.InitializeTextures(RHICmdList, FeatureLevel);

	// Allocate the maximum scene render target space for the current view family.
	GSceneRenderTargets.Allocate(ViewFamily);

	// Find the visible primitives.
	InitViews(RHICmdList);

	const bool bIsWireframe = ViewFamily.EngineShowFlags.Wireframe;

	static const auto ClearMethodCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.ClearSceneMethod"));
	bool bRequiresRHIClear = true;
	bool bRequiresFarZQuadClear = false;

	if (ClearMethodCVar)
	{
		switch (ClearMethodCVar->GetValueOnRenderThread())
		{
		case 0: // No clear
			{
				bRequiresRHIClear = false;
				bRequiresFarZQuadClear = false;
				break;
			}
		
		case 1: // RHICmdList.Clear
			{
				bRequiresRHIClear = true;
				bRequiresFarZQuadClear = false;
				break;
			}

		case 2: // Clear using far-z quad
			{
				bRequiresFarZQuadClear = true;
				bRequiresRHIClear = false;
				break;
			}
		}
	}

	// Always perform a full buffer clear for wireframe, shader complexity view mode, and stationary light overlap viewmode.
	if (bIsWireframe || ViewFamily.EngineShowFlags.ShaderComplexity || ViewFamily.EngineShowFlags.StationaryLightOverlap)
	{
		bRequiresRHIClear = true;
	}

	// force using occ queries for wireframe if rendering is parented or frozen in the first view
	check(Views.Num());
	#if (UE_BUILD_SHIPPING || UE_BUILD_TEST)
		const bool bIsViewFrozen = false;
		const bool bHasViewParent = false;
	#else
		const bool bIsViewFrozen = Views[0].State && ((FSceneViewState*)Views[0].State)->bIsFrozen;
		const bool bHasViewParent = Views[0].State && ((FSceneViewState*)Views[0].State)->HasViewParent();
	#endif

	
	const bool bIsOcclusionTesting = DoOcclusionQueries() && (!bIsWireframe || bIsViewFrozen || bHasViewParent);

	// Dynamic vertex and index buffers need to be committed before rendering.
	FGlobalDynamicVertexBuffer::Get().Commit();
	FGlobalDynamicIndexBuffer::Get().Commit();

	// Notify the FX system that the scene is about to be rendered.
	if (Scene->FXSystem)
	{
		Scene->FXSystem->PreRender(RHICmdList);
	}

	GRenderTargetPool.AddPhaseEvent(TEXT("EarlyZPass"));

	// Draw the scene pre-pass / early z pass, populating the scene depth buffer and HiZ
	RenderPrePass(RHICmdList);
	
	GSceneRenderTargets.AllocGBufferTargets();
	
	// Clear scene color buffer if necessary.
	if ( bRequiresRHIClear )
	{
		ClearView(RHICmdList);

		// Only clear once.
		bRequiresRHIClear = false;
	}

	// Clear LPVs for all views
	if ( IsFeatureLevelSupported(GRHIShaderPlatform, ERHIFeatureLevel::SM5) )
	{
		ClearLPVs(RHICmdList);
	}

	// only temporarily available after early z pass and until base pass
	check(!GSceneRenderTargets.DBufferA);
	check(!GSceneRenderTargets.DBufferB);
	check(!GSceneRenderTargets.DBufferC);

	if(IsDBufferEnabled())
	{
		GSceneRenderTargets.ResolveSceneDepthTexture(RHICmdList);
		GSceneRenderTargets.ResolveSceneDepthToAuxiliaryTexture(RHICmdList);

		// e.g. ambient cubemaps, ambient occlusion, deferred decals
		for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
		{	
			SCOPED_CONDITIONAL_DRAW_EVENTF(EventView,Views.Num() > 1, DEC_SCENE_ITEMS, TEXT("View%d"), ViewIndex);
			GCompositionLighting.ProcessBeforeBasePass(RHICmdList, Views[ViewIndex]);
		}
	}

	if(bIsWireframe && FDeferredShadingSceneRenderer::ShouldCompositeEditorPrimitives(Views[0]))
	{
		// In Editor we want wire frame view modes to be MSAA for better quality. Resolve will be done with EditorPrimitives
		SetRenderTarget(RHICmdList, GSceneRenderTargets.GetEditorPrimitivesColor(), GSceneRenderTargets.GetEditorPrimitivesDepth());
		RHICmdList.Clear(true, FLinearColor(0, 0, 0, 0), true, 0.0f, false, 0, FIntRect());
	}
	else
	{
	// Begin rendering to scene color
		GSceneRenderTargets.BeginRenderingSceneColor(RHICmdList, true);
	}

	GRenderTargetPool.AddPhaseEvent(TEXT("BasePass"));

	RenderBasePass(RHICmdList);

	if(ViewFamily.EngineShowFlags.VisualizeLightCulling)
	{
		// clear out emissive and baked lighting (not too efficient but simple and only needed for this debug view)
		GSceneRenderTargets.BeginRenderingSceneColor(RHICmdList, false);
		RHICmdList.Clear(true, FLinearColor(0, 0, 0, 0), false, 0, false, 0, FIntRect());
	}

	GSceneRenderTargets.DBufferA.SafeRelease();
	GSceneRenderTargets.DBufferB.SafeRelease();
	GSceneRenderTargets.DBufferC.SafeRelease();

	// only temporarily available after early z pass and until base pass
	check(!GSceneRenderTargets.DBufferA);
	check(!GSceneRenderTargets.DBufferB);
	check(!GSceneRenderTargets.DBufferC);

	if (bRequiresFarZQuadClear)
	{
		// Clears view by drawing quad at maximum Z
		// TODO: if all the platforms have fast color clears, we can replace this with an RHICmdList.Clear.
		ClearGBufferAtMaxZ(RHICmdList);

		bRequiresFarZQuadClear = false;
	}
	
	GSceneRenderTargets.ResolveSceneColor(RHICmdList, FResolveRect(0, 0, ViewFamily.FamilySizeX, ViewFamily.FamilySizeY));
	GSceneRenderTargets.ResolveSceneDepthTexture(RHICmdList);
	GSceneRenderTargets.ResolveSceneDepthToAuxiliaryTexture(RHICmdList);
	
	RenderCustomDepthPass(RHICmdList);

	// Notify the FX system that opaque primitives have been rendered and we now have a valid depth buffer.
	if (Scene->FXSystem && Views.IsValidIndex(0))
	{
		Scene->FXSystem->PostRenderOpaque(
			RHICmdList,
			Views.GetTypedData(),
			GSceneRenderTargets.GetSceneDepthTexture(),
			GSceneRenderTargets.GetGBufferATexture()
			);
	}

	// Update the quarter-sized depth buffer with the current contents of the scene depth texture.
	// This needs to happen before occlusion tests, which makes use of the small depth buffer.
	UpdateDownsampledDepthSurface(RHICmdList);

	// Issue occlusion queries
	// This is done after the downsampled depth buffer is created so that it can be used for issuing queries
	if ( bIsOcclusionTesting )
	{
		BeginOcclusionTests(RHICmdList);
	}
	
	// Render lighting.
	if (ViewFamily.EngineShowFlags.Lighting
		&& FeatureLevel >= ERHIFeatureLevel::SM4
		&& ViewFamily.EngineShowFlags.DeferredLighting
		)
	{
		GRenderTargetPool.AddPhaseEvent(TEXT("Lighting"));

		// Pre-lighting composition lighting stage
		// e.g. deferred decals, blurred GBuffer
		for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
		{	
			SCOPED_CONDITIONAL_DRAW_EVENTF(EventView,Views.Num() > 1, DEC_SCENE_ITEMS, TEXT("View%d"), ViewIndex);
			GCompositionLighting.ProcessAfterBasePass(RHICmdList, Views[ViewIndex]);
		}
		
		RenderDynamicSkyLighting(RHICmdList);

		// Clear the translucent lighting volumes before we accumulate
		ClearTranslucentVolumeLighting(RHICmdList);

		RenderLights(RHICmdList);

		GRenderTargetPool.AddPhaseEvent(TEXT("AfterRenderLights"));

		InjectAmbientCubemapTranslucentVolumeLighting(RHICmdList);

		CompositeIndirectTranslucentVolumeLighting(RHICmdList);

		// Filter the translucency lighting volume now that it is complete
		FilterTranslucentVolumeLighting(RHICmdList);

		// Clear LPVs for all views
		if ( IsFeatureLevelSupported(GRHIShaderPlatform, ERHIFeatureLevel::SM5) )
		{
			PropagateLPVs(RHICmdList);
		}

		// Render reflections that only operate on opaque pixels
		RenderDeferredReflections(RHICmdList);

		// Post-lighting composition lighting stage
		// e.g. ambient cubemaps, ambient occlusion, LPV indirect
		for(int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{	
			SCOPED_CONDITIONAL_DRAW_EVENTF(EventView,Views.Num() > 1, DEC_SCENE_ITEMS, TEXT("View%d"), ViewIndex);
			GCompositionLighting.ProcessLighting(RHICmdList, Views[ViewIndex]);
		}
	}

	if( ViewFamily.EngineShowFlags.StationaryLightOverlap &&
		FeatureLevel >= ERHIFeatureLevel::SM4)
	{
		RenderStationaryLightOverlap(RHICmdList);
	}

	FLightShaftsOutput LightShaftOutput;

	// Draw Lightshafts
	if (ViewFamily.EngineShowFlags.LightShafts)
	{
		LightShaftOutput = RenderLightShaftOcclusion(RHICmdList);
	}

	// Draw atmosphere
	if(ShouldRenderAtmosphere(ViewFamily))
	{
		if (Scene->AtmosphericFog)
		{
			// Update RenderFlag based on LightShaftTexture is valid or not
			if (LightShaftOutput.bRendered)
			{
				Scene->AtmosphericFog->RenderFlag &= EAtmosphereRenderFlag::E_LightShaftMask;
			}
			else
			{
				Scene->AtmosphericFog->RenderFlag |= EAtmosphereRenderFlag::E_DisableLightShaft;
			}
#if WITH_EDITOR
			if (Scene->bIsEditorScene)
			{
				// Precompute Atmospheric Textures
				Scene->AtmosphericFog->PrecomputeTextures(RHICmdList, Views.GetTypedData(), &ViewFamily);
			}
#endif
			RenderAtmosphere(RHICmdList, LightShaftOutput);
		}
	}

	GRenderTargetPool.AddPhaseEvent(TEXT("Fog"));

	// Draw fog.
	if(ShouldRenderFog(ViewFamily))
	{
		RenderFog(RHICmdList, LightShaftOutput);
	}

	// No longer needed, release
	LightShaftOutput.LightShaftOcclusion = NULL;

	GRenderTargetPool.AddPhaseEvent(TEXT("Translucency"));

	// Draw translucency.
	if(ViewFamily.EngineShowFlags.Translucency)
	{
		SCOPE_CYCLE_COUNTER(STAT_TranslucencyDrawTime);

		RenderTranslucency(RHICmdList);

		if(ViewFamily.EngineShowFlags.Refraction)
		{
			// To apply refraction effect by distorting the scene color.
			// After non separate translucency as that is considered at scene depth anyway
			// It allows skybox translucency (set to non separate translucency) to be refracted.
			RenderDistortion(RHICmdList);
		}
	}


	if( Views.Num() > 0 )
	{
		GetRendererModule().RenderPostOpaqueExtensions(Views[0]);
	}
	if (ViewFamily.EngineShowFlags.LightShafts)
	{
		RenderLightShaftBloom(RHICmdList);
	}

	if (ViewFamily.EngineShowFlags.VisualizeDistanceFieldAO)
	{
		FSceneRenderTargetItem DummyOutput(NULL, NULL, NULL);
		RenderDistanceFieldAOSurfaceCache(RHICmdList, DummyOutput, true);
	}

	// Resolve the scene color for post processing.
	GSceneRenderTargets.ResolveSceneColor(RHICmdList, FResolveRect(0, 0, ViewFamily.FamilySizeX, ViewFamily.FamilySizeY));

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if(CVarTestUIBlur.GetValueOnRenderThread() > 0)
	{
		Views[0].UIBlurOverrideRectangles.Add(FIntRect(20, 20, 400, 400));
	}
#endif

	// Finish rendering for each view.
	if(ViewFamily.bResolveScene)
	{
		SCOPED_DRAW_EVENT(FinishRendering, DEC_SCENE_ITEMS);
		SCOPE_CYCLE_COUNTER(STAT_FinishRenderViewTargetTime);
		for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
		{	
			SCOPED_CONDITIONAL_DRAW_EVENTF(EventView, Views.Num() > 1, DEC_SCENE_ITEMS, TEXT("View%d"), ViewIndex);
			FinishRenderViewTarget(RHICmdList, &Views[ViewIndex], ViewIndex == (Views.Num() - 1));
		}
	}
	else
	{
		// Release the original reference on the scene render targets
		GSceneRenderTargets.AdjustGBufferRefCount(-1);
	}

	RenderFinish(RHICmdList);
}

bool FDeferredShadingSceneRenderer::RenderPrePassViewDynamic(FRHICommandList& RHICmdList, const FViewInfo& View)
{
	// Draw the dynamic occluder primitives using a depth drawing policy.
	TDynamicPrimitiveDrawer<FDepthDrawingPolicyFactory> Drawer(RHICmdList, &View, FDepthDrawingPolicyFactory::ContextType(EarlyZPassMode), true);
	{
		SCOPED_DRAW_EVENT(Dynamic, DEC_SCENE_ITEMS);
		for(int32 PrimitiveIndex = 0;PrimitiveIndex < View.VisibleDynamicPrimitives.Num();PrimitiveIndex++)
		{
			const FPrimitiveSceneInfo* PrimitiveSceneInfo = View.VisibleDynamicPrimitives[PrimitiveIndex];
			int32 PrimitiveId = PrimitiveSceneInfo->GetIndex();
			const FPrimitiveViewRelevance& PrimitiveViewRelevance = View.PrimitiveViewRelevanceMap[PrimitiveId];

			bool bShouldUseAsOccluder = true;

			if(EarlyZPassMode != DDM_AllOccluders)
			{
				extern float GMinScreenRadiusForDepthPrepass;
				const float LODFactorDistanceSquared = (PrimitiveSceneInfo->Proxy->GetBounds().Origin - View.ViewMatrices.ViewOrigin).SizeSquared() * FMath::Square(View.LODDistanceFactor);
	
				// Only render primitives marked as occluders
				bShouldUseAsOccluder = PrimitiveSceneInfo->Proxy->ShouldUseAsOccluder()
					// Only render static objects unless movable are requested
					&& (!PrimitiveSceneInfo->Proxy->IsMovable() || GEarlyZPassMovable)
					&& (FMath::Square(PrimitiveSceneInfo->Proxy->GetBounds().SphereRadius) > GMinScreenRadiusForDepthPrepass * GMinScreenRadiusForDepthPrepass * LODFactorDistanceSquared);
			}

			// Only render opaque primitives marked as occluders
			if (bShouldUseAsOccluder && PrimitiveViewRelevance.bOpaqueRelevance && PrimitiveViewRelevance.bRenderInMainPass)
		{
				FScopeCycleCounter Context(PrimitiveSceneInfo->Proxy->GetStatId());
				Drawer.SetPrimitive(PrimitiveSceneInfo->Proxy);
				PrimitiveSceneInfo->Proxy->DrawDynamicElements(
					&Drawer,
					&View
					);
			}
		}
	}
	return Drawer.IsDirty();
}

static void SetupPrePassView(FRHICommandList& RHICmdList, const FViewInfo& View)
{
			// Disable color writes, enable depth tests and writes.
			RHICmdList.SetBlendState(TStaticBlendState<CW_NONE>::GetRHI());
			// Note, this is a reversed Z depth surface, using CF_GreaterEqual.
			RHICmdList.SetDepthStencilState(TStaticDepthStencilState<true,CF_GreaterEqual>::GetRHI());
			RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);
}

bool FDeferredShadingSceneRenderer::RenderPrePassView(FRHICommandList& RHICmdList, const FViewInfo& View)
{
	bool bDirty = false;

	SetupPrePassView(RHICmdList, View);

			// Draw the static occluder primitives using a depth drawing policy.
			{
				// Draw opaque occluders which support a separate position-only
				// vertex buffer to minimize vertex fetch bandwidth, which is
				// often the bottleneck during the depth only pass.
				SCOPED_DRAW_EVENT(PosOnlyOpaque, DEC_SCENE_ITEMS);
				bDirty |= Scene->PositionOnlyDepthDrawList.DrawVisible(RHICmdList, View,View.StaticMeshOccluderMap,View.StaticMeshBatchVisibility);
			}
			{
				// Draw opaque occluders, using double speed z where supported.
				SCOPED_DRAW_EVENT(Opaque, DEC_SCENE_ITEMS);
				bDirty |= Scene->DepthDrawList.DrawVisible(RHICmdList, View,View.StaticMeshOccluderMap,View.StaticMeshBatchVisibility);
			}

			if(EarlyZPassMode >= DDM_AllOccluders)
			{
				// Draw opaque occluders with masked materials
				SCOPED_DRAW_EVENT(Opaque, DEC_SCENE_ITEMS);
				bDirty |= Scene->MaskedDepthDrawList.DrawVisible(RHICmdList, View,View.StaticMeshOccluderMap,View.StaticMeshBatchVisibility);
			}

	bDirty |= RenderPrePassViewDynamic(RHICmdList, View);
	return bDirty;
}

class FSubmitCommandlistSetupPrepassView
			{
	const FViewInfo& View;
public:

	FSubmitCommandlistSetupPrepassView(const FViewInfo* InView)
		: View(*InView)
				{
	}

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FSubmitCommandlistSetupPrepassView, STATGROUP_TaskGraphTasks);
	}
					
	ENamedThreads::Type GetDesiredThread()
					{
		return ENamedThreads::RenderThread_Local;
	}

	static ESubsequentsMode::Type GetSubsequentsMode() { return ESubsequentsMode::TrackSubsequents; }

	void DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
	{
		FRHICommandList RHICmdList;
		SetupPrePassView(RHICmdList, View);
					}
};

class FRenderPrepassDynamicDataThreadTask
{
	FDeferredShadingSceneRenderer& ThisRenderer;
	FRHICommandList& RHICmdList;
	const FViewInfo& View;
	bool& OutDirty;

public:

	FRenderPrepassDynamicDataThreadTask(
		FDeferredShadingSceneRenderer* InThisRenderer,
		FRHICommandList* InRHICmdList,
		const FViewInfo* InView,
		bool* InOutDirty
		)
		: ThisRenderer(*InThisRenderer)
		, RHICmdList(*InRHICmdList)
		, View(*InView)
		, OutDirty(*InOutDirty)
					{
					}

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FRenderPrepassDynamicDataThreadTask, STATGROUP_TaskGraphTasks);
				}

	ENamedThreads::Type GetDesiredThread()
	{
		return ENamedThreads::AnyThread;
	}

	static ESubsequentsMode::Type GetSubsequentsMode() { return ESubsequentsMode::TrackSubsequents; }

	void DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
	{
		if (ThisRenderer.RenderPrePassViewDynamic(RHICmdList, View))
		{
			OutDirty = true;
		}
	}
};


FGraphEventRef FDeferredShadingSceneRenderer::RenderPrePassViewParallel(FRHICommandList& RHICmdList, const FViewInfo& View, int32 Width, FGraphEventRef SubmitChain, bool& OutDirty)
{
	{
		//optimization: this is probably only needed on the first view
		FGraphEventArray Prereqs;
		if (SubmitChain.GetReference())
		{
			Prereqs.Add(SubmitChain);
		}
		SubmitChain = TGraphTask<FSubmitCommandlistSetupPrepassView>::CreateTask(&Prereqs, ENamedThreads::RenderThread).ConstructAndDispatchWhenReady(&View);

	}

	// Draw the static occluder primitives using a depth drawing policy.
	{
		// Draw opaque occluders which support a separate position-only
		// vertex buffer to minimize vertex fetch bandwidth, which is
		// often the bottleneck during the depth only pass.
		//SCOPED_DRAW_EVENT(PosOnlyOpaque, DEC_SCENE_ITEMS);
		SubmitChain = Scene->PositionOnlyDepthDrawList.DrawVisibleParallel(View, View.StaticMeshOccluderMap, View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);
	}
	{
		// Draw opaque occluders, using double speed z where supported.
		//SCOPED_DRAW_EVENT(Opaque, DEC_SCENE_ITEMS);
		SubmitChain = Scene->DepthDrawList.DrawVisibleParallel(View,View.StaticMeshOccluderMap,View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);
	}

	if(EarlyZPassMode >= DDM_AllOccluders)
	{
		// Draw opaque occluders with masked materials
		//SCOPED_DRAW_EVENT(Opaque, DEC_SCENE_ITEMS);
		SubmitChain = Scene->MaskedDepthDrawList.DrawVisibleParallel(View,View.StaticMeshOccluderMap,View.StaticMeshBatchVisibility, Width, SubmitChain, OutDirty);
	}

	{
		FRHICommandList* CmdList = new FRHICommandList;

		FGraphEventRef AnyThreadCompletionEvent = TGraphTask<FRenderPrepassDynamicDataThreadTask>::CreateTask(nullptr, ENamedThreads::RenderThread)
			.ConstructAndDispatchWhenReady(this, CmdList, &View, &OutDirty);

		FGraphEventArray Prereqs;
		Prereqs.Add(AnyThreadCompletionEvent);
		if (SubmitChain.GetReference())
		{
			Prereqs.Add(SubmitChain);
		}

		SubmitChain = TGraphTask<FSubmitCommandlistThreadTask>::CreateTask(&Prereqs, ENamedThreads::RenderThread).ConstructAndDispatchWhenReady(CmdList);
	}

	return SubmitChain;
}


/** Renders the scene's prepass and occlusion queries */
bool FDeferredShadingSceneRenderer::RenderPrePass(FRHICommandListImmediate& RHICmdList)
{
	SCOPED_DRAW_EVENT(PrePass, DEC_SCENE_ITEMS);
	SCOPE_CYCLE_COUNTER(STAT_DepthDrawTime);

	bool bDirty = false;

	GSceneRenderTargets.BeginRenderingPrePass(RHICmdList);

	
	// Clear the depth buffer.
	// Note, this is a reversed Z depth surface, so 0.0f is the far plane.
	RHICmdList.Clear(false,FLinearColor::Black,true,0.0f,true,0, FIntRect());

	// Draw a depth pass to avoid overdraw in the other passes.
	if(EarlyZPassMode != DDM_None)
	{
		if (!GRHICommandList.Bypass())
		{
			int32 Width = CVarRHICmdWidth.GetValueOnRenderThread(); // we use a few more than needed to cover non-equal jobs

			FGraphEventRef SubmitChain;
			for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
			{
				SCOPED_CONDITIONAL_DRAW_EVENTF(EventView, Views.Num() > 1, DEC_SCENE_ITEMS, TEXT("View%d"), ViewIndex);
				const FViewInfo& View = Views[ViewIndex];
				SubmitChain = RenderPrePassViewParallel(RHICmdList, View, Width, SubmitChain, bDirty);
			}
			
			if (SubmitChain.GetReference())
			{
				FTaskGraphInterface::Get().WaitUntilTaskCompletes(SubmitChain, ENamedThreads::RenderThread_Local);
			}
		}
		else
		{
			for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
			{
				SCOPED_CONDITIONAL_DRAW_EVENTF(EventView, Views.Num() > 1, DEC_SCENE_ITEMS, TEXT("View%d"), ViewIndex);
				const FViewInfo& View = Views[ViewIndex];
				bDirty |= RenderPrePassView(RHICmdList, View);
			}
		}
	}

	GSceneRenderTargets.FinishRenderingPrePass(RHICmdList);

	return bDirty;
}

/**
 * Renders the scene's base pass 
 * @return true if anything was rendered
 */
bool FDeferredShadingSceneRenderer::RenderBasePass(FRHICommandListImmediate& RHICmdList)
{
	bool bDirty = false;

	if(ViewFamily.EngineShowFlags.LightMapDensity && AllowDebugViewmodes())
	{
		// Override the base pass with the lightmap density pass if the viewmode is enabled.
		bDirty = RenderLightMapDensities(RHICmdList);
	}
	else
	{
		SCOPED_DRAW_EVENT(BasePass, DEC_SCENE_ITEMS);
		SCOPE_CYCLE_COUNTER(STAT_BasePassDrawTime);

		if (!GRHICommandList.Bypass())
		{
			int32 Width = CVarRHICmdWidth.GetValueOnRenderThread(); // we use a few more than needed to cover non-equal jobs
			FGraphEventRef SubmitChain;

		for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
		{
			SCOPED_CONDITIONAL_DRAW_EVENTF(EventView, Views.Num() > 1, DEC_SCENE_ITEMS, TEXT("View%d"), ViewIndex);
			FViewInfo& View = Views[ViewIndex];
				SubmitChain = RenderBasePassViewParallel(RHICmdList, View, Width, SubmitChain, bDirty);
			}

			if (SubmitChain.GetReference())
			{
				FTaskGraphInterface::Get().WaitUntilTaskCompletes(SubmitChain, ENamedThreads::RenderThread_Local);
			}
			}
			else
			{
			for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
			{
				SCOPED_CONDITIONAL_DRAW_EVENTF(EventView, Views.Num() > 1, DEC_SCENE_ITEMS, TEXT("View%d"), ViewIndex);
				FViewInfo& View = Views[ViewIndex];

				bDirty |= RenderBasePassView(RHICmdList, View);
		}
	}

	}

	return bDirty;
}

void FDeferredShadingSceneRenderer::ClearLPVs(FRHICommandListImmediate& RHICmdList)
{
	// clear light propagation volumes

	for(int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		FViewInfo& View = Views[ViewIndex];

		FSceneViewState* ViewState = (FSceneViewState*)Views[ViewIndex].State;
		if(ViewState)
		{
			FLightPropagationVolume* LightPropagationVolume = ViewState->GetLightPropagationVolume();

			if(LightPropagationVolume)
			{
				SCOPED_DRAW_EVENT(ClearLPVs, DEC_SCENE_ITEMS);
				SCOPE_CYCLE_COUNTER(STAT_UpdateLPVs);
				LightPropagationVolume->InitSettings(RHICmdList, Views[ViewIndex]);
				LightPropagationVolume->Clear(RHICmdList);
			}
		}
	}
}

void FDeferredShadingSceneRenderer::PropagateLPVs(FRHICommandListImmediate& RHICmdList)
{
	for(int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		FViewInfo& View = Views[ViewIndex];

		FSceneViewState* ViewState = (FSceneViewState*)Views[ViewIndex].State;
		if(ViewState)
		{
			FLightPropagationVolume* LightPropagationVolume = ViewState->GetLightPropagationVolume();

			if(LightPropagationVolume)
			{
				SCOPED_DRAW_EVENT(UpdateLPVs, DEC_SCENE_ITEMS);
				SCOPE_CYCLE_COUNTER(STAT_UpdateLPVs);
				
				LightPropagationVolume->Propagate(RHICmdList);
			}
		}
	}
}

/** A simple pixel shader used on PC to read scene depth from scene color alpha and write it to a downsized depth buffer. */
class FDownsampleSceneDepthPS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FDownsampleSceneDepthPS,Global);
public:

	static bool ShouldCache(EShaderPlatform Platform)
	{ 
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM3);
	}

	FDownsampleSceneDepthPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer):
		FGlobalShader(Initializer)
	{
		SceneTextureParameters.Bind(Initializer.ParameterMap);
		ProjectionScaleBias.Bind(Initializer.ParameterMap,TEXT("ProjectionScaleBias"));
		SourceTexelOffsets01.Bind(Initializer.ParameterMap,TEXT("SourceTexelOffsets01"));
		SourceTexelOffsets23.Bind(Initializer.ParameterMap,TEXT("SourceTexelOffsets23"));
	}
	FDownsampleSceneDepthPS() {}

	void SetParameters(FRHICommandList& RHICmdList, const FSceneView& View)
	{
		FGlobalShader::SetParameters(RHICmdList, GetPixelShader(), View);

		// Used to remap view space Z (which is stored in scene color alpha) into post projection z and w so we can write z/w into the downsized depth buffer
		const FVector2D ProjectionScaleBiasValue(View.ViewMatrices.ProjMatrix.M[2][2], View.ViewMatrices.ProjMatrix.M[3][2]);
		SetShaderValue(RHICmdList, GetPixelShader(), ProjectionScaleBias, ProjectionScaleBiasValue);

		FIntPoint BufferSize = GSceneRenderTargets.GetBufferSizeXY();

		const uint32 DownsampledBufferSizeX = BufferSize.X / GSceneRenderTargets.GetSmallColorDepthDownsampleFactor();
		const uint32 DownsampledBufferSizeY = BufferSize.Y / GSceneRenderTargets.GetSmallColorDepthDownsampleFactor();

		// Offsets of the four full resolution pixels corresponding with a low resolution pixel
		const FVector4 Offsets01(0.0f, 0.0f, 1.0f / DownsampledBufferSizeX, 0.0f);
		SetShaderValue(RHICmdList, GetPixelShader(), SourceTexelOffsets01, Offsets01);
		const FVector4 Offsets23(0.0f, 1.0f / DownsampledBufferSizeY, 1.0f / DownsampledBufferSizeX, 1.0f / DownsampledBufferSizeY);
		SetShaderValue(RHICmdList, GetPixelShader(), SourceTexelOffsets23, Offsets23);
		SceneTextureParameters.Set(RHICmdList, GetPixelShader(), View);
	}

	virtual bool Serialize(FArchive& Ar)
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << ProjectionScaleBias;
		Ar << SourceTexelOffsets01;
		Ar << SourceTexelOffsets23;
		Ar << SceneTextureParameters;
		return bShaderHasOutdatedParameters;
	}

	FShaderParameter ProjectionScaleBias;
	FShaderParameter SourceTexelOffsets01;
	FShaderParameter SourceTexelOffsets23;
	FSceneTextureShaderParameters SceneTextureParameters;
};

IMPLEMENT_SHADER_TYPE(,FDownsampleSceneDepthPS,TEXT("DownsampleDepthPixelShader"),TEXT("Main"),SF_Pixel);

FGlobalBoundShaderState DownsampleDepthBoundShaderState;

/** Updates the downsized depth buffer with the current full resolution depth buffer. */
void FDeferredShadingSceneRenderer::UpdateDownsampledDepthSurface(FRHICommandList& RHICmdList)
{
	if (GSceneRenderTargets.UseDownsizedOcclusionQueries() && IsFeatureLevelSupported(GRHIShaderPlatform, ERHIFeatureLevel::SM3))
	{
		SetRenderTarget(RHICmdList, NULL, GSceneRenderTargets.GetSmallDepthSurface());

		SCOPED_DRAW_EVENT(DownsampleDepth, DEC_SCENE_ITEMS);

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			const FViewInfo& View = Views[ViewIndex];
			// Set shaders and texture
			TShaderMapRef<FScreenVS> ScreenVertexShader(GetGlobalShaderMap());
			TShaderMapRef<FDownsampleSceneDepthPS> PixelShader(GetGlobalShaderMap());

			extern TGlobalResource<FFilterVertexDeclaration> GFilterVertexDeclaration;

			SetGlobalBoundShaderState(RHICmdList, DownsampleDepthBoundShaderState, GFilterVertexDeclaration.VertexDeclarationRHI, *ScreenVertexShader, *PixelShader);

			RHICmdList.SetBlendState(TStaticBlendState<CW_NONE>::GetRHI());
			RHICmdList.SetRasterizerState(TStaticRasterizerState<FM_Solid, CM_None>::GetRHI());
			RHICmdList.SetDepthStencilState(TStaticDepthStencilState<true, CF_Always>::GetRHI());

			PixelShader->SetParameters(RHICmdList, View);

			const uint32 DownsampledX = FMath::TruncToInt(View.ViewRect.Min.X / GSceneRenderTargets.GetSmallColorDepthDownsampleFactor());
			const uint32 DownsampledY = FMath::TruncToInt(View.ViewRect.Min.Y / GSceneRenderTargets.GetSmallColorDepthDownsampleFactor());
			const uint32 DownsampledSizeX = FMath::TruncToInt(View.ViewRect.Width() / GSceneRenderTargets.GetSmallColorDepthDownsampleFactor());
			const uint32 DownsampledSizeY = FMath::TruncToInt(View.ViewRect.Height() / GSceneRenderTargets.GetSmallColorDepthDownsampleFactor());

			RHICmdList.SetViewport(DownsampledX, DownsampledY, 0.0f, DownsampledX + DownsampledSizeX, DownsampledY + DownsampledSizeY, 1.0f);

			DrawRectangle(
				RHICmdList,
				0, 0,
				DownsampledSizeX, DownsampledSizeY,
				View.ViewRect.Min.X, View.ViewRect.Min.Y,
				View.ViewRect.Width(), View.ViewRect.Height(),
				FIntPoint(DownsampledSizeX, DownsampledSizeY),
				GSceneRenderTargets.GetBufferSizeXY(),
				*ScreenVertexShader,
				EDRF_UseTriangleOptimization);
		}
	}
}

bool FDeferredShadingSceneRenderer::ShouldCompositeEditorPrimitives(const FViewInfo& View)
{
	// If the show flag is enabled
	if(!View.Family->EngineShowFlags.CompositeEditorPrimitives)
	{
		return false;
	}

	if(GIsEditor && View.Family->EngineShowFlags.Wireframe)
	{
		// In Editor we want wire frame view modes to be in MSAA
		return true;
	}

	// Any elements that needed compositing were drawn then compositing should be done
	if( View.ViewMeshElements.Num() || View.TopViewMeshElements.Num() || View.BatchedViewElements.HasPrimsToDraw() || View.TopBatchedViewElements.HasPrimsToDraw() || View.VisibleEditorPrimitives.Num() )
	{
		return true;
	}

	return false;
}
