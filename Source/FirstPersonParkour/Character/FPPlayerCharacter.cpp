// Copyright Epic Games, Inc. All Rights Reserved.

#include "Character/FPPlayerCharacter.h"
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
#include "UObject/ConstructorHelpers.h"

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

void AFPPlayerCharacter::Landed(const FHitResult& Hit)
{
	// Landed is called before native falling velocity is converted to ground motion.
	// Sample it here; feedback never writes to gameplay velocity or transforms.
	if (bWallJumpLandingPending && bEnableWallJumpCameraEffects && bEnableWallJumpLandingFeedback)
	{
		const float ImpactSpeed = FMath::Max(0.0f, -FVector::DotProduct(GetVelocity(), Hit.ImpactNormal));
		const float MinSpeed = FMath::Max(0.0f, WallJumpLandingMinSpeed);
		const float FullSpeed = FMath::Max(MinSpeed + 1.0f, WallJumpLandingFullEffectSpeed);
		const float Strength = FMath::Clamp((ImpactSpeed - MinSpeed) / (FullSpeed - MinSpeed), 0.0f, 1.0f);
		if (Strength > 0.0f)
		{
			WallJumpLandingPulse.Trigger(GetWorld()->GetTimeSeconds(),
				-FVector::UpVector * FMath::Clamp(WallJumpLandingIntensity, 0.0f, 2.0f) * Strength,
				0.0f, FMath::Clamp(WallJumpLandingRecoveryDuration, 0.1f, 0.6f));
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
	const float Rate = bIsCrouched ? SlideCameraTransitionSpeed : SlideCameraRestoreSpeed;
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
	// Share the existing camera collision solution with all cosmetic displacement.
	const FVector VisualOffset = SmoothedWallRunOffset + FVector::UpVector * (ContactMotion - EntryDip)
		+ JumpOffset + LandingOffset;

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
	OutResult.FOV = FMath::Clamp(OutResult.FOV + SmoothedWallRunFOV + JumpFOV, 5.0f, 170.0f);
	OutResult.FirstPersonFOV = FMath::Clamp(OutResult.FirstPersonFOV + SmoothedWallRunFOV + JumpFOV, 5.0f, 170.0f);
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
	bWallJumpLandingPending = false;
	// Possession changes must not leave sprint latched on the previous pawn.
	StopSprint();
	EndGrapple();
	ParkourMovementComponent->StopSlide();
	EndJump();
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
	EndGrapple();
	StopSprint();
	EndJump();
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
			EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Canceled, this, &AFPPlayerCharacter::EndJump);
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
		ParkourMovementComponent->TryStartGrapple(ViewLocation, ViewRotation.Vector());
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
		return;
	}
	bJumpInputHeld = true;
	if (ParkourMovementComponent->IsWallRunning())
	{
		// Consume this press even if fresh contact/clearance rejects the jump.
		// Never buffer it into a normal jump or a later wall attachment.
		StopJumping();
		ParkourMovementComponent->TryWallJump();
		return;
	}
	// Native UnCrouch performs the clearance query; blocked jumps remain low.
	if (ParkourMovementComponent->PrepareSlideJump())
	{
		Jump();
	}
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
