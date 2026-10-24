// Copyright Epic Games, Inc. All Rights Reserved.

#include "Character/FPPlayerCharacter.h"
#include "Character/FPGrappleCableComponent.h"
#include "Movement/FPParkourMovementComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "UObject/ConstructorHelpers.h"

static TAutoConsoleVariable<int32> CVarParkourTransitionDebug(
	TEXT("parkour.DebugTransitions"), 0, TEXT("Log accepted, consumed, expired and rejected jump transitions: 0 = off, 1 = on."));
DEFINE_LOG_CATEGORY_STATIC(LogParkourTransitions, Log, All);

AFPPlayerCharacter::AFPPlayerCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UFPParkourMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;
	static ConstructorHelpers::FObjectFinder<UInputAction> GrappleInput(TEXT("/Game/Input/IA_Grapple.IA_Grapple"));
	GrappleAction = GrappleInput.Object;

	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(40.0f, 96.0f);

	// First person camera drives pitch; pawn rotates with controller yaw
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = true;
	bUseControllerRotationRoll = false;

	// Cache custom movement component and configure rotation behavior
	ParkourMovementComponent = Cast<UFPParkourMovementComponent>(GetCharacterMovement());
	if (ParkourMovementComponent)
	{
		ParkourMovementComponent->bOrientRotationToMovement = false;
	}

	// Create and attach first-person camera to the capsule at eye height
	FirstPersonCameraComponent = ObjectInitializer.CreateDefaultSubobject<UCameraComponent>(this, TEXT("FirstPersonCamera"));
	FirstPersonCameraComponent->SetupAttachment(GetCapsuleComponent());
	FirstPersonCameraComponent->SetRelativeLocation(FVector(0.0f, 0.0f, 64.0f));
	FirstPersonCameraComponent->bUsePawnControlRotation = true;

	GrappleCableComponent = ObjectInitializer.CreateDefaultSubobject<UFPGrappleCableComponent>(this, TEXT("GrappleCable"));
	GrappleCableComponent->SetupAttachment(GetCapsuleComponent());
	GrappleCableComponent->AddTickPrerequisiteComponent(ParkourMovementComponent);
}

void AFPPlayerCharacter::BeginPlay()
{
	Super::BeginPlay();
	StandingCameraLocation = FirstPersonCameraComponent->GetRelativeLocation();
	SmoothedCameraZ = StandingCameraLocation.Z;
	StandingCapsuleHalfHeight = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
}

void AFPPlayerCharacter::FCameraFeedbackPulse::Sample(double Now, FVector& Offset, float& FOV) const
{
	Offset = FVector::ZeroVector;
	FOV = 0.0f;
	if (StartTime < 0.0 || Now < StartTime || Now >= StartTime + Duration)
	{
		return;
	}
	const float Age = static_cast<float>((Now - StartTime) / Duration);
	const auto SmoothStep = [](float T) { return T * T * (3.0f - 2.0f * T); };
	// Sixteen percent attack, then a longer recovery; zero slope at both ends
	// and at the peak. No oscillation, random shake or additive pulse stacking.
	constexpr float AttackFraction = 0.16f;
	const float Pulse = Age < AttackFraction ? SmoothStep(Age / AttackFraction)
		: 1.0f - SmoothStep((Age - AttackFraction) / (1.0f - AttackFraction));
	const float Carry = (1.0f - SmoothStep(Age)) * (1.0f - Pulse);
	// The nonnegative weights sum to at most one, bounding even rapid chaining.
	Offset = StartOffset * Carry + PeakOffset * Pulse;
	FOV = StartFOV * Carry + PeakFOV * Pulse;
}

void AFPPlayerCharacter::FCameraFeedbackPulse::Trigger(double Now, const FVector& Offset, float FOV, float RecoveryDuration)
{
	// Retain the currently rendered contribution so re-triggering cannot snap it.
	FVector CurrentOffset;
	float CurrentFOV;
	Sample(Now, CurrentOffset, CurrentFOV);
	StartOffset = CurrentOffset;
	StartFOV = CurrentFOV;
	PeakOffset = Offset;
	PeakFOV = FOV;
	StartTime = Now;
	Duration = RecoveryDuration;
}

