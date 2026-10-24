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

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
	virtual void OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
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

	/** Exponential transition rate per second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Slide", meta = (ClampMin = "0.1"))
	float SlideCameraTransitionSpeed = 12.0f;

	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> InputSubsystem;

private:
	float StandingCameraZ = 64.0f;
	float StandingCapsuleHalfHeight = 96.0f;
};
