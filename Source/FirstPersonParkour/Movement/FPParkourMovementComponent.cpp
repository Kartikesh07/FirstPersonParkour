// Copyright Epic Games, Inc. All Rights Reserved.

#include "Movement/FPParkourMovementComponent.h"
#include "Character/FPPlayerCharacter.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
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
	const bool bWasWallRunning = PreviousMovementMode == MOVE_Custom && PreviousCustomMode == WallRunMode;
	if (IsWallRunning())
	{
		bWallRunUsed = true;
		WallRunElapsed = 0.0f;
	}
	if (IsMovingOnGround())
	{
		bWallRunUsed = false;
		WallRunElapsed = 0.0f;
	}
	if (IsFalling() && PreviousMovementMode != MOVE_Falling)
	{
		FallingSpeedLimit = FMath::Max(Velocity.Size2D(), CurrentGroundSpeed);
	}
	else if (!IsFalling() && !IsWallRunning())
	{
		FallingSpeedLimit = 0.0f;
	}
	if (IsMovingOnGround() && (PreviousMovementMode == MOVE_Falling || bWasWallRunning))
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
		const FString State = IsWallRunning() ? TEXT("Wall Running") : IsFalling() ? TEXT("Falling") : IsMovingOnGround()
			? (bSliding ? TEXT("Sliding") : IsCrouching() ? TEXT("Crouched (clearance)")
				: IsSprinting() ? TEXT("Sprinting") : TEXT("Walking")) : GetMovementName();
		GEngine->AddOnScreenDebugMessage(static_cast<uint64>(GetUniqueID()), 0.0f, FColor::Green,
			FString::Printf(TEXT("Speed XY: %.1f cm/s | Velocity: %s | %s"), Velocity.Size2D(), *Velocity.ToCompactString(), *State));
	}
}

void UFPParkourMovementComponent::PhysCustom(float deltaTime, int32 Iterations)
{
	if (IsWallRunning())
	{
		PhysWallRun(deltaTime, Iterations);
		return;
	}
	Super::PhysCustom(deltaTime, Iterations);
}

bool UFPParkourMovementComponent::IsSuitableWall(const FHitResult& Hit) const
{
	const UPrimitiveComponent* Surface = Hit.GetComponent();
	return Hit.IsValidBlockingHit() && Surface && !IsWalkable(Hit)
		&& FMath::Abs(Hit.ImpactNormal.Z) <= FMath::Clamp(WallRunMaxNormalZ, 0.0f, 0.3f)
		// Moving/physics surfaces require base-relative movement, which this mode doesn't implement.
		&& Surface->GetMobility() != EComponentMobility::Movable && !Surface->IsSimulatingPhysics()
		&& !Cast<APawn>(Hit.GetActor());
}

bool UFPParkourMovementComponent::TraceWall(const FVector& Direction, FHitResult& Hit) const
{
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WallRun), false, CharacterOwner);
	FCollisionResponseParams Response;
	InitCollisionParams(Params, Response);
	const FVector Start = UpdatedComponent->GetComponentLocation();
	const float Reach = CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius()
		+ FMath::Max(0.0f, WallDetectionDistance);
	return GetWorld()->LineTraceSingleByChannel(Hit, Start, Start + Direction * Reach,
		UpdatedComponent->GetCollisionObjectType(), Params, Response) && IsSuitableWall(Hit);
}

