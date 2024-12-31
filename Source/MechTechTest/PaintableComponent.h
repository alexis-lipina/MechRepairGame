// Fill out your copyright notice in the Description page of Project Settings.

// credit to nicholas477 for most of the async rendertarget read stuff here

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "RHIResources.h"
#include "PaintableComponent.generated.h"

class UTextureRenderTarget2D;

struct FAsyncReadRTData
{
	FGPUFenceRHIRef TextureFence;
	FTexture2DRHIRef Texture;
	TAtomic<bool> FinishedRead; // false by default. becomes true when the read has completed. 
	TAtomic<bool> CurrentlyReading; // false by default. true while a reading thread/command is lined up, becomes false when it completes.
	TAtomic<bool> CurrentlyPolling; // false by default. true while a polling thread is lined up.
	TArray<FLinearColor> PixelColors;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCoverageCompleteDelegate, int, ChannelIndex);

/// <summary>
/// This component handles functionality for objects in the game which can be painted, washed, or otherwise have their surfaces altered by a tool.
/// </summary>
UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class MECHTECHTEST_API UPaintableComponent : public UActorComponent
{
	GENERATED_BODY()

public:	
	// Sets default values for this component's properties
	UPaintableComponent();

protected:
	// Called when the game starts
	virtual void BeginPlay() override;

	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

public:	
	UFUNCTION(BlueprintCallable)
	void InitializePaintable(UTextureRenderTarget2D* RenderTarget);

	UPROPERTY()
	UTextureRenderTarget2D* PaintRenderTarget;

	// 0...1 value for how "complete" the paint job on this render target is
	UPROPERTY(BlueprintReadOnly)
	FVector NormalizedCompletion = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	bool bUsesAutocomplete = true;

	// Used to kick-off a read (maybe mark an active read as dirty later?)
	UFUNCTION(BlueprintCallable)
	void AsyncReadPaint(UTextureRenderTarget2D* TextureRenderTarget, bool bFlushRHI);

	// Uses index of array as channel index (0=r, 1=g, 2=b, 3=a)
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	TArray<bool> bIsChannelCoverageComplete;

	UPROPERTY(BlueprintAssignable)
	FOnCoverageCompleteDelegate OnCoverageComplete;

	// 0...1 %age of "true coverage" necessary to flush the channel and call it done, to prevent ppl from needing to pixel-hunt for dirt
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = 0, ClampMax = 1))
	float CompletenessThreshold = 0.95f;

	TSharedPtr<FAsyncReadRTData, ESPMode::ThreadSafe> ReadRTData;

protected:
	bool bShouldRefresh = false;
	uint64 StartFrame;
};