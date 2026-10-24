// Copyright Epic Games, Inc. All Rights Reserved.

#include "Movement/FPParkourMovementComponent.h"
#include "Character/FPPlayerCharacter.h"
#include "Camera/CameraComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/Character.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarParkourMovementDebug(
	TEXT("parkour.DebugMovement"), 0, TEXT("Display local player movement: 0 = off, 1 = on."));

UFPParkourMovementComponent::UFPParkourMovementComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	MaxAcceleration = 4000.0f;
	GroundFriction = 5.0f;
	// Use the current physics mode's friction for braking: ground friction when
	// walking, zero lateral friction in air. Separate braking friction also acts
	// during falling and would bleed momentum whenever movement input is released.
	bUseSeparateBrakingFriction = false;
	BrakingFriction = 4.0f;
	BrakingFrictionFactor = 1.0f;
	BrakingDecelerationWalking = 1800.0f;
	AirControl = 0.25f;
	AirControlBoostMultiplier = 1.0f;
	FallingLateralFriction = 0.0f;
	BrakingDecelerationFalling = 0.0f;
	GetNavAgentPropertiesRef().bCanCrouch = true;
	bCanWalkOffLedgesWhenCrouching = true;
	// Native crouch clamps this to the capsule radius; editable through Character Movement.
	SetCrouchedHalfHeight(48.0f);
}

void UFPParkourMovementComponent::BeginPlay()
{
	Super::BeginPlay();
	CurrentGroundSpeed = FMath::Max(0.0f, WalkSpeed);
}

void UFPParkourMovementComponent::Crouch(bool bClientSimulation)
{
	AFPPlayerCharacter* Player = Cast<AFPPlayerCharacter>(CharacterOwner);
	const FVector EyeLocation = Player ? Player->GetFirstPersonCameraComponent()->GetComponentLocation() : FVector::ZeroVector;
	Super::Crouch(bClientSimulation);
	if (Player)
	{
		Player->RestoreCameraAfterCapsuleResize(EyeLocation);
	}
}

void UFPParkourMovementComponent::UnCrouch(bool bClientSimulation)
{
	AFPPlayerCharacter* Player = Cast<AFPPlayerCharacter>(CharacterOwner);
	const FVector EyeLocation = Player ? Player->GetFirstPersonCameraComponent()->GetComponentLocation() : FVector::ZeroVector;
	Super::UnCrouch(bClientSimulation);
	if (Player)
	{
		Player->RestoreCameraAfterCapsuleResize(EyeLocation);
	}
}

void UFPParkourMovementComponent::StopSlide()
{
	if (bSliding)
	{
		// Resume the speed-cap transition from actual slide momentum, including downhill speed.
		CurrentGroundSpeed = FMath::Max(WalkSpeed, Velocity.Size2D());
	}
	bSlideRequested = false;
	bSliding = false;
	// Native state updates retry uncrouching until the standing capsule fits.
	bWantsToCrouch = false;
}

bool UFPParkourMovementComponent::PrepareSlideJump()
{
	StopSlide();
	if (IsCrouching())
	{
		UnCrouch();
	}
	return !IsCrouching();
}

void UFPParkourMovementComponent::UpdateCharacterStateBeforeMovement(float DeltaSeconds)
{
	if (bSliding && (!IsMovingOnGround() || !IsCrouching() || !CharacterOwner->Controller
		|| Velocity.Size2D() < FMath::Max(0.0f, SlideEndSpeed)))
	{
		StopSlide();
	}
	if (bSlideRequested)
	{
		bSlideRequested = false;
		if (bSliding)
		{
			StopSlide();
		}
		else if (IsSprinting() && CurrentFloor.IsWalkableFloor()
			&& Velocity.Size2D() >= FMath::Max(SlideMinSpeed, SlideEndSpeed))
		{
			bWantsToCrouch = true;
			Crouch();
			bSliding = IsCrouching();
			if (!bSliding)
			{
				bWantsToCrouch = false;
			}
		}
	}
	Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);
}

void UFPParkourMovementComponent::UpdateCharacterStateAfterMovement(float DeltaSeconds)
{
	if (bSliding && (!IsMovingOnGround() || !IsCrouching()
		|| Velocity.Size2D() < FMath::Max(0.0f, SlideEndSpeed)))
	{
		StopSlide();
	}
	Super::UpdateCharacterStateAfterMovement(DeltaSeconds);
}

