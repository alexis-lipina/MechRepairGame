// Fill out your copyright notice in the Description page of Project Settings.


#include "PaintableComponent.h"

#include "RHICommandList.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderGraphBuilder.h"
#include "Async/Async.h"
#include "CoreGlobals.h"
#include "TextureResource.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Engine/World.h"
#include "TimerManager.h"

UE_DISABLE_OPTIMIZATION

// Sets default values for this component's properties
UPaintableComponent::UPaintableComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	bIsChannelCoverageComplete = {false, false, false};
}


// Called when the game starts
void UPaintableComponent::BeginPlay()
{
	Super::BeginPlay();
}

// this should eventually do things like cache total coverage of the mesh UVs in texture space so future percentage calculations arent just percentages of texture but percentage of UV
void UPaintableComponent::InitializePaintable(UTextureRenderTarget2D* RenderTarget)
{
	PaintRenderTarget = RenderTarget;
	ReadRTData = MakeShared<FAsyncReadRTData, ESPMode::ThreadSafe>();
}


// Used to poll the waiting thread. Waits for the texture
// If bFlushRHI is false, then it checks the render fence. If the render fence hasn't completed, then the function early exits
static void PollRTRead(FRHICommandListImmediate& RHICmdList,
	TSharedPtr<FAsyncReadRTData, ESPMode::ThreadSafe> ReadData,
	TWeakObjectPtr<UPaintableComponent> PaintableComponent, bool bFlushRHI)
{
	SCOPED_NAMED_EVENT_TEXT("AsyncReadEntireRTAction::AsyncReadRT::PollRTRead", FColor::Magenta);

	check(IsInRenderingThread());
	//ReadData->FinishedRead = false;

	// If we didn't flush the RHI then make sure the previous rendering commands got done
	if (!bFlushRHI)
	{
		// Return if we haven't finished the texture commands
		if (!ReadData->TextureFence.IsValid() || !ReadData->TextureFence->Poll())
		{
			ReadData->CurrentlyPolling = false;
			//UE_LOG(LogTemp, Warning, TEXT("PollRTRead() called - texturefence still up, cannot process"));
			return;
		}
	}
	//UE_LOG(LogTemp, Warning, TEXT("PollRTRead() called - texturefence down! Processing texture."));

	// asyncing this saves about 5 fps. major issue im pretty sure is all the material swapping and render target bullshit 
	AsyncTask(ENamedThreads::AnyBackgroundHiPriTask, [bFlushRHI, &RHICmdList, ReadData, PaintableComponent]()
		{
			SCOPED_NAMED_EVENT_TEXT("AsyncReadEntireRTAction::AsyncReadRT::MapTexture", FColor::Magenta);
			void* OutputBuffer = NULL;
			int32 RowPitchInPixels, Height;

			if (bFlushRHI)
			{
				// This flushes the command list
				RHICmdList.MapStagingSurface(ReadData->Texture, ReadData->TextureFence, OutputBuffer, RowPitchInPixels, Height);
			}
			else
			{
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 2
				GDynamicRHI->RHIMapStagingSurface_RenderThread(RHICmdList, ReadData->Texture, INDEX_NONE, ReadData->TextureFence, OutputBuffer, RowPitchInPixels, Height);
#else
				GDynamicRHI->RHIMapStagingSurface_RenderThread(RHICmdList, ReadData->Texture, ReadData->TextureFence, OutputBuffer, RowPitchInPixels, Height);
#endif
			}

			const int32 Width = ReadData->Texture->GetSizeX();
			check(RowPitchInPixels >= Width);
			check(Height == ReadData->Texture->GetSizeY());
			const int32 SrcPitch = RowPitchInPixels * GPixelFormats[ReadData->Texture->GetFormat()].BlockBytes;

			ReadData->PixelColors.Empty(Width * Height);

			float PercentCompletion_R = 0.0f;
			float PercentCompletion_G = 0.0f;
			float PercentCompletion_B = 0.0f;
			float PercentCoverage = 0.0f; // how much of the render target actually is used by the mesh, essentially. Using Alpha channel.

			const EPixelFormat Format = ReadData->Texture->GetFormat();
			for (int32 YIndex = 0; YIndex < Height; YIndex++)
			{
				for (int32 X = 0; X < Width; ++X)
				{
					const int32 PixelOffset = X + (YIndex * RowPitchInPixels);

					FLinearColor& OutColor = ReadData->PixelColors.AddDefaulted_GetRef();
					switch (Format)
					{
					case EPixelFormat::PF_FloatRGBA:
					{
						FFloat16Color* OutputColor = reinterpret_cast<FFloat16Color*>(OutputBuffer) + PixelOffset;
						OutColor.R = OutputColor->R;
						OutColor.G = OutputColor->G;
						OutColor.B = OutputColor->B;
						OutColor.A = OutputColor->A;
						break;
					}
					case EPixelFormat::PF_B8G8R8A8:
					{
						FColor* OutputColor = reinterpret_cast<FColor*>(OutputBuffer) + PixelOffset;
						OutColor.R = OutputColor->R;
						OutColor.G = OutputColor->G;
						OutColor.B = OutputColor->B;
						OutColor.A = OutputColor->A;
						OutColor /= 255.f;
						break;
					}
					default:
						UE_LOG(LogTemp, Warning, TEXT("UAsyncReadRTAction: Unsupported RT format! Format: %d"), static_cast<int32>(Format)); // Unsupported, add a new switch statement.
					}
					PercentCompletion_R += OutColor.R / (Width * Height);  // gets the average by adding each pixels value / total number of pixels
					PercentCompletion_G += OutColor.G / (Width * Height);  // gets the average by adding each pixels value / total number of pixels
					PercentCompletion_B += OutColor.B / (Width * Height);  // gets the average by adding each pixels value / total number of pixels
					PercentCoverage += (1.0f - OutColor.A) / (Width * Height);  // gets the average by adding each pixels value / total number of pixels
				}
			}
			RHICmdList.UnmapStagingSurface(ReadData->Texture);
			ReadData->FinishedRead = true;
			PaintableComponent->NormalizedCompletion = FVector(PercentCompletion_R, PercentCompletion_G, PercentCompletion_B) / PercentCoverage;
			for (int i = 0; i < 3; i++)
			{
				if (PaintableComponent->NormalizedCompletion[i] > PaintableComponent->CompletenessThreshold && !PaintableComponent->bIsChannelCoverageComplete[i])
				{
					PaintableComponent->bIsChannelCoverageComplete[i] = true;
					AsyncTask(ENamedThreads::GameThread, [PaintableComponent, i]()
						{
							PaintableComponent->OnCoverageComplete.Broadcast(i);
						});
				}
			}
			ReadData->CurrentlyPolling = false;
			ReadData->CurrentlyReading = false;
			//UE_LOG(LogTemp, Warning, TEXT("PollRTRead() completed successfully! Texture processed"));
		});
	
}


