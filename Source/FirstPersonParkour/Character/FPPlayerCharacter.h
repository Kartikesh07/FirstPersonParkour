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
class UFPGrappleCableComponent;
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
	virtual void CheckJumpInput(float DeltaTime) override;
	void ClearBufferedJump(const TCHAR* Reason);

	/** One press may wait for landing, standing clearance, or fresh wall contact. Zero disables buffering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Input", meta = (ClampMin = "0", ClampMax = "0.5", Units = "s"))
	float JumpBufferDuration = 0.12f;

	/** Accepted, consumed, expired and rejected jump transitions; also parkour.DebugTransitions 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Debug")
	bool bShowTransitionDebug = false;

	/** Called around native capsule resizing to compensate its actual displacement, including clearance adjustments. */
	void RestoreCameraAfterCapsuleResize(const FVector& PreviousEyeLocation);
	/** Cosmetic notification from the successful movement-component wall-jump path only. */
	void NotifyWallJumpCamera(const FVector& JumpedWallNormal);
	/** Cosmetic notification for every successful attach and release, including automatic detachment. */
	void NotifyGrappleVisuals(bool bAttached, const FVector& Momentum);

protected:
	virtual void BeginPlay() override;
	virtual void Landed(const FHitResult& Hit) override;
	virtual void OnJumped_Implementation() override;
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
	void EndJump();
	void CancelJump();
	void DebugJumpTransition(const TCHAR* Event) const;
	void StartGrapple();
	void EndGrapple();
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

	/** Stable rope width, material slots and tiling are editable on this component. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Parkour|Grapple Visuals")
	TObjectPtr<UFPGrappleCableComponent> GrappleCableComponent;

	/** Master feedback envelope. Stance eye-height and safety sweeps remain active. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Unified")
	bool bEnableCameraEffects = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Unified", meta=(ClampMin="0", ClampMax="2"))
	float CameraEffectsIntensity = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Unified", meta=(ClampMin="0.5", ClampMax="12", Units="Hz"))
	float CameraPositionFrequency = 4.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Unified", meta=(ClampMin="0.5", ClampMax="12", Units="Hz"))
	float CameraRotationFrequency = 3.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Unified", meta=(ClampMin="0.5", ClampMax="8", Units="Hz"))
	float CameraFOVFrequency = 2.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Unified", meta=(ClampMin="0.7", ClampMax="2"))
	float CameraSpringDampingRatio = 0.95f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Comfort", meta=(ClampMin="0", ClampMax="15", Units="cm"))
	float CameraMaxPositionOffset = 8.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Comfort", meta=(ClampMin="0", ClampMax="6", Units="deg"))
	float CameraMaxPitch = 3.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Comfort", meta=(ClampMin="0", ClampMax="15", Units="deg"))
	float CameraMaxRoll = 12.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Comfort", meta=(ClampMin="0", ClampMax="20", Units="deg"))
	float CameraMaxFOVBoost = 12.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Comfort", meta=(ClampMin="50", ClampMax="120", Units="deg"))
	float CameraMinFOV = 70.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Comfort", meta=(ClampMin="70", ClampMax="140", Units="deg"))
	float CameraMaxFOV = 110.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Comfort", meta=(ClampMin="1", ClampMax="30"))
	float CameraCollisionReleaseSpeed = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Inertia")
	bool bEnableCameraInertia = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Inertia", meta=(ClampMin="0", ClampMax="4", Units="cm"))
	float CameraInertiaDistance = 1.6f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Inertia", meta=(ClampMin="0", ClampMax="3", Units="deg"))
	float CameraInertiaPitch = 1.1f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Inertia", meta=(ClampMin="0", ClampMax="4", Units="deg"))
	float CameraStrafeRoll = 1.8f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Inertia", meta=(ClampMin="0", ClampMax="3", Units="deg"))
	float CameraTurnRoll = 1.3f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Inertia", meta=(ClampMin="1", Units="cm/s^2"))
	float CameraInertiaFullAcceleration = 2200.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Inertia", meta=(ClampMin="1", Units="deg/s"))
	float CameraTurnFullSpeed = 240.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Inertia", meta=(ClampMin="1", ClampMax="30"))
	float CameraAccelerationFilterSpeed = 12.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Slide")
	bool bEnableSlideCameraEffects = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Slide", meta=(ClampMin="0", ClampMax="5", Units="deg"))
	float SlideCameraPitch = 3.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Slide", meta=(ClampMin="0", ClampMax="4", Units="cm"))
	float SlideCameraForwardLean = 1.8f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Airborne", meta=(ClampMin="0", ClampMax="3", Units="deg"))
	float AirborneCameraPitch = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Landing", meta=(ClampMin="0", ClampMax="3", Units="deg"))
	float LandingCameraPitch = 1.2f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Grapple", meta=(ClampMin="0", ClampMax="4", Units="deg"))
	float GrappleSwingCameraRoll = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Grapple")
	bool bEnableGrappleCameraEffects = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Grapple", meta = (ClampMin = "0", ClampMax = "2", Units = "cm"))
	float GrappleAttachCameraImpulse = 1.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Grapple", meta = (ClampMin = "0", ClampMax = "5", Units = "deg"))
	float GrappleAttachFOVPulse = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Grapple", meta = (ClampMin = "0.05", ClampMax = "1", Units = "s"))
	float GrappleAttachFeedbackDuration = 0.22f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Grapple", meta = (ClampMin = "0", ClampMax = "2", Units = "cm"))
	float GrappleReleaseCameraImpulse = 1.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Grapple", meta = (ClampMin = "0", ClampMax = "5", Units = "deg"))
	float GrappleReleaseFOVPulse = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Grapple", meta = (ClampMin = "0.05", ClampMax = "1", Units = "s"))
	float GrappleReleaseFeedbackDuration = 0.2f;

	/** Taut-rope positional lean, weighted by swing speed and measured inward acceleration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Grapple", meta = (ClampMin = "0", ClampMax = "4", Units = "cm"))
	float GrappleTensionCameraOffset = 1.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Grapple", meta = (ClampMin = "0", ClampMax = "12", Units = "deg"))
	float GrappleTensionFOVBoost = 9.0f;

	/** Visual response rate per second, for both tension build and recovery. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Grapple", meta = (ClampMin = "0.1"))
	float GrappleCameraBlendSpeed = 8.0f;

	/** Speed reaching full release/tension feedback; independent of movement tuning. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Grapple", meta = (ClampMin = "1", Units = "cm/s"))
	float GrappleCameraFullEffectSpeed = 1800.0f;

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

	/** IA_Grapple: press E to attach, release to detach. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> GrappleAction;

	/** Camera drop from standing eye height, also used while awaiting standing clearance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Slide", meta = (ClampMin = "0", Units = "cm"))
	float SlideCameraLowering = 88.0f;

	/** Exponential lowering rate per second; 10 reaches 95 percent of the drop in 0.3 seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Slide", meta = (ClampMin = "0.1"))
	float SlideCameraTransitionSpeed = 10.0f;

	/** Exponential restoration rate per second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Slide", meta = (ClampMin = "0.1"))
	float SlideCameraRestoreSpeed = 9.0f;

	/** Slower eye-height recovery after a slide jump; capsule resize compensation is preserved. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Slide", meta = (ClampMin = "0.1"))
	float SlideJumpCameraRestoreSpeed = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sprint")
	bool bEnableSprintFOV = true;

	/** Speed FOV candidate; combined with other states by maximum, never summed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sprint", meta = (ClampMin = "0", ClampMax = "15", Units = "deg"))
	float SprintCameraFOVBoost = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sprint", meta = (ClampMin = "0.1"))
	float SprintCameraBlendInSpeed = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sprint", meta = (ClampMin = "0.1"))
	float SprintCameraBlendOutSpeed = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sprint")
	bool bEnableSprintMotion = true;

	/** Distance-driven vertical and lateral gait, only while grounded and standing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sprint", meta = (ClampMin = "0", ClampMax = "3", Units = "cm"))
	float SprintCameraMotionAmplitude = 1.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sprint", meta = (ClampMin = "0.5", ClampMax = "4", Units = "Hz"))
	float SprintCameraMotionFrequency = 1.8f;

	/** Small backward translation along horizontal travel, blended through acceleration/deceleration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sprint", meta = (ClampMin = "0", ClampMax = "1", Units = "cm"))
	float SprintCameraMomentumOffset = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sprint", meta = (ClampMin = "0.1"))
	float SprintCameraMotionBlendSpeed = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Jump")
	bool bEnableJumpCameraFeedback = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Jump", meta = (ClampMin = "0", ClampMax = "6", Units = "cm"))
	float JumpCameraImpulse = 2.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Jump", meta = (ClampMin = "0.1", ClampMax = "0.6", Units = "s"))
	float JumpCameraFeedbackDuration = 0.28f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Airborne")
	bool bEnableAirborneCameraFeedback = true;

	/** Downward during ascent, upward during descent; crosses zero smoothly at the apex. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Airborne", meta = (ClampMin = "0", ClampMax = "3", Units = "cm"))
	float AirborneCameraOffset = 1.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Airborne", meta = (ClampMin = "1", Units = "cm/s"))
	float AirborneCameraFullEffectSpeed = 700.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Airborne", meta = (ClampMin = "0.1"))
	float AirborneCameraBlendSpeed = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Landing")
	bool bEnableLandingCameraFeedback = true;

	/** Maximum compression target. All landings share one replaceable transition envelope. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Landing", meta = (ClampMin = "0", ClampMax = "10", Units = "cm"))
	float LandingCameraIntensity = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Landing", meta = (ClampMin = "0.1", ClampMax = "0.6", Units = "s"))
	float LandingCameraRecoveryDuration = 0.36f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Landing", meta = (ClampMin = "0", Units = "cm/s"))
	float LandingCameraMinSpeed = 140.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Landing", meta = (ClampMin = "1", Units = "cm/s"))
	float LandingCameraFullEffectSpeed = 850.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Slide", meta = (ClampMin = "0.1", Units = "cm"))
	float SlideCameraProbeRadius = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run")
	bool bEnableWallRunCameraEffects = true;

	/** Roll toward the wall, measured relative to the current view's right axis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0", ClampMax = "20", Units = "deg"))
	float WallRunCameraTiltAngle = 9.0f;

	/** Wall-run FOV candidate at sprint speed; never added to sprint FOV. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0", ClampMax = "15", Units = "deg"))
	float WallRunCameraFOVBoost = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0", ClampMax = "8", Units = "cm"))
	float WallRunCameraOffset = 3.0f;

	/** State-weight response rates per second; the unified spring handles the final pose. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0.1"))
	float WallRunCameraBlendInSpeed = 9.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0.1"))
	float WallRunCameraBlendOutSpeed = 7.0f;

	/** A single smooth vertical dip, with zero displacement and slope at both ends. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0", ClampMax = "2", Units = "cm"))
	float WallRunCameraEntryImpulse = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0.05", ClampMax = "0.5", Units = "s"))
	float WallRunCameraEntryDuration = 0.18f;

	/** Distance-driven wall contact texture. Zero disables it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0", ClampMax = "0.5", Units = "cm"))
	float WallRunCameraMotionAmplitude = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Run", meta = (ClampMin = "0.5", ClampMax = "4", Units = "Hz"))
	float WallRunCameraMotionFrequency = 2.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Jump")
	bool bEnableWallJumpCameraEffects = true;

	/** Peak world-space displacement away from the jumped wall; zero disables the kick. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Jump", meta = (ClampMin = "0", ClampMax = "3", Units = "cm"))
	float WallJumpCameraImpulseIntensity = 2.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Jump", meta = (ClampMin = "0", ClampMax = "5", Units = "deg"))
	float WallJumpCameraFOVPulse = 3.0f;

	/** Transition envelope duration; unified spring recovery can continue after it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Jump", meta = (ClampMin = "0.15", ClampMax = "1", Units = "s"))
	float WallJumpCameraRecoveryDuration = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Jump|Landing")
	bool bEnableWallJumpLandingFeedback = true;

	/** Maximum wall-jump landing compression, sharing the general landing pitch profile. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Jump|Landing", meta = (ClampMin = "0", ClampMax = "10", Units = "cm"))
	float WallJumpLandingIntensity = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Jump|Landing", meta = (ClampMin = "0.1", ClampMax = "0.6", Units = "s"))
	float WallJumpLandingRecoveryDuration = 0.34f;

	/** Impact speed into the landing surface below which no landing feedback is played. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Jump|Landing", meta = (ClampMin = "0", Units = "cm/s"))
	float WallJumpLandingMinSpeed = 150.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Wall Jump|Landing", meta = (ClampMin = "1", Units = "cm/s"))
	float WallJumpLandingFullEffectSpeed = 900.0f;

	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> InputSubsystem;

private:
	enum class ECameraEvent : uint8 { None, Jump, Landing, WallJump, WallContact, Grapple };
	/** A single replaceable transition, evaluated by world time, then fed through the shared springs. */
	struct FCameraFeedbackPulse
	{
		FVector StartOffset = FVector::ZeroVector;
		FVector PeakOffset = FVector::ZeroVector;
		FVector StartRotation = FVector::ZeroVector;
		FVector PeakRotation = FVector::ZeroVector;
		float StartFOV = 0.0f;
		float PeakFOV = 0.0f;
		double StartTime = -1.0;
		float Duration = 0.4f;
		void Sample(double Now, FVector& Offset, FVector& Rotation, float& FOV) const;
	};
	void TriggerCameraEvent(ECameraEvent Source, const FVector& Offset, const FVector& Rotation, float FOV, float Duration);
	void UpdateParkourCamera(float DeltaTime, double Now, const FRotator& ViewRotation);
	void ResetCameraFeedback();
	FVector ResolveCameraCollision(const FVector& Origin, const FVector& Desired) const;
	FCameraFeedbackPulse CameraEvent;
	ECameraEvent CameraEventSource = ECameraEvent::None;
	FVector CameraPosition = FVector::ZeroVector;
	FVector CameraPositionVelocity = FVector::ZeroVector;
	FVector CameraRotation = FVector::ZeroVector;
	FVector CameraRotationVelocity = FVector::ZeroVector;
	float CameraFOV = 0.0f;
	float CameraFOVVelocity = 0.0f;
	float CameraStateFOV = 0.0f;
	FVector FilteredCameraAcceleration = FVector::ZeroVector;
	FVector PreviousCameraVelocity = FVector::ZeroVector;
	FVector PreviousCameraLocation = FVector::ZeroVector;
	float PreviousCameraYaw = 0.0f;
	double LastCameraUpdateTime = -1.0;
	bool bHasCameraHistory = false;
	float SprintCameraMotionWeight = 0.0f;
	float SprintCameraMotionPhase = 0.0f;
	float SmoothedSprintFOV = 0.0f;
	float WallCameraWeight = 0.0f;
	float GrappleCameraWeight = 0.0f;
	float SlideCameraWeight = 0.0f;
	float AirCameraWeight = 0.0f;
	bool bWasCameraWallRunning = false;
	bool bWallJumpLandingPending = false;
	bool bCameraLandingWasWallJump = false;
	bool bJumpInputHeld = false;
	bool bJumpBuffered = false;
	bool bBufferedWallJumpOnly = false;
	double JumpBufferExpiresAt = 0.0;
	FString LastJumpRejection;

	FVector StandingCameraLocation = FVector(0.0f, 0.0f, 64.0f);
	float SmoothedCameraZ = 64.0f;
	FVector CameraCollisionOffset = FVector::ZeroVector;
	float StandingCapsuleHalfHeight = 96.0f;
};