void AFPPlayerCharacter::NotifyWallJumpCamera(const FVector& JumpedWallNormal)
{
	bWallJumpLandingPending = true;
	if (!bEnableWallJumpCameraEffects || JumpedWallNormal.ContainsNaN())
	{
		return;
	}
	const FVector AwayFromWall = JumpedWallNormal.GetSafeNormal2D();
	if (AwayFromWall.IsNearlyZero())
	{
		return;
	}
	WallJumpCameraPulse.Trigger(GetWorld()->GetTimeSeconds(),
		AwayFromWall * FMath::Clamp(WallJumpCameraImpulseIntensity, 0.0f, 3.0f),
		FMath::Clamp(WallJumpCameraFOVPulse, 0.0f, 5.0f),
		FMath::Clamp(WallJumpCameraRecoveryDuration, 0.15f, 1.0f));
}

void AFPPlayerCharacter::NotifyGrappleVisuals(bool bAttached, const FVector& Momentum)
{
	if (!bAttached)
	{
		GrappleCableComponent->HideGrappleCable();
	}
	if (!bEnableGrappleCameraEffects || !IsLocallyControlled() || Momentum.ContainsNaN())
	{
		return;
	}
	const FVector Direction = bAttached
		? (ParkourMovementComponent->GetGrappleAnchor() - GetActorLocation()).GetSafeNormal()
		: Momentum.GetSafeNormal();
	const float Strength = bAttached ? 1.0f
		: FMath::Clamp(Momentum.Size() / FMath::Max(1.0f, GrappleCameraFullEffectSpeed), 0.0f, 1.0f);
	// Attach/release reuse one bounded envelope so rapid shots do not stack kicks.
	GrappleCameraPulse.Trigger(GetWorld()->GetTimeSeconds(),
		Direction * Strength * FMath::Clamp(bAttached ? GrappleAttachCameraImpulse : GrappleReleaseCameraImpulse, 0.0f, 2.0f),
		Strength * FMath::Clamp(bAttached ? GrappleAttachFOVPulse : GrappleReleaseFOVPulse, 0.0f, 5.0f),
		FMath::Clamp(bAttached ? GrappleAttachFeedbackDuration : GrappleReleaseFeedbackDuration, 0.05f, 1.0f));
}

void AFPPlayerCharacter::SampleGrappleCameraEffects(float DeltaTime, FVector& Offset, float& FOV)
{
	FVector TargetOffset = FVector::ZeroVector;
	float TargetFOV = 0.0f;
	if (bEnableGrappleCameraEffects && ParkourMovementComponent->IsGrappling())
	{
		const FVector ToAnchor = ParkourMovementComponent->GetGrappleAnchor() - GetActorLocation();
		const FVector RopeDirection = ToAnchor.GetSafeNormal();
		const float Slack = FMath::Max(0.0f, ParkourMovementComponent->GetActiveGrappleRopeLength() - ToAnchor.Size());
		const float TangentialSpeed = FVector::VectorPlaneProject(GetVelocity(), RopeDirection).Size();
		// Geometric tautness is only a visual weight, never a force or rope constraint.
		const float TensionWeight = (1.0f - FMath::Clamp(Slack / 8.0f, 0.0f, 1.0f))
			* FMath::Clamp(TangentialSpeed / FMath::Max(1.0f, GrappleCameraFullEffectSpeed), 0.0f, 1.0f);
		TargetOffset = RopeDirection * TensionWeight * FMath::Clamp(GrappleTensionCameraOffset, 0.0f, 1.0f);
		TargetFOV = TensionWeight * FMath::Clamp(GrappleTensionFOVBoost, 0.0f, 3.0f);
	}
	const float Alpha = 1.0f - FMath::Exp(-FMath::Max(0.1f, GrappleCameraBlendSpeed) * FMath::Max(0.0f, DeltaTime));
	SmoothedGrappleCameraOffset = FMath::Lerp(SmoothedGrappleCameraOffset, TargetOffset, Alpha);
	SmoothedGrappleFOV = FMath::Lerp(SmoothedGrappleFOV, TargetFOV, Alpha);
	Offset = FVector::ZeroVector;
	FOV = 0.0f;
	if (bEnableGrappleCameraEffects)
	{
		GrappleCameraPulse.Sample(GetWorld()->GetTimeSeconds(), Offset, FOV);
	}
	Offset += SmoothedGrappleCameraOffset;
	FOV += SmoothedGrappleFOV;
}