bool UFPParkourMovementComponent::TryStartWallRun()
{
	if (!HasValidData() || !IsFalling() || bWallRunUsed || IsCrouching() || bWantsToCrouch
		|| !CharacterOwner->Controller || HasAnimRootMotion() || CurrentRootMotion.HasOverrideVelocity()
		|| Velocity.Size2D() < FMath::Max(1.0f, WallRunMinSpeed)
		|| Velocity.Z < -FMath::Max(0.0f, WallRunMaxEntryFallSpeed))
	{
		return false;
	}

	const FVector Travel = Velocity.GetSafeNormal2D();
	// Enter while traveling forward, rather than when backing or strafing into a wall.
	if (FVector::DotProduct(Travel, CharacterOwner->GetActorForwardVector()) <= 0.25f)
	{
		return false;
	}
	// Left/right are relative to travel, so looking around never breaks an active run.
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Travel);
	FHitResult BestHit;
	float BestDistance = UE_BIG_NUMBER;
	for (const float Side : { -1.0f, 1.0f })
	{
		FHitResult Hit;
		if (TraceWall(Right * Side, Hit))
		{
			const FVector Tangent = FVector::CrossProduct(FVector::UpVector, Hit.ImpactNormal).GetSafeNormal();
			if (FMath::Abs(FVector::DotProduct(Travel, Tangent)) >= FMath::Clamp(WallRunMinAlignment, 0.0f, 1.0f)
				&& Hit.Distance < BestDistance)
			{
				BestHit = Hit;
				BestDistance = Hit.Distance;
			}
		}
	}
	if (!BestHit.IsValidBlockingHit())
	{
		return false;
	}
	WallRunNormal = BestHit.ImpactNormal.GetSafeNormal2D();
	WallRunDirection = Travel;
	// No velocity boost, vertical reset or position snap on entry.
	SetMovementMode(MOVE_Custom, WallRunMode);
	return IsWallRunning();
}

void UFPParkourMovementComponent::PhysFalling(float deltaTime, int32 Iterations)
{
	if (deltaTime >= MIN_TICK_TIME && TryStartWallRun())
	{
		StartNewPhysics(deltaTime, Iterations);
		return;
	}
	Super::PhysFalling(deltaTime, Iterations);
}

void UFPParkourMovementComponent::EndWallRun(float RemainingTime, int32 Iterations)
{
	// Preserve velocity and let native falling consume any unused simulation time.
	SetMovementMode(MOVE_Falling);
	StartNewPhysics(RemainingTime, Iterations);
}

