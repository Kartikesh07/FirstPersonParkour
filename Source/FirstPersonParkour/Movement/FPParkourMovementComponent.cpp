// Copyright Epic Games, Inc. All Rights Reserved.

#include "Movement/FPParkourMovementComponent.h"
#include "Character/FPPlayerCharacter.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarParkourMovementDebug(
	TEXT("parkour.DebugMovement"), 0, TEXT("Display local player movement: 0 = off, 1 = on."));

static TAutoConsoleVariable<int32> CVarParkourWallRunDebug(
	TEXT("parkour.DebugWallRun"), 0, TEXT("Wall probes, rejection and detach diagnostics: 0 = off, 1 = on."));

DEFINE_LOG_CATEGORY_STATIC(LogParkourWallRun, Log, All);

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

bool UFPParkourMovementComponent::TryWallJump()
{
	if (!HasValidData() || !IsWallRunning() || !CharacterOwner->Controller
		|| IsCrouching() || HasAnimRootMotion() || CurrentRootMotion.HasOverrideVelocity()
		|| Velocity.ContainsNaN() || WallRunNormal.ContainsNaN() || !WallRunNormal.IsNormalized())
	{
		return false;
	}

	// Grace-time contact is sufficient to keep running, but never to create an impulse.
	FHitResult WallHit;
	if (!TraceWall(-WallRunNormal, WallHit) || IsWallJumpSurfaceBlocked(WallHit)
		|| FVector::DotProduct(WallHit.ImpactNormal.GetSafeNormal2D(), WallRunNormal)
			< FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(WallRunMaxCornerAngle, 0.0f, 60.0f))))
	{
		DebugWallRun(TEXT("wall jump rejected: no fresh active-wall contact"));
		return false;
	}

	const FVector Normal = WallHit.ImpactNormal.GetSafeNormal2D();
	const FVector HorizontalVelocity(Velocity.X, Velocity.Y, 0.0f);
	const FVector TangentialVelocity = FVector::VectorPlaneProject(HorizontalVelocity, Normal);
	FVector JumpVelocity = TangentialVelocity * FMath::Clamp(WallJumpMomentumRetention, 0.0f, 1.0f)
		+ Normal * (FMath::Max(0.0f, FVector::DotProduct(HorizontalVelocity, Normal))
			+ FMath::Max(0.0f, WallJumpOutwardImpulse))
		+ FVector::UpVector * (Velocity.Z + FMath::Max(0.0f, WallJumpUpwardImpulse));
	JumpVelocity = ConstrainDirectionToPlane(JumpVelocity);
	if (JumpVelocity.ContainsNaN() || FVector::DotProduct(JumpVelocity, Normal) <= UE_SMALL_NUMBER
		|| JumpVelocity.Z <= UE_SMALL_NUMBER)
	{
		return false;
	}

	// Check actual capsule clearance, including initial penetration and low ceilings.
	// Do not teleport or pre-move: native PhysFalling sweeps the entire subsequent path.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WallJump), false, CharacterOwner);
	FCollisionResponseParams Response;
	InitCollisionParams(Params, Response);
	const FVector Center = UpdatedComponent->GetComponentLocation();
	FHitResult ClearanceHit;
	if (GetWorld()->SweepSingleByChannel(ClearanceHit, Center, Center + JumpVelocity.GetSafeNormal() * 2.0f,
		UpdatedComponent->GetComponentQuat(), UpdatedComponent->GetCollisionObjectType(),
		CharacterOwner->GetCapsuleComponent()->GetCollisionShape(), Params, Response))
	{
		DebugWallRun(TEXT("wall jump rejected: capsule takeoff obstructed"));
		return false;
	}

	FWallJumpRestriction& Restriction = WallJumpRestrictions.AddDefaulted_GetRef();
	Restriction.Surface = WallHit.GetComponent();
	Restriction.Normal = Normal;
	Restriction.Point = WallHit.ImpactPoint;
	Restriction.UnlockTime = GetWorld()->GetTimeSeconds() + FMath::Max(0.01f, WallJumpSameWallCooldown);
	CharacterOwner->StopJumping();
	Velocity = JumpVelocity;
	SetMovementMode(MOVE_Falling);
	// A different wall can attach immediately if eligible. The jumped surface has its
	// own stronger restriction, and the existing airborne duration is never reset here.
	WallRunReattachTime = GetWorld()->GetTimeSeconds();
	DebugWallRun(TEXT("wall jump: native falling; same surface locked"));
	return true;
}

