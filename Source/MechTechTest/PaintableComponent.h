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
	float NormalizedCompletion = 0.0f;

	// Used to kick-off a read (maybe mark an active read as dirty later?)
	UFUNCTION(BlueprintCallable)
	void AsyncReadPaint(UTextureRenderTarget2D* TextureRenderTarget, bool bFlushRHI);

	TSharedPtr<FAsyncReadRTData, ESPMode::ThreadSafe> ReadRTData;

protected:
	bool bShouldRefresh = false;
	uint64 StartFrame;
	// used to poll the thread data
	UFUNCTION()
	void OnNextFrame();
	// Calcuoated at startup, the required %age of the render target texture that must be filled in for the piece to be called "done" (since UV islands will not cover the entire texture)
	//float CompletionPercentCoverage = 0.0f;
};