void AFPPlayerCharacter::OnJumped_Implementation()
{
	Super::OnJumped_Implementation();
	// Native successful jumps only: buffered/blocked attempts and custom wall jumps
	// do not invoke this event. This also covers successful slide jumps.
	if (bEnableJumpCameraFeedback)
	{
		NormalJumpCameraPulse.Trigger(GetWorld()->GetTimeSeconds(),
			-FVector::UpVector * FMath::Clamp(JumpCameraImpulse, 0.0f, 2.0f),
			0.0f, FMath::Clamp(JumpCameraFeedbackDuration, 0.1f, 0.6f));
	}
}

void AFPPlayerCharacter::SampleMovementCameraEffects(float DeltaTime, FVector& Offset, float& FOV)
{
	const float Dt = FMath::Max(0.0f, DeltaTime);
	const bool bSpecialMovement = ParkourMovementComponent->IsWallRunning() || ParkourMovementComponent->IsGrappling();
	const bool bGrounded = ParkourMovementComponent->IsMovingOnGround();
	const bool bFalling = ParkourMovementComponent->IsFalling();
	// Actual speed, rather than the sprint button, preserves deceleration and slide
	// momentum. Special movement owns its own FOV; residual sprint FOV fades out.
	const float SpeedWeight = !bSpecialMovement && (bGrounded || bFalling)
		? FMath::Clamp((GetVelocity().Size2D() - ParkourMovementComponent->WalkSpeed)
			/ FMath::Max(1.0f, ParkourMovementComponent->SprintSpeed - ParkourMovementComponent->WalkSpeed), 0.0f, 1.0f)
		: 0.0f;
	const float TargetFOV = bEnableSprintFOV ? SpeedWeight * FMath::Clamp(SprintCameraFOVBoost, 0.0f, 15.0f) : 0.0f;
	const float FOVRate = TargetFOV > SmoothedSprintFOV ? SprintCameraBlendInSpeed : SprintCameraBlendOutSpeed;
	SmoothedSprintFOV = FMath::Lerp(SmoothedSprintFOV, TargetFOV, 1.0f - FMath::Exp(-FMath::Max(0.1f, FOVRate) * Dt));

	const float MotionTarget = bEnableSprintMotion && bGrounded && !bIsCrouched
		&& !ParkourMovementComponent->IsSliding() ? SpeedWeight : 0.0f;
	const float MotionAlpha = 1.0f - FMath::Exp(-FMath::Max(0.1f, SprintCameraMotionBlendSpeed) * Dt);
	SprintCameraMotionWeight = FMath::Lerp(SprintCameraMotionWeight, MotionTarget, MotionAlpha);
	const FVector MomentumTarget = -GetVelocity().GetSafeNormal2D() * MotionTarget
		* FMath::Clamp(SprintCameraMomentumOffset, 0.0f, 1.0f);
	SmoothedSprintMomentumOffset = FMath::Lerp(SmoothedSprintMomentumOffset, MomentumTarget, MotionAlpha);
	// Continuous phase, a smoothly fading envelope, and sub-centimeter motion.
	const float Frequency = FMath::Clamp(SprintCameraMotionFrequency, 0.5f, 4.0f)
		* FMath::Lerp(0.6f, 1.0f, SprintCameraMotionWeight);
	SprintCameraMotionPhase = FMath::Fmod(SprintCameraMotionPhase + 2.0f * PI * Frequency * Dt, 2.0f * PI);
	const float FootstepZ = FMath::Sin(SprintCameraMotionPhase) * SprintCameraMotionWeight
		* FMath::Clamp(SprintCameraMotionAmplitude, 0.0f, 0.5f);

	const float AirTarget = bEnableAirborneCameraFeedback && bFalling && !bSpecialMovement
		? -FMath::Clamp(GetVelocity().Z / FMath::Max(1.0f, AirborneCameraFullEffectSpeed), -1.0f, 1.0f)
			* FMath::Clamp(AirborneCameraOffset, 0.0f, 1.0f) : 0.0f;
	SmoothedAirborneCameraZ = FMath::Lerp(SmoothedAirborneCameraZ, AirTarget,
		1.0f - FMath::Exp(-FMath::Max(0.1f, AirborneCameraBlendSpeed) * Dt));
	float UnusedJumpFOV;
	NormalJumpCameraPulse.Sample(GetWorld()->GetTimeSeconds(), Offset, UnusedJumpFOV);
	Offset += SmoothedSprintMomentumOffset + FVector::UpVector * (FootstepZ + SmoothedAirborneCameraZ);
	FOV = SmoothedSprintFOV;
}