bool UFPParkourMovementComponent::IsWallJumpSurfaceBlocked(const FHitResult& Hit) const
{
	const FVector Normal = Hit.ImpactNormal.GetSafeNormal2D();
	const float SameSurfaceCos = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(WallJumpSameSurfaceAngle, 0.0f, 60.0f)));
	for (const FWallJumpRestriction& Restriction : WallJumpRestrictions)
	{
		if (FVector::DotProduct(Normal, Restriction.Normal) >= SameSurfaceCos
			&& ((Restriction.Surface.IsValid() && Restriction.Surface.Get() == Hit.GetComponent())
				|| FMath::Abs(FVector::DotProduct(Hit.ImpactPoint - Restriction.Point, Restriction.Normal))
					<= FMath::Max(0.0f, WallJumpSurfacePlaneTolerance)))
		{
			return true;
		}
	}
	return false;
}

void UFPParkourMovementComponent::UpdateWallJumpRestrictions()
{
	const FVector Center = UpdatedComponent->GetComponentLocation();
	const UCapsuleComponent* Capsule = CharacterOwner->GetCapsuleComponent();
	const float Separation = FMath::Max(1.0f, WallJumpMinSeparation);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WallJumpSeparation), false, CharacterOwner);
	FCollisionResponseParams Response;
	InitCollisionParams(Params, Response);
	for (int32 Index = WallJumpRestrictions.Num() - 1; Index >= 0; --Index)
	{
		FWallJumpRestriction& Restriction = WallJumpRestrictions[Index];
		if (!Restriction.bHasSeparated
			&& FVector::DotProduct(Center - Restriction.Point, Restriction.Normal)
				>= Capsule->GetScaledCapsuleRadius() + Separation)
		{
			// A plane distance alone can falsely clear a curved wall or a corner.
			// Require a full capsule-width corridor back toward the departed surface.
			FHitResult Hit;
			Restriction.bHasSeparated = !GetWorld()->SweepSingleByChannel(Hit, Center,
				Center - Restriction.Normal * Separation, UpdatedComponent->GetComponentQuat(),
				UpdatedComponent->GetCollisionObjectType(), Capsule->GetCollisionShape(), Params, Response);
		}
		if (Restriction.bHasSeparated && GetWorld()->GetTimeSeconds() >= Restriction.UnlockTime)
		{
			WallJumpRestrictions.RemoveAtSwap(Index);
		}
	}
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
		WallRunContactLostTime = 0.0f;
	}
	if (bWasWallRunning && !IsWallRunning())
	{
		WallRunReattachTime = GetWorld()->GetTimeSeconds() + FMath::Max(0.01f, WallRunReattachCooldown);
	}
	if (IsMovingOnGround())
	{
		WallRunElapsed = 0.0f;
		WallRunReattachTime = 0.0;
		WallJumpRestrictions.Reset();
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
	return GetWallRejectionReason(Hit) == nullptr;
}

const TCHAR* UFPParkourMovementComponent::GetWallRejectionReason(const FHitResult& Hit) const
{
	const UPrimitiveComponent* Surface = Hit.GetComponent();
	if (!Hit.IsValidBlockingHit() || !Surface)
	{
		return Hit.bStartPenetrating ? TEXT("probe starts penetrating") : TEXT("no blocking wall");
	}
	if (Hit.ImpactNormal.ContainsNaN() || !Hit.ImpactNormal.IsNormalized()
		|| Hit.ImpactNormal.GetSafeNormal2D().IsNearlyZero())
	{
		return TEXT("invalid wall normal");
	}
	if (IsWalkable(Hit) || FMath::Abs(Hit.ImpactNormal.Z) > FMath::Clamp(WallRunMaxNormalZ, 0.0f, 0.3f))
	{
		return TEXT("floor/ceiling/unsuitable wall angle");
	}
	// Mobility describes whether a component CAN move, not whether it IS moving.
	// Ordinary stationary BlockAllDynamic test meshes must remain eligible.
	if (Surface->IsSimulatingPhysics() || Cast<APawn>(Hit.GetActor()))
	{
		return TEXT("physics body or pawn");
	}
	if (!Surface->GetComponentVelocity().IsNearlyZero(1.0f))
	{
		return TEXT("moving surface requires base-relative physics");
	}
	return nullptr;
}