void UPaintableComponent::AsyncReadPaint(UTextureRenderTarget2D* TextureRenderTarget, bool bFlushRHI)
{
	if (ReadRTData->CurrentlyReading)
	{
		return;
	}
	if (!bUsesAutocomplete)
	{
		return;
	}
	ReadRTData->CurrentlyReading = true;
	ReadRTData->FinishedRead = false; 
	//UE_LOG(LogTemp, Warning, TEXT("AsyncReadPaint called"));

	FTextureRenderTarget2DResource* TextureResource = (FTextureRenderTarget2DResource*)PaintRenderTarget->GetResource();
	check(TextureResource);
	check(TextureResource->GetRenderTargetTexture());

	StartFrame = GFrameCounter;

	// below is the actual "threaded" bit - this one copies the render target into a resource
	ENQUEUE_RENDER_COMMAND(FCopyRTAsync)([bFlushRHI = bFlushRHI, AsyncReadPtr = TWeakObjectPtr<UPaintableComponent>(this), TextureRHI = TextureResource->GetRenderTargetTexture(), ReadData = ReadRTData](FRHICommandListImmediate& RHICmdList)
		{
			//UE_LOG(LogTemp, Warning, TEXT("AsyncReadPaint's rendercommand called"));
			check(IsInRenderingThread());
			check(TextureRHI.IsValid());

			FGPUFenceRHIRef Fence = RHICreateGPUFence(TEXT("AsyncEntireRTReadback"));

			SCOPED_NAMED_EVENT_TEXT("AsyncReadEntireRTAction::AsyncReadRT", FColor::Magenta);

			FTexture2DRHIRef IORHITextureCPU;
			{
				SCOPED_NAMED_EVENT_TEXT("AsyncReadEntireRTAction::AsyncReadRT::CreateCopyTexture", FColor::Magenta);

				int32 Width, Height;
				Width = TextureRHI->GetSizeX();
				Height = TextureRHI->GetSizeY();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION > 2
				FRHITextureCreateDesc TextureDesc = FRHITextureCreateDesc::Create2D(TEXT("AsyncEntireRTReadback"), Width, Height, TextureRHI->GetFormat());
				TextureDesc.AddFlags(ETextureCreateFlags::CPUReadback);
				TextureDesc.InitialState = ERHIAccess::CopyDest;
#if ENGINE_MINOR_VERSION > 3
				IORHITextureCPU = GDynamicRHI->RHICreateTexture(FRHICommandListExecutor::GetImmediateCommandList(), TextureDesc);
#else // ENGINE_MINOR_VERSION
				IORHITextureCPU = GDynamicRHI->RHICreateTexture(TextureDesc);
#endif // ENGINE_MINOR_VERSION
#else
				FRHIResourceCreateInfo CreateInfo(TEXT("AsyncRTReadback"));
				IORHITextureCPU = RHICreateTexture2D(Width, Height, TextureRHI->GetFormat(), 1, 1, TexCreate_CPUReadback, ERHIAccess::CopyDest, CreateInfo);
#endif

				FRHICopyTextureInfo CopyTextureInfo;
				CopyTextureInfo.Size = FIntVector(Width, Height, 1);
				CopyTextureInfo.SourceMipIndex = 0;
				CopyTextureInfo.DestMipIndex = 0;
				CopyTextureInfo.SourcePosition = FIntVector(0, 0, 0);
				CopyTextureInfo.DestPosition = FIntVector(0, 0, 0);

				RHICmdList.Transition(FRHITransitionInfo(TextureRHI, ERHIAccess::Unknown, ERHIAccess::CopySrc));
				RHICmdList.CopyTexture(TextureRHI, IORHITextureCPU, CopyTextureInfo);

				RHICmdList.Transition(FRHITransitionInfo(IORHITextureCPU, ERHIAccess::CopyDest, ERHIAccess::CopySrc));
				RHICmdList.WriteGPUFence(Fence);
			}
			check(Fence.IsValid());

			ReadData->Texture = IORHITextureCPU;
			ReadData->TextureFence = Fence;
			//UE_LOG(LogTemp, Warning, TEXT("AsyncReadPaint's thread that writes to the command list and shit is DONE."));

			// If we flush the RHI then we can just go ahead and read the mapped texture asap
			if (bFlushRHI)
			{
				PollRTRead(RHICmdList, ReadData, AsyncReadPtr, bFlushRHI);
			}
		});

	//GetWorld()->GetTimerManager().SetTimerForNextTick(this, &UPaintableComponent::OnNextFrame);
	bShouldRefresh = true;
}



void UPaintableComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	if (!ReadRTData.IsValid())
	{
		return;
	}
	if (ReadRTData->FinishedRead)
	{
		return;
	}
	if (!bShouldRefresh)
	{
		return;
	}
	if (!bUsesAutocomplete)
	{
		return;
	}

	if (!ReadRTData->CurrentlyPolling)
	{
		ReadRTData->CurrentlyPolling = true;
//		AsyncTask(ENamedThreads::AnyBackgroundHiPriTask, [this]()
//			{
				ENQUEUE_RENDER_COMMAND(FReadRTAsync)([WeakThis = TWeakObjectPtr<UPaintableComponent>(this), ReadRTData = ReadRTData](FRHICommandListImmediate& RHICmdList)
					{
						//UE_LOG(LogTemp, Warning, TEXT("OnNextFrame calling PollRTRead..."));
						PollRTRead(RHICmdList, ReadRTData, WeakThis, false);
					});
//			});
	}
}
/*
void UPaintableComponent::OnNextFrame()
{
	//const int32 FramesWaited = GFrameCounter - StartFrame;
	//UE_LOG(LogTemp, Warning, TEXT("OnNextFrame() called - Frames waited: %d"), FramesWaited);

	check(IsInGameThread());
	check(ReadRTData.IsValid());

	if (ReadRTData->FinishedRead)
	{
		//OnReadEntireRenderTarget.Broadcast(ReadRTData->PixelColors);
		//SetReadyToDestroy();
		bShouldRefresh = false; // concerned about possibility of bShouldRefresh being set between the kickoff and actual read & there being unprocessed data.
	}
	else
	{
		ReadRTData->CurrentlyPolling = true;
		ENQUEUE_RENDER_COMMAND(FReadRTAsync)([WeakThis = TWeakObjectPtr<UPaintableComponent>(this), ReadRTData = ReadRTData](FRHICommandListImmediate& RHICmdList)
			{
				//UE_LOG(LogTemp, Warning, TEXT("OnNextFrame calling PollRTRead..."));
				PollRTRead(RHICmdList, ReadRTData, WeakThis, false);
			});

		GetWorld()->GetTimerManager().SetTimerForNextTick(this, &UPaintableComponent::OnNextFrame); // pretty sure this +1s the number of calls to OnNextFrame
	}
}*/

UE_ENABLE_OPTIMIZATION