void AFPPlayerCharacter::Landed(const FHitResult& Hit)
{
	// Landed is called before native falling velocity is converted to ground motion.
	// Sample it here; feedback never writes to gameplay velocity or transforms.
	// Choose exactly one profile. Disabling wall-jump landing feedback must not
	// silently replace it with the general landing effect.
	const bool bWallProfile = bWallJumpLandingPending;
	const bool bFeedbackEnabled = bWallProfile
		? bEnableWallJumpCameraEffects && bEnableWallJumpLandingFeedback : bEnableLandingCameraFeedback;
	if (bFeedbackEnabled)
	{
		const float ImpactSpeed = FMath::Max(0.0f, -FVector::DotProduct(GetVelocity(), Hit.ImpactNormal));
		const float MinSpeed = FMath::Max(0.0f, bWallProfile ? WallJumpLandingMinSpeed : LandingCameraMinSpeed);
		const float FullSpeed = FMath::Max(MinSpeed + 1.0f, bWallProfile ? WallJumpLandingFullEffectSpeed : LandingCameraFullEffectSpeed);
		const float Strength = FMath::Clamp((ImpactSpeed - MinSpeed) / (FullSpeed - MinSpeed), 0.0f, 1.0f);
		if (Strength > 0.0f)
		{
			WallJumpLandingPulse.Trigger(GetWorld()->GetTimeSeconds(),
				-FVector::UpVector * FMath::Clamp(bWallProfile ? WallJumpLandingIntensity : LandingCameraIntensity, 0.0f, 2.0f) * Strength,
				0.0f, FMath::Clamp(bWallProfile ? WallJumpLandingRecoveryDuration : LandingCameraRecoveryDuration, 0.1f, 0.6f));
		}
	}
	bWallJumpLandingPending = false;
	Super::Landed(Hit);
}

void AFPPlayerCharacter::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	// Resolve the eye after movement, when the view is requested. Collision only
	// constrains this frame's rendered position; it must not reset the smoothing state.
	FVector CameraLocation = StandingCameraLocation;
	const float CapsuleReduction = StandingCapsuleHalfHeight - GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	const float TargetZ = StandingCameraLocation.Z + CapsuleReduction - (bIsCrouched ? SlideCameraLowering : 0.0f);
	const float RestoreRate = ParkourMovementComponent->IsFalling() ? SlideJumpCameraRestoreSpeed : SlideCameraRestoreSpeed;
	const float Rate = bIsCrouched ? SlideCameraTransitionSpeed : RestoreRate;
	const float Alpha = 1.0f - FMath::Exp(-FMath::Max(0.1f, Rate) * FMath::Max(0.0f, DeltaTime));
	SmoothedCameraZ = FMath::Lerp(SmoothedCameraZ, TargetZ, Alpha);
	CameraLocation.Z = SmoothedCameraZ;
	// New obstructions constrain the view immediately for safety. Once clear,
	// release that correction smoothly rather than snapping back to the desired eye.
	CameraCollisionOffset *= FMath::Exp(-FMath::Max(0.1f, SlideCameraRestoreSpeed) * FMath::Max(0.0f, DeltaTime));
	CameraLocation += CameraCollisionOffset;

	// The smoothed eye can briefly extend beyond the shortened capsule. Keep it
	// outside overhead geometry while the collision capsule passes underneath.
	const FTransform CapsuleTransform = GetCapsuleComponent()->GetComponentTransform();
	FHitResult CameraHit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(SlideCamera), false, this);
	if ((bIsCrouched || !CameraCollisionOffset.IsNearlyZero()
		|| !FMath::IsNearlyEqual(CameraLocation.Z, StandingCameraLocation.Z, 0.01f))
		&& GetWorld()->SweepSingleByChannel(CameraHit, CapsuleTransform.GetLocation(),
		CapsuleTransform.TransformPosition(CameraLocation), FQuat::Identity, ECC_Camera,
		FCollisionShape::MakeSphere(FMath::Max(0.1f, SlideCameraProbeRadius)), Params))
	{
		const FVector SafeLocation = CapsuleTransform.InverseTransformPosition(CameraHit.Location);
		CameraCollisionOffset += SafeLocation - CameraLocation;
		CameraLocation = SafeLocation;
	}
	FirstPersonCameraComponent->SetRelativeLocation(CameraLocation);
	Super::CalcCamera(DeltaTime, OutResult);
	ApplyWallRunCameraEffects(DeltaTime, OutResult);
}