bool UFPParkourMovementComponent::IsWallRunDebugEnabled() const
{
	return bShowWallRunDebug || CVarParkourWallRunDebug.GetValueOnGameThread() != 0;
}

void UFPParkourMovementComponent::DebugWallRun(const TCHAR* Message)
{
	if (!IsWallRunDebugEnabled())
	{
		return;
	}
	const double Now = GetWorld()->GetTimeSeconds();
	if (LastWallRunDebugMessage == Message && Now < NextWallRunDebugTime)
	{
		return;
	}
	LastWallRunDebugMessage = Message;
	NextWallRunDebugTime = Now + 0.5;
	const FString Status = FString::Printf(TEXT("WallRun: %s | Mode %d/%d | XY %.0f/%.0f Z %.0f | budget %.2f/%.2f"),
		Message, static_cast<int32>(MovementMode), CustomMovementMode, Velocity.Size2D(), WallRunMinSpeed,
		Velocity.Z, WallRunElapsed, WallRunMaxDuration);
	UE_LOG(LogParkourWallRun, Log, TEXT("%s: %s"), *GetNameSafe(CharacterOwner), *Status);
	if (GEngine && CharacterOwner && CharacterOwner->IsLocallyControlled())
	{
		GEngine->AddOnScreenDebugMessage((1ull << 32) | GetUniqueID(), 0.6f, FColor::Cyan, Status);
	}
}

bool UFPParkourMovementComponent::TraceWall(const FVector& Direction, FHitResult& Hit) const
{
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WallRun), false, CharacterOwner);
	FCollisionResponseParams Response;
	InitCollisionParams(Params, Response);
	const FVector Center = UpdatedComponent->GetComponentLocation();
	const UCapsuleComponent* Capsule = CharacterOwner->GetCapsuleComponent();
	const float Reach = CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius()
		+ FMath::Max(0.0f, WallDetectionDistance);
	// Both probes stay within the capsule's cylindrical section. The lower probe
	// keeps a jump alongside a wall detectable when its top passes below capsule center.
	const float LowerOffset = FMath::Min(Capsule->GetScaledCapsuleHalfHeight() * 0.5f,
		FMath::Max(0.0f, Capsule->GetScaledCapsuleHalfHeight() - Capsule->GetScaledCapsuleRadius()));
	const bool bDebug = IsWallRunDebugEnabled();
	for (const float Offset : { 0.0f, LowerOffset })
	{
		const FVector Start = Center - FVector::UpVector * Offset;
		const FVector End = Start + Direction * Reach;
		FHitResult ProbeHit;
		GetWorld()->LineTraceSingleByChannel(ProbeHit, Start, End,
			UpdatedComponent->GetCollisionObjectType(), Params, Response);
		const TCHAR* Rejection = GetWallRejectionReason(ProbeHit);
		if (bDebug)
		{
			const FColor Color = !Rejection ? FColor::Green : ProbeHit.bBlockingHit ? FColor::Yellow : FColor::Red;
			DrawDebugLine(GetWorld(), Start, End, Color, false, 0.05f, 0, 1.0f);
			if (ProbeHit.bBlockingHit)
			{
				DrawDebugPoint(GetWorld(), ProbeHit.ImpactPoint, 6.0f, Color, false, 0.05f);
				DrawDebugLine(GetWorld(), ProbeHit.ImpactPoint, ProbeHit.ImpactPoint + ProbeHit.ImpactNormal * 25.0f,
					FColor::Cyan, false, 0.05f);
				DrawDebugString(GetWorld(), ProbeHit.ImpactPoint, FString::Printf(TEXT("%s: %s (channel %d, normal Z %.2f)"),
					*GetNameSafe(ProbeHit.GetComponent()), Rejection ? Rejection : TEXT("eligible"),
					static_cast<int32>(UpdatedComponent->GetCollisionObjectType()), ProbeHit.ImpactNormal.Z),
					nullptr, Color, 0.05f);
			}
		}
		if (!Rejection)
		{
			Hit = ProbeHit;
			return true;
		}
		// Retain a rejected blocking hit so callers can diagnose invalid surfaces.
		if (!Hit.bBlockingHit || ProbeHit.bBlockingHit)
		{
			Hit = ProbeHit;
		}
	}
	return false;
}

