// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "FPPlayerCharacter.generated.h"

class UCameraComponent;
class UFPParkourMovementComponent;
class UInputMappingContext;
class UInputAction;
class UEnhancedInputLocalPlayerSubsystem;
struct FInputActionValue;

/**
 * Base first-person player character for First Person Parkour.
 * Features a first-person camera attached to capsule, camera-relative movement,
 * mouse look, jumping, and integration with UFPParkourMovementComponent and Enhanced Input.
 */
UCLASS(config=Game)
class FIRSTPERSONPARKOUR_API AFPPlayerCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AFPPlayerCharacter(const FObjectInitializer& ObjectInitializer);
	virtual void CalcCamera(float DeltaTime, struct FMinimalViewInfo& OutResult) override;

	/** Called around native capsule resizing to compensate its actual displacement, including clearance adjustments. */
	void RestoreCameraAfterCapsuleResize(const FVector& PreviousEyeLocation);

protected:
	virtual void BeginPlay() override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	virtual void NotifyControllerChanged() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Handles camera-relative movement input (WASD). */
	void Move(const FInputActionValue& Value);

	/** Handles mouse look input (pitch and yaw). */
	void Look(const FInputActionValue& Value);
	void StartSprint();
	void StopSprint();
	void StartSlide();
	void StartJump();
	void RemoveDefaultMappingContext();

public:
	/** Returns FirstPersonCameraComponent subobject. */
	FORCEINLINE UCameraComponent* GetFirstPersonCameraComponent() const { return FirstPersonCameraComponent; }

	/** Returns custom ParkourMovementComponent. */
	FORCEINLINE UFPParkourMovementComponent* GetParkourMovementComponent() const { return ParkourMovementComponent; }

protected:
	/** First-person camera attached to the capsule. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> FirstPersonCameraComponent;

	/** Custom parkour movement component reference. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Movement", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UFPParkourMovementComponent> ParkourMovementComponent;

	/** Mapping context applied to the local player controller upon possession. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputMappingContext> DefaultMappingContext;

	/** Priority with which DefaultMappingContext is registered. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	int32 DefaultMappingPriority = 0;

	/** Move Input Action for 2D movement (WASD). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> MoveAction;

	/** Look Input Action for 2D mouse aiming. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> LookAction;

	/** Jump Input Action. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> JumpAction;

	/** IA_Sprint asset, mapped in IMC_Default. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> SprintAction;

	/** IA_Slide asset, mapped in IMC_Default. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> SlideAction;

	/** Camera drop from standing eye height, also used while awaiting standing clearance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Slide", meta = (ClampMin = "0", Units = "cm"))
	float SlideCameraLowering = 72.0f;

	/** Exponential lowering rate per second; 10 reaches 95 percent of the drop in 0.3 seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Slide", meta = (ClampMin = "0.1"))
	float SlideCameraTransitionSpeed = 10.0f;

	/** Exponential restoration rate per second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Slide", meta = (ClampMin = "0.1"))
	float SlideCameraRestoreSpeed = 9.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Slide", meta = (ClampMin = "0.1", Units = "cm"))
	float SlideCameraProbeRadius = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run")
	bool bEnableWallRunCameraEffects = true;

	/** Roll toward the wall, measured relative to the current view's right axis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0", ClampMax = "20", Units = "deg"))
	float WallRunCameraTiltAngle = 10.0f;

	/** Maximum additive FOV at sprint speed; scales down with horizontal speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0", ClampMax = "15", Units = "deg"))
	float WallRunCameraFOVBoost = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0", ClampMax = "8", Units = "cm"))
	float WallRunCameraOffset = 3.0f;

	/** Exponential response rates per second, independent of frame rate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0.1"))
	float WallRunCameraBlendInSpeed = 9.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0.1"))
	float WallRunCameraBlendOutSpeed = 7.0f;

	/** A single smooth vertical dip, with zero displacement and slope at both ends. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0", ClampMax = "2", Units = "cm"))
	float WallRunCameraEntryImpulse = 0.65f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0.05", ClampMax = "0.5", Units = "s"))
	float WallRunCameraEntryDuration = 0.18f;

	/** Tiny vertical contact motion; no procedural yaw/pitch shake. Zero disables it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0", ClampMax = "0.5", Units = "cm"))
	float WallRunCameraMotionAmplitude = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0.5", ClampMax = "4", Units = "Hz"))
	float WallRunCameraMotionFrequency = 2.4f;

	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> InputSubsystem;

private:
	void ApplyWallRunCameraEffects(float DeltaTime, FMinimalViewInfo& OutResult);
	float SmoothedWallRunRoll = 0.0f;
	float SmoothedWallRunFOV = 0.0f;
	float WallRunCameraMotionWeight = 0.0f;
	float WallRunCameraMotionPhase = 0.0f;
	float WallRunCameraEntryAge = 1.0f;
	bool bWasCameraWallRunning = false;
	FVector SmoothedWallRunOffset = FVector::ZeroVector;
	FVector WallRunCameraCollisionOffset = FVector::ZeroVector;

	FVector StandingCameraLocation = FVector(0.0f, 0.0f, 64.0f);
	float SmoothedCameraZ = 64.0f;
	FVector CameraCollisionOffset = FVector::ZeroVector;
	float StandingCapsuleHalfHeight = 96.0f;
};