void AFPPlayerCharacter::ApplyWallRunCameraEffects(float DeltaTime, FMinimalViewInfo& OutResult)
{
	const float Dt = FMath::Max(0.0f, DeltaTime);
	const bool bWallRunning = bEnableWallRunCameraEffects && ParkourMovementComponent->IsWallRunning();
	const FVector TowardWall = -ParkourMovementComponent->GetWallRunNormal();
	const float SpeedAlpha = FMath::Clamp(ParkourMovementComponent->Velocity.Size2D()
		/ FMath::Max(1.0f, ParkourMovementComponent->SprintSpeed), 0.0f, 1.0f);
	const float BlendRate = bWallRunning ? WallRunCameraBlendInSpeed : WallRunCameraBlendOutSpeed;
	const float BlendAlpha = 1.0f - FMath::Exp(-FMath::Max(0.1f, BlendRate) * Dt);
	const FVector ViewRight = FRotationMatrix(FRotator(0.0f, OutResult.Rotation.Yaw, 0.0f)).GetUnitAxis(EAxis::Y);
	const float WallSide = FMath::Clamp(FVector::DotProduct(TowardWall, ViewRight), -1.0f, 1.0f);
	// Invert the wall-side sign for the intended first-person screen tilt.
	// Looking around keeps the tilt tied to the wall's current screen side.
	const float TargetRoll = bWallRunning ? -WallSide * FMath::Clamp(WallRunCameraTiltAngle, 0.0f, 20.0f) : 0.0f;
	const float TargetFOV = bWallRunning ? SpeedAlpha * FMath::Clamp(WallRunCameraFOVBoost, 0.0f, 15.0f) : 0.0f;
	const FVector TargetOffset = bWallRunning ? TowardWall * FMath::Clamp(WallRunCameraOffset, 0.0f, 8.0f) : FVector::ZeroVector;
	SmoothedWallRunRoll = FMath::Lerp(SmoothedWallRunRoll, TargetRoll, BlendAlpha);
	SmoothedWallRunFOV = FMath::Lerp(SmoothedWallRunFOV, TargetFOV, BlendAlpha);
	SmoothedWallRunOffset = FMath::Lerp(SmoothedWallRunOffset, TargetOffset, BlendAlpha);
	WallRunCameraMotionWeight = FMath::Lerp(WallRunCameraMotionWeight, bWallRunning ? SpeedAlpha : 0.0f, BlendAlpha);

	const float EntryDuration = FMath::Clamp(WallRunCameraEntryDuration, 0.05f, 0.5f);
	if (bWallRunning && !bWasCameraWallRunning)
	{
		WallRunCameraEntryAge = 0.0f;
	}
	bWasCameraWallRunning = bWallRunning;
	WallRunCameraEntryAge = FMath::Min(EntryDuration, WallRunCameraEntryAge + Dt);
	const float EntryPulse = FMath::Square(FMath::Sin(PI * WallRunCameraEntryAge / EntryDuration));
	// Continuous phase avoids a bob discontinuity on reattachment. Both motion
	// amplitude and frequency depend on smoothed speed, with no random jitter.
	const float Frequency = FMath::Clamp(WallRunCameraMotionFrequency, 0.5f, 4.0f)
		* FMath::Lerp(0.5f, 1.0f, WallRunCameraMotionWeight);
	WallRunCameraMotionPhase = FMath::Fmod(WallRunCameraMotionPhase + 2.0f * PI * Frequency * Dt, 2.0f * PI);
	const float ContactMotion = FMath::Sin(WallRunCameraMotionPhase)
		* FMath::Clamp(WallRunCameraMotionAmplitude, 0.0f, 0.5f) * WallRunCameraMotionWeight;
	const float EntryDip = EntryPulse * FMath::Clamp(WallRunCameraEntryImpulse, 0.0f, 2.0f);
	FVector JumpOffset;
	FVector LandingOffset;
	float JumpFOV;
	float LandingFOV;
	const double Now = GetWorld()->GetTimeSeconds();
	WallJumpCameraPulse.Sample(Now, JumpOffset, JumpFOV);
	WallJumpLandingPulse.Sample(Now, LandingOffset, LandingFOV);
	FVector GrappleOffset;
	float GrappleFOV;
	SampleGrappleCameraEffects(Dt, GrappleOffset, GrappleFOV);
	FVector MovementOffset;
	float MovementFOV;
	SampleMovementCameraEffects(Dt, MovementOffset, MovementFOV);
	// Share the existing camera collision solution with all cosmetic displacement.
	const FVector VisualOffset = SmoothedWallRunOffset + FVector::UpVector * (ContactMotion - EntryDip)
		+ JumpOffset + LandingOffset + GrappleOffset + MovementOffset;

	// Preserve the existing slide-camera solution. Sweep the additional visual
	// displacement from that safe eye, constraining it immediately at obstructions.
	// Collision correction relaxes separately, so clearance never causes a snap.
	WallRunCameraCollisionOffset *= FMath::Exp(-FMath::Max(0.1f, WallRunCameraBlendOutSpeed) * Dt);
	const FVector BaseEye = OutResult.Location;
	const FVector DesiredEye = BaseEye + VisualOffset + WallRunCameraCollisionOffset;
	if (!DesiredEye.Equals(BaseEye, 0.001f))
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(WallRunCamera), false, this);
		const FCollisionShape Probe = FCollisionShape::MakeSphere(FMath::Max(0.1f, SlideCameraProbeRadius));
		FHitResult CameraHit;
		bool bHit = GetWorld()->SweepSingleByChannel(CameraHit, BaseEye, DesiredEye, FQuat::Identity, ECC_Camera, Probe, Params);
		// Wall detection follows capsule collision responses. Also respect those
		// responses here for walls configured to block Pawn but ignore Camera.
		FCollisionResponseParams Response;
		GetCapsuleComponent()->InitSweepCollisionParams(Params, Response);
		FHitResult PawnHit;
		if (GetWorld()->SweepSingleByChannel(PawnHit, BaseEye, DesiredEye, FQuat::Identity,
			GetCapsuleComponent()->GetCollisionObjectType(), Probe, Params, Response)
			&& (!bHit || PawnHit.Time < CameraHit.Time))
		{
			CameraHit = PawnHit;
			bHit = true;
		}
		OutResult.Location = bHit ? (CameraHit.bStartPenetrating ? BaseEye : CameraHit.Location) : DesiredEye;
		WallRunCameraCollisionOffset += OutResult.Location - DesiredEye;
	}

	// Super::CalcCamera supplies a fresh base view each call. Apply offsets only
	// to that view, never to control rotation, component rotation or stored FOV.
	OutResult.Rotation.Roll = FRotator::NormalizeAxis(OutResult.Rotation.Roll + SmoothedWallRunRoll);
	OutResult.FOV = FMath::Clamp(OutResult.FOV + SmoothedWallRunFOV + JumpFOV + GrappleFOV + MovementFOV, 5.0f, 170.0f);
	OutResult.FirstPersonFOV = FMath::Clamp(OutResult.FirstPersonFOV + SmoothedWallRunFOV + JumpFOV + GrappleFOV + MovementFOV, 5.0f, 170.0f);
}