bool UFPParkourMovementComponent::TryStartWallRun()
{
	if (!HasValidData() || !IsFalling())
	{
		return false;
	}
	const auto Reject = [this](const TCHAR* Reason)
	{
		DebugWallRun(Reason);
		return false;
	};
	if (WallRunElapsed >= FMath::Max(0.01f, WallRunMaxDuration) - MIN_TICK_TIME)
	{
		return Reject(TEXT("reject: airborne time budget exhausted; land to reset"));
	}
	if (GetWorld()->GetTimeSeconds() < WallRunReattachTime)
	{
		return Reject(TEXT("reject: reattachment cooldown"));
	}
	if (IsCrouching() || bWantsToCrouch || !CharacterOwner->Controller)
	{
		return Reject(TEXT("reject: crouched or no controller"));
	}
	if (HasAnimRootMotion() || CurrentRootMotion.HasOverrideVelocity())
	{
		return Reject(TEXT("reject: root motion owns velocity"));
	}
	if (Velocity.Size2D() < FMath::Max(1.0f, WallRunMinSpeed))
	{
		return Reject(TEXT("reject: below minimum horizontal speed"));
	}
	if (Velocity.Z < -FMath::Max(0.0f, WallRunMaxEntryFallSpeed))
	{
		return Reject(TEXT("reject: descending too fast"));
	}

	const FVector Travel = Velocity.GetSafeNormal2D();
	// Enter while traveling forward, rather than when backing or strafing into a wall.
	if (FVector::DotProduct(Travel, CharacterOwner->GetActorForwardVector()) <= 0.25f)
	{
		return Reject(TEXT("reject: not traveling forward"));
	}
	// Left/right are relative to travel, so looking around never breaks an active run.
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Travel);
	FHitResult BestHit;
	float BestDistance = UE_BIG_NUMBER;
	const TCHAR* Rejection = TEXT("reject: no blocking side wall within reach");
	for (const float Side : { -1.0f, 1.0f })
	{
		FHitResult Hit;
		if (TraceWall(Right * Side, Hit))
		{
			if (IsWallJumpSurfaceBlocked(Hit))
			{
				Rejection = TEXT("reject: jumped surface needs cooldown and capsule separation");
				continue;
			}
			const FVector Tangent = FVector::CrossProduct(FVector::UpVector, Hit.ImpactNormal).GetSafeNormal();
			if (FMath::Abs(FVector::DotProduct(Travel, Tangent)) >= FMath::Clamp(WallRunMinAlignment, 0.0f, 1.0f)
				&& Hit.Distance < BestDistance)
			{
				BestHit = Hit;
				BestDistance = Hit.Distance;
			}
			else
			{
				Rejection = TEXT("reject: travel/wall alignment below minimum");
			}
		}
		else if (Hit.bBlockingHit)
		{
			Rejection = GetWallRejectionReason(Hit);
		}
	}
	if (!BestHit.IsValidBlockingHit())
	{
		return Reject(Rejection);
	}
	WallRunNormal = BestHit.ImpactNormal.GetSafeNormal2D();
	WallRunDirection = Travel;
	// No velocity boost, vertical reset or position snap on entry.
	SetMovementMode(MOVE_Custom, WallRunMode);
	DebugWallRun(IsWallRunning() ? TEXT("entered custom wall-run mode") : TEXT("reject: movement mode transition failed"));
	return IsWallRunning();
}

void UFPParkourMovementComponent::PhysFalling(float deltaTime, int32 Iterations)
{
	if (HasValidData())
	{
		UpdateWallJumpRestrictions();
	}
	if (deltaTime >= MIN_TICK_TIME && TryStartWallRun())
	{
		StartNewPhysics(deltaTime, Iterations);
		return;
	}
	Super::PhysFalling(deltaTime, Iterations);
}