void UFPParkourMovementComponent::PhysWallRun(float DeltaTime, int32 Iterations)
{
	if (!HasValidData() || DeltaTime < MIN_TICK_TIME)
	{
		return;
	}

	float RemainingTime = DeltaTime;
	while (RemainingTime >= MIN_TICK_TIME && Iterations < MaxSimulationIterations && IsWallRunning())
	{
		FHitResult WallHit;
		const float DurationLeft = FMath::Max(0.01f, WallRunMaxDuration) - WallRunElapsed;
		const float CornerCos = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(WallRunMaxCornerAngle, 0.0f, 60.0f)));
		const FVector Input = Acceleration / FMath::Max(1.0f, GetMaxAcceleration());
		if (DurationLeft < MIN_TICK_TIME || !CharacterOwner->Controller || IsCrouching() || bWantsToCrouch
			|| HasAnimRootMotion() || CurrentRootMotion.HasOverrideVelocity()
			|| Velocity.Size2D() < FMath::Max(1.0f, WallRunMinSpeed)
			|| FVector::DotProduct(Input, WallRunNormal) > FMath::Clamp(WallRunDetachInputThreshold, 0.1f, 1.0f)
			|| !TraceWall(-WallRunNormal, WallHit)
			|| FVector::DotProduct(WallHit.ImpactNormal.GetSafeNormal2D(), WallRunNormal) < CornerCos)
		{
			EndWallRun(RemainingTime, Iterations);
			return;
		}

		++Iterations;
		const float TimeTick = FMath::Min(GetSimulationTimeStep(RemainingTime, Iterations), DurationLeft);
		RemainingTime -= TimeTick;
		WallRunElapsed += TimeTick;
		WallRunNormal = WallHit.ImpactNormal.GetSafeNormal2D();
		FVector Tangent = FVector::CrossProduct(FVector::UpVector, WallRunNormal);
		if (FVector::DotProduct(Tangent, WallRunDirection) < 0.0f)
		{
			Tangent *= -1.0f;
		}
		const FVector OldVelocity = Velocity;
		const FVector Travel = Velocity.GetSafeNormal2D();
		const float Angle = FMath::FindDeltaAngleRadians(FMath::Atan2(Travel.Y, Travel.X), FMath::Atan2(Tangent.Y, Tangent.X));
		const float MaxTurn = FMath::DegreesToRadians(FMath::Max(0.0f, WallRunTurnRate)) * TimeTick;
		WallRunDirection = Travel.RotateAngleAxis(FMath::RadiansToDegrees(FMath::Clamp(Angle, -MaxTurn, MaxTurn)), FVector::UpVector);
		const float OpposingInput = FMath::Clamp(-FVector::DotProduct(Input, Tangent), 0.0f, 1.0f);
		const float Speed = FMath::Max(0.0f, Velocity.Size2D() - OpposingInput * FMath::Max(0.0f, WallRunBrakingDeceleration) * TimeTick);
		Velocity.X = WallRunDirection.X * Speed;
		Velocity.Y = WallRunDirection.Y * Speed;
		Velocity = NewFallVelocity(Velocity, FVector(0.0f, 0.0f, GetGravityZ()) * FMath::Clamp(WallRunGravityScale, 0.0f, 1.0f), TimeTick);

		// Close the gap gradually using the same swept capsule movement as travel.
		const float Gap = FMath::Max(0.0f, WallHit.Distance - CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius() - 2.0f);
		const FVector Attraction = -WallRunNormal * FMath::Min(Gap, FMath::Max(0.0f, WallRunAttractionSpeed) * TimeTick);
		const FVector Delta = (OldVelocity + Velocity) * (0.5f * TimeTick) + Attraction;
		FHitResult Hit;
		SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);
		if (!HasValidData() || !IsWallRunning())
		{
			if (HasValidData())
			{
				StartNewPhysics(RemainingTime, Iterations);
			}
			return;
		}

		if (Hit.IsValidBlockingHit())
		{
			// Handle both the first sweep and its slide: either can encounter the floor.
			float UnusedTime = TimeTick * (1.0f - Hit.Time);
			for (int32 Impact = 0; Impact < 2; ++Impact)
			{
				if (Velocity.Z <= 0.0f && IsValidLandingSpot(UpdatedComponent->GetComponentLocation(), Hit))
				{
					SetMovementMode(MOVE_Falling);
					ProcessLanded(Hit, RemainingTime + UnusedTime, Iterations);
					return;
				}
				HandleImpact(Hit, TimeTick, Delta);
				if (!HasValidData() || !IsWallRunning())
				{
					if (HasValidData())
					{
						StartNewPhysics(RemainingTime + UnusedTime, Iterations);
					}
					return;
				}
				// Only collision removes velocity into a surface; entry/exit never clamp it.
				if (FVector::DotProduct(Velocity, Hit.Normal) < 0.0f)
				{
					Velocity = ComputeSlideVector(Velocity, 1.0f, Hit.Normal, Hit);
				}
				if (!IsSuitableWall(Hit) || FVector::DotProduct(Hit.ImpactNormal.GetSafeNormal2D(), WallRunNormal) < CornerCos)
				{
					EndWallRun(RemainingTime + UnusedTime, Iterations);
					return;
				}
				if (Impact == 1 || UnusedTime < MIN_TICK_TIME)
				{
					break;
				}
				const FVector SlideDelta = ComputeSlideVector(Delta, 1.0f - Hit.Time, Hit.Normal, Hit);
				SafeMoveUpdatedComponent(SlideDelta, UpdatedComponent->GetComponentQuat(), true, Hit);
				if (!HasValidData() || !IsWallRunning())
				{
					return;
				}
				UnusedTime *= 1.0f - Hit.Time;
				if (!Hit.IsValidBlockingHit())
				{
					break;
				}
			}
		}
	}
	if (IsWallRunning() && WallRunElapsed >= FMath::Max(0.01f, WallRunMaxDuration))
	{
		EndWallRun(RemainingTime, Iterations);
	}
}