void AFPPlayerCharacter::RestoreCameraAfterCapsuleResize(const FVector& PreviousEyeLocation)
{
	const FVector RestoredLocation = GetCapsuleComponent()->GetComponentTransform().InverseTransformPosition(PreviousEyeLocation);
	SmoothedCameraZ += RestoredLocation.Z - FirstPersonCameraComponent->GetRelativeLocation().Z;
	FirstPersonCameraComponent->SetRelativeLocation(RestoredLocation);
}

void AFPPlayerCharacter::NotifyControllerChanged()
{
	WallJumpCameraPulse = FCameraFeedbackPulse();
	WallJumpLandingPulse = FCameraFeedbackPulse();
	NormalJumpCameraPulse = FCameraFeedbackPulse();
	SmoothedSprintFOV = 0.0f;
	SprintCameraMotionWeight = 0.0f;
	SprintCameraMotionPhase = 0.0f;
	SmoothedSprintMomentumOffset = FVector::ZeroVector;
	SmoothedAirborneCameraZ = 0.0f;
	bWallJumpLandingPending = false;
	// Possession changes must not leave sprint latched on the previous pawn.
	StopSprint();
	ParkourMovementComponent->StopGrapple(TEXT("controller changed"));
	GrappleCameraPulse = FCameraFeedbackPulse();
	SmoothedGrappleCameraOffset = FVector::ZeroVector;
	SmoothedGrappleFOV = 0.0f;
	GrappleCableComponent->HideGrappleCable();
	ParkourMovementComponent->StopSlide();
	EndJump();
	ClearBufferedJump(TEXT("rejected: controller changed"));
	RemoveDefaultMappingContext();
	Super::NotifyControllerChanged();

	// Add input mapping context when possessed by a local player controller
	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			if (DefaultMappingContext)
			{
				Subsystem->AddMappingContext(DefaultMappingContext, DefaultMappingPriority);
				InputSubsystem = Subsystem;
			}
		}
	}
}

void AFPPlayerCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ParkourMovementComponent->StopGrapple(TEXT("end play"));
	GrappleCableComponent->HideGrappleCable();
	StopSprint();
	EndJump();
	ClearBufferedJump(TEXT("rejected: end play"));
	ParkourMovementComponent->StopSlide();
	RemoveDefaultMappingContext();
	Super::EndPlay(EndPlayReason);
}

void AFPPlayerCharacter::RemoveDefaultMappingContext()
{
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = InputSubsystem.Get())
	{
		Subsystem->RemoveMappingContext(DefaultMappingContext);
	}
	InputSubsystem.Reset();
}

void AFPPlayerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		if (GrappleAction)
		{
			EnhancedInputComponent->BindAction(GrappleAction, ETriggerEvent::Started, this, &AFPPlayerCharacter::StartGrapple);
			EnhancedInputComponent->BindAction(GrappleAction, ETriggerEvent::Completed, this, &AFPPlayerCharacter::EndGrapple);
			EnhancedInputComponent->BindAction(GrappleAction, ETriggerEvent::Canceled, this, &AFPPlayerCharacter::EndGrapple);
		}
		if (SprintAction)
		{
			EnhancedInputComponent->BindAction(SprintAction, ETriggerEvent::Started, this, &AFPPlayerCharacter::StartSprint);
			EnhancedInputComponent->BindAction(SprintAction, ETriggerEvent::Completed, this, &AFPPlayerCharacter::StopSprint);
			EnhancedInputComponent->BindAction(SprintAction, ETriggerEvent::Canceled, this, &AFPPlayerCharacter::StopSprint);
		}
		if (SlideAction)
		{
			EnhancedInputComponent->BindAction(SlideAction, ETriggerEvent::Started, this, &AFPPlayerCharacter::StartSlide);
		}
		// Jump
		if (JumpAction)
		{
			EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &AFPPlayerCharacter::StartJump);
			EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &AFPPlayerCharacter::EndJump);
			EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Canceled, this, &AFPPlayerCharacter::CancelJump);
		}

		// Move (WASD)
		if (MoveAction)
		{
			EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AFPPlayerCharacter::Move);
		}

		// Look (Mouse)
		if (LookAction)
		{
			EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AFPPlayerCharacter::Look);
		}
	}
}

void AFPPlayerCharacter::StartSprint()
{
	ParkourMovementComponent->SetSprintRequested(true);
}

void AFPPlayerCharacter::StartGrapple()
{
	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		FVector ViewLocation;
		FRotator ViewRotation;
		// Use the rendered first-person viewpoint, including existing camera offsets.
		PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);
		if (ParkourMovementComponent->TryStartGrapple(ViewLocation, ViewRotation.Vector()))
		{
			ClearBufferedJump(TEXT("rejected: grapple accepted movement ownership"));
		}
	}
}

void AFPPlayerCharacter::EndGrapple()
{
	ParkourMovementComponent->StopGrapple();
}

void AFPPlayerCharacter::StopSprint()
{
	ParkourMovementComponent->SetSprintRequested(false);
}

void AFPPlayerCharacter::StartSlide()
{
	ParkourMovementComponent->RequestSlide();
}

void AFPPlayerCharacter::StartJump()
{
	if (bJumpInputHeld)
	{
		DebugJumpTransition(TEXT("rejected: duplicate press while held"));
		return;
	}
	bJumpInputHeld = true;
	if (!Controller || ParkourMovementComponent->IsGrappling())
	{
		DebugJumpTransition(TEXT("rejected: no controller or active grapple"));
		return;
	}
	StopJumping();
	bJumpBuffered = true;
	bBufferedWallJumpOnly = ParkourMovementComponent->IsWallRunning();
	JumpBufferExpiresAt = GetWorld()->GetTimeSeconds() + FMath::Clamp(JumpBufferDuration, 0.0f, 0.5f);
	LastJumpRejection.Reset();
	DebugJumpTransition(TEXT("accepted: jump press buffered"));
	CheckJumpInput(0.0f);
}