void UFPParkourMovementComponent::EndWallRun(float RemainingTime, int32 Iterations, const TCHAR* Reason)
{
	DebugWallRun(Reason);
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
		const TCHAR* DetachReason = nullptr;
		if (DurationLeft < MIN_TICK_TIME)
		{
			DetachReason = TEXT("detach: airborne time budget exhausted");
		}
		else if (!CharacterOwner->Controller || IsCrouching() || bWantsToCrouch)
		{
			DetachReason = TEXT("detach: crouched or no controller");
		}
		else if (HasAnimRootMotion() || CurrentRootMotion.HasOverrideVelocity())
		{
			DetachReason = TEXT("detach: root motion owns velocity");
		}
		else if (Velocity.Size2D() < FMath::Max(1.0f, WallRunMinSpeed))
		{
			DetachReason = TEXT("detach: below minimum horizontal speed");
		}
		else if (FVector::DotProduct(Input, WallRunNormal) > FMath::Clamp(WallRunDetachInputThreshold, 0.1f, 1.0f))
		{
			DetachReason = TEXT("detach: outward movement input");
		}
		if (DetachReason)
		{
			EndWallRun(RemainingTime, Iterations, DetachReason);
			return;
		}

		++Iterations;
		const float TimeTick = FMath::Min(GetSimulationTimeStep(RemainingTime, Iterations), DurationLeft);
		const bool bHasContact = TraceWall(-WallRunNormal, WallHit);
		if (bHasContact)
		{
			if (IsWallJumpSurfaceBlocked(WallHit))
			{
				EndWallRun(RemainingTime, Iterations, TEXT("detach: jumped surface still locked"));
				return;
			}
			WallRunContactLostTime = 0.0f;
			if (FVector::DotProduct(WallHit.ImpactNormal.GetSafeNormal2D(), WallRunNormal) < CornerCos)
			{
				EndWallRun(RemainingTime, Iterations, TEXT("detach: wall corner too sharp"));
				return;
			}
			WallRunNormal = WallHit.ImpactNormal.GetSafeNormal2D();
			DebugWallRun(TEXT("active: valid wall contact"));
		}
		else
		{
			WallRunContactLostTime += TimeTick;
			if (WallHit.bBlockingHit || WallRunContactLostTime > FMath::Clamp(WallRunContactGraceTime, 0.0f, 0.25f))
			{
				EndWallRun(RemainingTime, Iterations, WallHit.bBlockingHit
					? GetWallRejectionReason(WallHit) : TEXT("detach: wall contact lost beyond grace time"));
				return;
			}
			DebugWallRun(TEXT("contact grace: briefly following last valid wall normal"));
		}
		RemainingTime -= TimeTick;
		WallRunElapsed += TimeTick;
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
		const float Gap = bHasContact
			? FMath::Max(0.0f, WallHit.Distance - CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius() - 2.0f) : 0.0f;
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
		if (Hit.bStartPenetrating)
		{
			EndWallRun(RemainingTime, Iterations, TEXT("detach: unresolved capsule penetration"));
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
					DebugWallRun(TEXT("detach: landed on walkable floor"));
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
				if (IsWallJumpSurfaceBlocked(Hit))
				{
					EndWallRun(RemainingTime + UnusedTime, Iterations, TEXT("detach: collision with locked jumped surface"));
					return;
				}
				if (!IsSuitableWall(Hit) || FVector::DotProduct(Hit.ImpactNormal.GetSafeNormal2D(), WallRunNormal) < CornerCos)
				{
					EndWallRun(RemainingTime + UnusedTime, Iterations,
						IsSuitableWall(Hit) ? TEXT("detach: blocking corner too sharp") : GetWallRejectionReason(Hit));
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
				if (Hit.bStartPenetrating)
				{
					EndWallRun(RemainingTime, Iterations, TEXT("detach: unresolved slide penetration"));
					return;
				}
				if (!Hit.IsValidBlockingHit())
				{
					break;
				}
			}
		}
	}
	if (IsWallRunning() && WallRunElapsed >= FMath::Max(0.01f, WallRunMaxDuration))
	{
		EndWallRun(RemainingTime, Iterations, TEXT("detach: airborne time budget exhausted"));
	}
}