void UFPParkourMovementComponent::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration)
{
	if (!bSliding || !IsMovingOnGround())
	{
		if (IsMovingOnGround() && !IsCrouching())
		{
			const float TargetSpeed = bSprintRequested ? FMath::Max(WalkSpeed, SprintSpeed) : WalkSpeed;
			const float Duration = TargetSpeed > CurrentGroundSpeed ? SprintAccelerationTime : SprintDecelerationTime;
			const float Rate = FMath::Max(1.0f, SprintSpeed - WalkSpeed) / FMath::Max(0.01f, Duration);
			CurrentGroundSpeed = FMath::FInterpConstantTo(CurrentGroundSpeed, FMath::Max(0.0f, TargetSpeed), DeltaTime, Rate);

			// Retain sprint braking while Shift is held, and blend back by actual
			// momentum when released (including simultaneous Shift/WASD release).
			// This blends physics settings only; native CalcVelocity owns velocity.
			const float SprintRange = FMath::Max(1.0f, SprintSpeed - WalkSpeed);
			const float MomentumAlpha = FMath::Clamp((Velocity.Size2D() - WalkSpeed) / SprintRange, 0.0f, 1.0f);
			const float SprintAlpha = bSprintRequested ? 1.0f : MomentumAlpha;
			Friction = FMath::Max(0.0f, FMath::Lerp(WalkingGroundFriction, SprintingGroundFriction, SprintAlpha));
			BrakingDeceleration = FMath::Max(0.0f,
				FMath::Lerp(WalkingBrakingDeceleration, SprintingBrakingDeceleration, SprintAlpha));
		}
		Super::CalcVelocity(DeltaTime, Friction, bFluid, BrakingDeceleration);
		return;
	}
	if (!HasValidData() || DeltaTime < MIN_TICK_TIME)
	{
		return;
	}

	FVector HorizontalVelocity(Velocity.X, Velocity.Y, 0.0f);
	const FVector FloorGravity = FVector::VectorPlaneProject(
		FVector(0.0f, 0.0f, GetGravityZ()), CurrentFloor.HitResult.ImpactNormal)
		* FMath::Max(0.0f, SlideSlopeGravityScale);
	const FVector SlopeAcceleration(FloorGravity.X, FloorGravity.Y, 0.0f);
	const float Drag = FMath::Max(0.0f, SlideFriction);
	// Exact constant-force/linear-drag integration for each native walking substep.
	const float Decay = FMath::Exp(-Drag * DeltaTime);
	HorizontalVelocity = HorizontalVelocity * Decay + SlopeAcceleration
		* (Drag > UE_SMALL_NUMBER ? (1.0f - Decay) / Drag : DeltaTime);

	const float Speed = HorizontalVelocity.Size2D();
	if (Speed > UE_SMALL_NUMBER && !Acceleration.IsNearlyZero())
	{
		const float InputStrength = FMath::Clamp(Acceleration.Size2D() / FMath::Max(GetMaxAcceleration(), 1.0f), 0.0f, 1.0f);
		const float Angle = FMath::FindDeltaAngleRadians(FMath::Atan2(HorizontalVelocity.Y, HorizontalVelocity.X),
			FMath::Atan2(Acceleration.Y, Acceleration.X));
		const float MaxTurn = FMath::DegreesToRadians(FMath::Max(0.0f, SlideSteeringRate)) * InputStrength * DeltaTime;
		HorizontalVelocity = HorizontalVelocity.RotateAngleAxis(
			FMath::RadiansToDegrees(FMath::Clamp(Angle, -MaxTurn, MaxTurn)), FVector::UpVector);
	}
	Velocity.X = HorizontalVelocity.X;
	Velocity.Y = HorizontalVelocity.Y;
	// PhysWalking continues to handle swept movement, floors, steps, walls and ledges.
}

float UFPParkourMovementComponent::GetMaxAcceleration() const
{
	if (IsMovingOnGround() && !IsCrouching() && !bSliding)
	{
		// Queried by native input scaling before CalcVelocity, not just by physics.
		return FMath::Max(0.0f, bSprintRequested ? SprintingAcceleration : WalkingAcceleration);
	}
	// Preserve existing air control, slide steering input and other movement modes.
	return Super::GetMaxAcceleration();
}

float UFPParkourMovementComponent::GetMaxSpeed() const
{
	if (IsFalling())
	{
		// Keep a fixed takeoff cap, rather than continually raising it with steering velocity.
		return FMath::Max(FallingSpeedLimit, WalkSpeed);
	}
	if (IsMovingOnGround() && !IsCrouching())
	{
		return CurrentGroundSpeed;
	}
	return Super::GetMaxSpeed();
}

void UFPParkourMovementComponent::OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode)
{
	if (IsFalling() && PreviousMovementMode != MOVE_Falling)
	{
		FallingSpeedLimit = FMath::Max(Velocity.Size2D(), CurrentGroundSpeed);
	}
	else if (!IsFalling())
	{
		FallingSpeedLimit = 0.0f;
	}
	if (IsMovingOnGround() && PreviousMovementMode == MOVE_Falling)
	{
		// Ease down from landing momentum instead of immediately reinstating the walking cap.
		CurrentGroundSpeed = FMath::Max(WalkSpeed, Velocity.Size2D());
	}
	if (!IsMovingOnGround())
	{
		StopSlide();
	}
	Super::OnMovementModeChanged(PreviousMovementMode, PreviousCustomMode);
}

void UFPParkourMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if ((bShowMovementDebug || CVarParkourMovementDebug.GetValueOnGameThread() != 0)
		&& GEngine && CharacterOwner && CharacterOwner->IsLocallyControlled())
	{
		const FString State = IsFalling() ? TEXT("Falling") : IsMovingOnGround()
			? (bSliding ? TEXT("Sliding") : IsCrouching() ? TEXT("Crouched (clearance)")
				: IsSprinting() ? TEXT("Sprinting") : TEXT("Walking")) : GetMovementName();
		GEngine->AddOnScreenDebugMessage(static_cast<uint64>(GetUniqueID()), 0.0f, FColor::Green,
			FString::Printf(TEXT("Speed XY: %.1f cm/s | Velocity: %s | %s"), Velocity.Size2D(), *Velocity.ToCompactString(), *State));
	}
}

void UFPParkourMovementComponent::PhysCustom(float deltaTime, int32 Iterations)
{
	Super::PhysCustom(deltaTime, Iterations);

	// Reserved for future custom parkour modes; sprinting uses native walking physics.
}