void AFPPlayerCharacter::CheckJumpInput(float DeltaTime)
{
	if (!bJumpBuffered)
	{
		Super::CheckJumpInput(DeltaTime);
		return;
	}
	if (!Controller || ParkourMovementComponent->IsGrappling()
		|| (!ParkourMovementComponent->IsMovingOnGround() && !ParkourMovementComponent->IsFalling()
			&& !ParkourMovementComponent->IsWallRunning()))
	{
		ClearBufferedJump(TEXT("rejected: movement ownership changed"));
		return;
	}
	if (GetWorld()->GetTimeSeconds() > JumpBufferExpiresAt)
	{
		ClearBufferedJump(TEXT("expired: jump buffer"));
		return;
	}
	const TCHAR* Rejection = TEXT("rejected: waiting for landing or valid wall attachment");
	if (ParkourMovementComponent->IsWallRunning())
	{
		if (ParkourMovementComponent->TryWallJump())
		{
			ClearBufferedJump(TEXT("consumed: wall jump"));
			return;
		}
		Rejection = TEXT("rejected: wall jump contact, clearance or surface lock; buffer retained");
	}
	else if (ParkourMovementComponent->IsMovingOnGround() && !bBufferedWallJumpOnly)
	{
		// Clear pending slide input and use native standing clearance before the native jump.
		if (ParkourMovementComponent->PrepareSlideJump())
		{
			Jump();
			Super::CheckJumpInput(DeltaTime);
			if (bWasJumping)
			{
				ClearBufferedJump(TEXT("consumed: ground/slide jump"));
				// A released tap may still consume its buffer, without latching jump hold.
				if (!bJumpInputHeld)
				{
					StopJumping();
				}
				return;
			}
			StopJumping();
		}
		Rejection = TEXT("rejected: ground jump clearance or native jump eligibility; buffer retained");
	}
	else if (ParkourMovementComponent->IsMovingOnGround() && bBufferedWallJumpOnly)
	{
		ClearBufferedJump(TEXT("rejected: wall-jump press cannot become a landing jump"));
		return;
	}
	if (LastJumpRejection != Rejection)
	{
		DebugJumpTransition(Rejection);
		LastJumpRejection = Rejection;
	}
}

void AFPPlayerCharacter::ClearBufferedJump(const TCHAR* Reason)
{
	if (bJumpBuffered)
	{
		DebugJumpTransition(Reason);
	}
	bJumpBuffered = false;
	bBufferedWallJumpOnly = false;
	JumpBufferExpiresAt = 0.0;
	LastJumpRejection.Reset();
}

void AFPPlayerCharacter::DebugJumpTransition(const TCHAR* Event) const
{
	if (bShowTransitionDebug || CVarParkourTransitionDebug.GetValueOnGameThread() != 0)
	{
		UE_LOG(LogParkourTransitions, Log, TEXT("%s: %s | mode=%s | velocity=%s"),
			*GetName(), Event, *ParkourMovementComponent->GetMovementName(), *ParkourMovementComponent->Velocity.ToCompactString());
	}
}

void AFPPlayerCharacter::CancelJump()
{
	EndJump();
	ClearBufferedJump(TEXT("rejected: input canceled"));
}

void AFPPlayerCharacter::EndJump()
{
	bJumpInputHeld = false;
	StopJumping();
}

void AFPPlayerCharacter::Move(const FInputActionValue& Value)
{
	const FVector2D MovementVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// Get camera-relative yaw orientation
		const FRotator ControlRotation = Controller->GetControlRotation();
		const FRotator YawRotation(0.0f, ControlRotation.Yaw, 0.0f);

		// Determine forward and right directions relative to camera/controller yaw
		const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
		const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		// Apply movement input (Y: Forward/Backward, X: Right/Left)
		AddMovementInput(ForwardDirection, MovementVector.Y);
		AddMovementInput(RightDirection, MovementVector.X);
	}
}

void AFPPlayerCharacter::Look(const FInputActionValue& Value)
{
	const FVector2D LookAxisVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// Apply yaw and pitch mouse input
		AddControllerYawInput(LookAxisVector.X);
		AddControllerPitchInput(LookAxisVector.Y);
	}
}
