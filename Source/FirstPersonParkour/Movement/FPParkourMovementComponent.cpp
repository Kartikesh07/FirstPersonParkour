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

static TAutoConsoleVariable<int32> CVarParkourGrappleDebug(
	TEXT("parkour.DebugGrapple"), 0, TEXT("Grapple rope lengths, state and release reason: 0 = off, 1 = on."));

DEFINE_LOG_CATEGORY_STATIC(LogParkourWallRun, Log, All);
DEFINE_LOG_CATEGORY_STATIC(LogParkourGrapple, Log, All);

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
	const float MinimumUpwardSpeed = FMath::Max(1.0f, WallJumpMinUpwardTakeoffSpeed);
	const float UpwardSpeed = FMath::Max(MinimumUpwardSpeed, Velocity.Z + FMath::Max(0.0f, WallJumpUpwardImpulse));
	FVector JumpVelocity = TangentialVelocity * FMath::Clamp(WallJumpMomentumRetention, 0.0f, 1.0f)
		+ Normal * (FMath::Max(0.0f, FVector::DotProduct(HorizontalVelocity, Normal))
			+ FMath::Max(0.0f, WallJumpOutwardImpulse))
		+ FVector::UpVector * UpwardSpeed;
	JumpVelocity = ConstrainDirectionToPlane(JumpVelocity);
	if (JumpVelocity.ContainsNaN() || FVector::DotProduct(JumpVelocity, Normal) <= UE_SMALL_NUMBER
		|| JumpVelocity.Z < MinimumUpwardSpeed - UE_KINDA_SMALL_NUMBER)
	{
		DebugWallRun(TEXT("wall jump rejected: movement constraint prevents safe upward takeoff"));
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
	WallJumpRunSource = Restriction;
	bWallJumpRunPending = true;
	CharacterOwner->StopJumping();
	Velocity = JumpVelocity;
	LastWallJumpTakeoffVelocity = JumpVelocity;
	SetMovementMode(MOVE_Falling);
	// A different wall can attach immediately if eligible. The jumped surface has its
	// own stronger restriction. Duration refresh waits for a valid different-wall attachment.
	WallRunReattachTime = GetWorld()->GetTimeSeconds();
	DebugWallRun(TEXT("wall jump: same surface locked; different-wall run reset armed"));
	if (AFPPlayerCharacter* Player = Cast<AFPPlayerCharacter>(CharacterOwner))
	{
		Player->NotifyWallJumpCamera(Normal);
	}
	return true;
}

bool UFPParkourMovementComponent::MatchesWallSurface(const FHitResult& Hit, const FWallJumpRestriction& Surface) const
{
	const FVector Normal = Hit.ImpactNormal.GetSafeNormal2D();
	const float SameSurfaceCos = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(WallJumpSameSurfaceAngle, 0.0f, 60.0f)));
	return FVector::DotProduct(Normal, Surface.Normal) >= SameSurfaceCos
		&& ((Surface.Surface.IsValid() && Surface.Surface.Get() == Hit.GetComponent())
			|| FMath::Abs(FVector::DotProduct(Hit.ImpactPoint - Surface.Point, Surface.Normal))
				<= FMath::Max(0.0f, WallJumpSurfacePlaneTolerance));
}

bool UFPParkourMovementComponent::IsWallJumpSurfaceBlocked(const FHitResult& Hit) const
{
	for (const FWallJumpRestriction& Restriction : WallJumpRestrictions)
	{
		if (MatchesWallSurface(Hit, Restriction))
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
	if (!IsFalling())
	{
		StopGrapple(IsMovingOnGround() ? TEXT("landed: ground movement mode") : TEXT("movement mode changed"));
	}
	if (!IsWallRunning())
	{
		WallRunHighSpeedBraking = 0.0f;
		bWallRunVerticalStabilizing = false;
	}
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
		bGrappleWallRunHandoffPending = false;
		WallRunElapsed = 0.0f;
		WallRunNumber = 0;
		bWallJumpRunPending = false;
		WallJumpRunSource = FWallJumpRestriction();
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
	if (bGrappling && HasValidData() && bShowGrappleDebug)
	{
		DrawDebugLine(GetWorld(), UpdatedComponent->GetComponentLocation(), GrappleAnchor,
			FColor::Cyan, false, 0.0f, 0, 1.5f);
	}
	if (IsGrappleStateDebugEnabled() && GEngine && CharacterOwner && CharacterOwner->IsLocallyControlled())
	{
		const float Distance = HasValidData() ? FVector::Distance(UpdatedComponent->GetComponentLocation(), GrappleAnchor) : 0.0f;
		const bool bTaut = Distance >= ActiveRopeLength - 0.1f;
		const bool bReeling = GrappleReelSpeed > 0.0f && ActiveRopeLength > DesiredRopeLength + 0.1f;
		const TCHAR* State = bTaut ? (bReeling ? TEXT("taut/reeling") : TEXT("taut/swinging"))
			: (bReeling ? TEXT("slack/reeling") : TEXT("slack"));
		GEngine->AddOnScreenDebugMessage((2ull << 32) | GetUniqueID(), 0.0f, FColor::Cyan,
			bGrappling ? GetGrappleDebugStatus(State, TEXT("none")) : LastGrappleReleaseStatus);
	}
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
	const FString Status = FString::Printf(TEXT("WallRun: %s | Mode %d/%d | XY %.0f/%.0f Z %.0f | run #%u %.2f/%.2f s | chain reset armed %d | cooldown %.3f s surface locks %d | grapple release XY %.0f Z %.0f age %.2f s handoff %d | alignment %.2f tangent %.0f descent limit %.0f | excess braking %.0f cm/s/s -> %.0f | entry Z %.0f vertical assist %s %.2f/%.2f s drift %.0f | jump takeoff %s"),
		Message, static_cast<int32>(MovementMode), CustomMovementMode, Velocity.Size2D(), FMath::Max(500.0f, WallRunMinSpeed),
		Velocity.Z, WallRunNumber, WallRunElapsed, WallRunMaxDuration, bWallJumpRunPending ? 1 : 0,
		FMath::Max(0.0, WallRunReattachTime - Now), WallJumpRestrictions.Num(), GrappleWallRunReleaseVelocity.Size2D(), GrappleWallRunReleaseVelocity.Z,
		GrappleWallRunReleaseTime >= 0.0 ? Now - GrappleWallRunReleaseTime : -1.0, IsGrappleWallRunHandoffActive() ? 1 : 0,
		WallRunEntryAlignment, WallRunEntryTangentialSpeed, WallRunEntryFallSpeedLimit, WallRunHighSpeedBraking, WallRunComfortSpeed,
		WallRunEntryVerticalVelocity, bWallRunVerticalStabilizing ? TEXT("active") : TEXT("off"),
		FMath::Min(WallRunElapsed, FMath::Max(0.0f, WallRunVerticalStabilizationDuration)),
		WallRunVerticalStabilizationDuration, WallRunMaxControlledDescentSpeed, *LastWallJumpTakeoffVelocity.ToCompactString());
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
	// A capsule sweep catches partial-height walls and oblique contacts that the
	// center/lower rays can miss. Normalize distance to the rays' center-distance
	// convention, used by wall attraction and wall-jump clearance.
	FHitResult CapsuleHit;
	GetWorld()->SweepSingleByChannel(CapsuleHit, Center,
		Center + Direction * FMath::Max(0.0f, WallDetectionDistance), UpdatedComponent->GetComponentQuat(),
		UpdatedComponent->GetCollisionObjectType(), Capsule->GetCollisionShape(), Params, Response);
	if (CapsuleHit.IsValidBlockingHit())
	{
		CapsuleHit.Distance += Capsule->GetScaledCapsuleRadius();
		if (bDebug)
		{
			DrawDebugLine(GetWorld(), Center, CapsuleHit.ImpactPoint, FColor::Orange, false, 0.05f, 0, 2.0f);
		}
		Hit = CapsuleHit;
		return IsSuitableWall(CapsuleHit);
	}
	return false;
}

bool UFPParkourMovementComponent::IsGrappleWallRunHandoffActive() const
{
	return bGrappleWallRunHandoffPending && GetWorld()
		&& GetWorld()->GetTimeSeconds() - GrappleWallRunReleaseTime <= FMath::Max(0.0f, WallRunGrappleHandoffWindow);
}

bool UFPParkourMovementComponent::TryStartWallRun(const FHitResult* ImpactHit, const FVector* ApproachVelocity)
{
	if (!HasValidData() || !IsFalling() || bGrappling)
	{
		return false;
	}
	WallRunEntryAlignment = 0.0f;
	WallRunEntryTangentialSpeed = 0.0f;
	WallRunEntryFallSpeedLimit = FMath::Max(0.0f, WallRunMaxEntryFallSpeed);
	const auto Reject = [this](const TCHAR* Reason)
	{
		DebugWallRun(Reason);
		return false;
	};
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
	const float MinimumSpeed = FMath::Max(500.0f, WallRunMinSpeed);
	if (Velocity.ContainsNaN() || Velocity.Size2D() < MinimumSpeed)
	{
		return Reject(TEXT("reject: below minimum horizontal speed"));
	}
	const FVector EntryVelocity = ApproachVelocity ? *ApproachVelocity : Velocity;
	if (EntryVelocity.ContainsNaN())
	{
		return Reject(TEXT("reject: invalid approach velocity"));
	}
	const FVector Travel = EntryVelocity.GetSafeNormal2D();
	// Enter while traveling forward, rather than when backing or strafing into a wall.
	if (!IsGrappleWallRunHandoffActive() && FVector::DotProduct(Travel, CharacterOwner->GetActorForwardVector()) <= 0.25f)
	{
		return Reject(TEXT("reject: not traveling forward"));
	}
	// Left/right are relative to travel, so looking around never breaks an active run.
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Travel);
	FHitResult BestHit;
	float BestDistance = UE_BIG_NUMBER;
	const TCHAR* Rejection = TEXT("reject: no blocking side wall within reach");
	const auto ConsiderWall = [&](const FHitResult& Hit)
	{
		if (!IsSuitableWall(Hit))
		{
			Rejection = GetWallRejectionReason(Hit);
			return;
		}
		if (IsWallJumpSurfaceBlocked(Hit))
		{
			Rejection = TEXT("reject: jumped surface needs cooldown and capsule separation");
			return;
		}
		// Check duration per candidate, after collision restrictions. An exhausted
		// old run must not hide an eligible different-wall jump destination.
		const bool bStartsNewRun = WallRunNumber == 0
			|| (bWallJumpRunPending && !MatchesWallSurface(Hit, WallJumpRunSource));
		if (!bStartsNewRun && WallRunElapsed >= FMath::Max(0.01f, WallRunMaxDuration) - MIN_TICK_TIME)
		{
			Rejection = bWallJumpRunPending
				? TEXT("reject: same-wall duration exhausted; jump did not authorize a same-wall reset")
				: TEXT("reject: current run duration exhausted; no different-wall jump reset");
			return;
		}
		const FVector Tangent = FVector::CrossProduct(FVector::UpVector, Hit.ImpactNormal).GetSafeNormal();
		WallRunEntryAlignment = FMath::Abs(FVector::DotProduct(Travel, Tangent));
		WallRunEntryTangentialSpeed = FMath::Abs(FVector::DotProduct(EntryVelocity, Tangent));
		WallRunEntryFallSpeedLimit = FMath::Max(FMath::Max(0.0f, WallRunMaxEntryFallSpeed),
			FMath::Min(FMath::Max(0.0f, WallRunMaxHighSpeedEntryFallSpeed),
				WallRunEntryTangentialSpeed * FMath::Clamp(WallRunMaxEntryDescentRatio, 0.0f, 1.0f)));
		if (FVector::DotProduct(Travel, Hit.ImpactNormal.GetSafeNormal2D()) > 0.15f)
		{
			Rejection = TEXT("reject: traveling away from wall");
			return;
		}
		if (WallRunEntryAlignment < FMath::Clamp(WallRunMinAlignment, 0.5f, 1.0f))
		{
			Rejection = TEXT("reject: head-on/approach angle too steep");
			return;
		}
		if (WallRunEntryTangentialSpeed < MinimumSpeed || FMath::Abs(FVector::DotProduct(Velocity, Tangent)) < MinimumSpeed)
		{
			Rejection = TEXT("reject: insufficient speed along wall");
			return;
		}
		if (EntryVelocity.Z < -WallRunEntryFallSpeedLimit || Velocity.Z < -WallRunEntryFallSpeedLimit)
		{
			Rejection = TEXT("reject: descending too fast for tangential speed");
			return;
		}
		if (Hit.Distance < BestDistance)
		{
			BestHit = Hit;
			BestDistance = Hit.Distance;
		}
	};
	if (ImpactHit && FVector::DistSquared(ImpactHit->Location, UpdatedComponent->GetComponentLocation())
		<= FMath::Square(CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius()))
	{
		// This is an actual collision at the current capsule, not a future wall
		// selected from a long predictive trace. Validate pre-deflection incidence.
		ConsiderWall(*ImpactHit);
	}
	for (const float Side : { -1.0f, 1.0f })
	{
		FHitResult Hit;
		if (TraceWall(Right * Side, Hit))
		{
			ConsiderWall(Hit);
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
	const FVector EntryTangent = FVector::CrossProduct(FVector::UpVector, BestHit.ImpactNormal).GetSafeNormal();
	WallRunEntryAlignment = FMath::Abs(FVector::DotProduct(Travel, EntryTangent));
	WallRunEntryTangentialSpeed = FMath::Abs(FVector::DotProduct(EntryVelocity, EntryTangent));
	WallRunEntryFallSpeedLimit = FMath::Max(FMath::Max(0.0f, WallRunMaxEntryFallSpeed),
		FMath::Min(FMath::Max(0.0f, WallRunMaxHighSpeedEntryFallSpeed),
			WallRunEntryTangentialSpeed * FMath::Clamp(WallRunMaxEntryDescentRatio, 0.0f, 1.0f)));
	WallRunNormal = BestHit.ImpactNormal.GetSafeNormal2D();
	WallRunDirection = Travel;
	const bool bInitialRun = WallRunNumber == 0;
	const bool bStartsNewRun = bInitialRun
		|| (bWallJumpRunPending && !MatchesWallSurface(BestHit, WallJumpRunSource));
	// No velocity boost, vertical reset or position snap on entry.
	SetMovementMode(MOVE_Custom, WallRunMode);
	if (!IsWallRunning())
	{
		return Reject(TEXT("reject: movement mode transition failed"));
	}
	if (bStartsNewRun)
	{
		WallRunElapsed = 0.0f;
		++WallRunNumber;
	}
	WallRunEntryVerticalVelocity = Velocity.Z;
	bWallRunVerticalStabilizing = false;
	// Consume the authorization on the first successful attachment, even if it
	// resumes the same wall. Seam misses and mode changes cannot keep it alive.
	bWallJumpRunPending = false;
	bGrappleWallRunHandoffPending = false;
	DebugWallRun(bInitialRun ? TEXT("duration reset: first run since landing")
		: bStartsNewRun ? TEXT("duration reset: successful wall jump to a different surface")
		: TEXT("reattached: duration retained; no legitimate different-wall jump reset"));
	return true;
}

void UFPParkourMovementComponent::PhysFalling(float deltaTime, int32 Iterations)
{
	if (HasValidData())
	{
		UpdateWallJumpRestrictions();
	}
	if (bGrappling)
	{
		PhysGrapple(deltaTime, Iterations);
		return;
	}
	if (deltaTime < MIN_TICK_TIME || Iterations >= MaxSimulationIterations || !HasValidData())
	{
		return;
	}
	if (!IsGrappleWallRunHandoffActive() && Velocity.Size2D() <= FMath::Max(500.0f, WallRunComfortSpeed))
	{
		// Preserve native air-control integration for ordinary jumping/falling.
		// Extra travel sampling is needed only for fast traversal or rope handoff.
		if (TryStartWallRun())
		{
			StartNewPhysics(deltaTime, Iterations);
			return;
		}
		Super::PhysFalling(deltaTime, Iterations);
		return;
	}
	float RemainingTime = deltaTime;
	// Sample by actual travel distance, not render-frame count. Every slice still
	// uses native swept falling, gravity, air control and landing response.
	for (int32 Step = 0; Step < 64 && RemainingTime >= MIN_TICK_TIME; ++Step)
	{
		if (!HasValidData())
		{
			return;
		}
		if (!IsFalling() || bGrappling)
		{
			StartNewPhysics(RemainingTime, Iterations + 1);
			return;
		}
		UpdateWallJumpRestrictions();
		if (TryStartWallRun())
		{
			StartNewPhysics(RemainingTime, Iterations + 1);
			return;
		}
		const float TravelTime = CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius() * 0.5f
			/ FMath::Max(1.0, Velocity.Size());
		const float TimeTick = FMath::Min3(RemainingTime, 1.0f / 60.0f, FMath::Max(MIN_TICK_TIME, TravelTime));
		FallingWallImpact = FHitResult();
		{
			TGuardValue<bool> CaptureImpact(bCaptureFallingWallImpact, true);
			Super::PhysFalling(TimeTick, Iterations);
		}
		RemainingTime -= TimeTick;
		if (!HasValidData())
		{
			return;
		}
		if (IsFalling() && !bGrappling && FallingWallImpact.IsValidBlockingHit())
		{
			TryStartWallRun(&FallingWallImpact, &FallingWallApproachVelocity);
		}
		if (!IsFalling() || bGrappling)
		{
			StartNewPhysics(RemainingTime, Iterations + 1);
			return;
		}
	}
	if (RemainingTime >= MIN_TICK_TIME)
	{
		// Keep hitch work bounded; native falling still sweeps the entire remainder.
		Super::PhysFalling(RemainingTime, Iterations);
	}
}

void UFPParkourMovementComponent::HandleImpact(const FHitResult& Hit, float TimeSlice, const FVector& MoveDelta)
{
	if (bCaptureFallingWallImpact && HasValidData() && IsFalling() && !bGrappling
		&& !FallingWallImpact.IsValidBlockingHit() && IsSuitableWall(Hit))
	{
		// Retain the approach before native collision removes inward velocity.
		// Entry waits until native falling finishes this short slice, so no unused
		// simulation time is lost and no mode changes occur inside impact callbacks.
		FallingWallImpact = Hit;
		FallingWallApproachVelocity = Velocity;
	}
	Super::HandleImpact(Hit, TimeSlice, MoveDelta);
}

bool UFPParkourMovementComponent::TryStartGrapple(const FVector& ViewLocation, const FVector& ViewDirection)
{
	if (!HasValidData() || bGrappling || !CharacterOwner->Controller
		|| (!IsMovingOnGround() && !IsFalling() && !IsWallRunning())
		|| HasAnimRootMotion() || CurrentRootMotion.HasOverrideVelocity()
		|| ViewLocation.ContainsNaN() || ViewDirection.ContainsNaN() || ViewDirection.IsNearlyZero())
	{
		return false;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(GrappleTarget), false, CharacterOwner);
	FCollisionResponseParams Response;
	InitCollisionParams(Params, Response);
	FHitResult Hit;
	GetWorld()->LineTraceSingleByChannel(Hit, ViewLocation,
		ViewLocation + ViewDirection.GetSafeNormal() * FMath::Max(1.0f, GrappleRange),
		UpdatedComponent->GetCollisionObjectType(), Params, Response);
	UPrimitiveComponent* Surface = Hit.GetComponent();
	if (!Hit.IsValidBlockingHit() || !IsValid(Surface) || Cast<APawn>(Hit.GetActor())
		|| Surface->IsSimulatingPhysics() || !Surface->GetComponentVelocity().IsNearlyZero(1.0f)
		|| Hit.ImpactPoint.ContainsNaN() || !Hit.ImpactNormal.IsNormalized())
	{
		return false;
	}
	GrappleAnchor = Hit.ImpactPoint;
	GrappleNormal = Hit.ImpactNormal;
	GrappleSurface = Surface;
	GrappleSurfaceTransform = Surface->GetComponentTransform();
	const float Distance = FVector::Distance(UpdatedComponent->GetComponentLocation(), GrappleAnchor);
	if (Distance <= FMath::Max(1.0f, GrappleArrivalDistance) || Distance > FMath::Max(1.0f, GrappleRange)
		|| !IsGrappleAnchorValid() || !IsGrapplePathClear(true))
	{
		GrappleSurface.Reset();
		return false;
	}
	CharacterOwner->StopJumping();
	StopSlide();
	SetMovementMode(MOVE_Falling);
	// The target is never an initial length or a lower bound on rope take-up.
	// Short shots must not silently acquire the old default's 1200 cm of slack.
	ActiveRopeLength = Distance + FMath::Max(0.0f, GrappleInitialSlack);
	DesiredRopeLength = FMath::Min(ActiveRopeLength, FMath::Max(100.0f, GrappleRopeLength));
	bGrappleClearanceLimited = false;
	UpdateGrappleReelTarget();
	bGrappling = IsFalling();
	bGrappleWallRunHandoffPending = false;
	return bGrappling;
}

void UFPParkourMovementComponent::StopGrapple(const TCHAR* Reason)
{
	if (bGrappling)
	{
		LastGrappleReleaseStatus = GetGrappleDebugStatus(TEXT("detached"), Reason);
		if (IsGrappleStateDebugEnabled())
		{
			UE_LOG(LogParkourGrapple, Log, TEXT("%s: %s"), *GetNameSafe(CharacterOwner), *LastGrappleReleaseStatus);
		}
		FallingSpeedLimit = FMath::Max(FallingSpeedLimit, Velocity.Size2D());
		// Releasing the rope is not a failed wall run. Do not impose a new generic
		// cooldown or clear an existing wall-run cooldown / jumped-surface lock.
		if (GetWorld())
		{
			GrappleWallRunReleaseTime = GetWorld()->GetTimeSeconds();
			GrappleWallRunReleaseVelocity = Velocity;
			bGrappleWallRunHandoffPending = IsFalling() && !Velocity.ContainsNaN();
			DebugWallRun(TEXT("grapple released: velocity retained; existing cooldown and surface locks retained"));
		}
	}
	bGrappling = false;
	ActiveRopeLength = 0.0f;
	DesiredRopeLength = 0.0f;
	GrappleSurface.Reset();
}

bool UFPParkourMovementComponent::IsGrappleStateDebugEnabled() const
{
	return bShowGrappleStateDebug || CVarParkourGrappleDebug.GetValueOnGameThread() != 0;
}

FString UFPParkourMovementComponent::GetGrappleDebugStatus(const TCHAR* State, const TCHAR* Reason) const
{
	const float Distance = HasValidData() ? FVector::Distance(UpdatedComponent->GetComponentLocation(), GrappleAnchor) : 0.0f;
	return FString::Printf(TEXT("Grapple: %s | rope %.1f cm | distance %.1f cm | target %.1f cm | clearance target %s | release: %s"),
		State, ActiveRopeLength, Distance, DesiredRopeLength, bGrappleClearanceLimited ? TEXT("limited") : TEXT("normal"), Reason);
}

void UFPParkourMovementComponent::UpdateGrappleReelTarget()
{
	const FVector Center = UpdatedComponent->GetComponentLocation();
	const FVector Down = GetGravityDirection();
	if (GrappleMinSwingClearance <= 0.0f || FVector::DotProduct(GrappleAnchor - Center, -Down) <= 0.0)
	{
		return;
	}
	const UCapsuleComponent* Capsule = CharacterOwner->GetCapsuleComponent();
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const float Support = Radius + (Capsule->GetScaledCapsuleHalfHeight() - Radius)
		* FMath::Abs(FVector::DotProduct(Capsule->GetUpVector(), Down));
	const float Reach = ActiveRopeLength + Support + GrappleMinSwingClearance;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(GrappleGroundClearance), false, CharacterOwner);
	FCollisionResponseParams Response;
	InitCollisionParams(Params, Response);
	// Probe both the swing's lowest point and the ground currently under the
	// player. A floor plane predicts clearance before the swing descends into it.
	for (const FVector Start : { GrappleAnchor + Down * 2.0f, Center })
	{
		FHitResult FloorHit;
		if (!GetWorld()->LineTraceSingleByChannel(FloorHit, Start, Start + Down * Reach,
			UpdatedComponent->GetCollisionObjectType(), Params, Response) || !FloorHit.IsValidBlockingHit() || !IsWalkable(FloorHit))
		{
			continue;
		}
		const double Denominator = FVector::DotProduct(Down, FloorHit.ImpactNormal);
		if (Denominator >= -UE_SMALL_NUMBER)
		{
			continue;
		}
		const float AnchorHeight = FVector::DotProduct(FloorHit.ImpactPoint - GrappleAnchor, FloorHit.ImpactNormal) / Denominator;
		const float ClearanceLength = FMath::Max(100.0f, AnchorHeight - Support - GrappleMinSwingClearance);
		if (ClearanceLength < DesiredRopeLength)
		{
			DesiredRopeLength = ClearanceLength;
			bGrappleClearanceLimited = true;
		}
	}
	// This only requests a shorter target. Pull force, reel speed, actual capsule
	// travel and collision determine whether it is achievable before reaching ground.
}

bool UFPParkourMovementComponent::IsGrappleAnchorValid() const
{
	const UPrimitiveComponent* Surface = GrappleSurface.Get();
	if (!HasValidData() || !CharacterOwner->Controller || !IsValid(Surface)
		|| !Surface->IsQueryCollisionEnabled() || Surface->IsSimulatingPhysics()
		|| Surface->GetCollisionResponseToChannel(UpdatedComponent->GetCollisionObjectType()) != ECR_Block
		|| !Surface->GetComponentTransform().Equals(GrappleSurfaceTransform, 0.01f)
		|| HasAnimRootMotion() || CurrentRootMotion.HasOverrideVelocity())
	{
		return false;
	}
	// Range/arrival apply to acquisition only. A slack rope may carry the player
	// close to the anchor, or beyond the acquisition range when configured longer.
	return !GrappleAnchor.ContainsNaN()
		&& FVector::DistSquared(UpdatedComponent->GetComponentLocation(), GrappleAnchor) > 1.0f;
}

bool UFPParkourMovementComponent::IsGrapplePathClear(bool bCheckCapsulePath) const
{
	FCollisionQueryParams Params(SCENE_QUERY_STAT(GrappleClearance), false, CharacterOwner);
	FCollisionResponseParams Response;
	InitCollisionParams(Params, Response);
	const FVector Center = UpdatedComponent->GetComponentLocation();
	FHitResult Hit;
	// Extend slightly into the surface so the anchor must still exist. An earlier
	// hit on the same mesh is an obstruction too (e.g. the near side of a concave wall).
	GetWorld()->LineTraceSingleByChannel(Hit, Center, GrappleAnchor + (GrappleAnchor - Center).GetSafeNormal() * 2.0f,
		UpdatedComponent->GetCollisionObjectType(), Params, Response);
	if (!Hit.IsValidBlockingHit() || Hit.GetComponent() != GrappleSurface.Get()
		|| !Hit.ImpactPoint.Equals(GrappleAnchor, 3.0f))
	{
		return false;
	}
	if (!bCheckCapsulePath)
	{
		// A swinging capsule need not be able to travel directly to the anchor.
		// Only the rope needs line of sight; actual travel is swept in PhysGrapple.
		return true;
	}
	// Stop the clearance sweep short of the anchor by the capsule's support
	// distance, so a valid destination wall/floor/ceiling does not reject itself.
	const UCapsuleComponent* Capsule = CharacterOwner->GetCapsuleComponent();
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const float Support = Radius + (Capsule->GetScaledCapsuleHalfHeight() - Radius) * FMath::Abs(GrappleNormal.Z);
	const FVector Destination = GrappleAnchor + GrappleNormal * (Support + 2.0f);
	return !GetWorld()->SweepSingleByChannel(Hit, Center, Destination,
		UpdatedComponent->GetComponentQuat(), UpdatedComponent->GetCollisionObjectType(),
		Capsule->GetCollisionShape(), Params, Response);
}

FVector UFPParkourMovementComponent::NewFallVelocity(const FVector& InitialVelocity, const FVector& Gravity, float DeltaTime) const
{
	FVector Result = Super::NewFallVelocity(InitialVelocity, Gravity, DeltaTime);
	if (bGrappling && IsFalling() && DeltaTime > 0.0f)
	{
		const FVector ToAnchor = GrappleAnchor - UpdatedComponent->GetComponentLocation();
		const FVector Direction = ConstrainDirectionToPlane(ToAnchor).GetSafeNormal();
		// A force-limited radial speed controller accounts for gravity and the
		// centrifugal load. It cannot shorten the rope by editing position or
		// invent a velocity impulse when clearance requires faster reeling.
		const float Error = FMath::Max(0.0f, static_cast<float>(ToAnchor.Size()) - DesiredRopeLength);
		const float PullAcceleration = FMath::Max(0.0f, GrapplePullAcceleration);
		const float ReelSpeed = FMath::Min(FMath::Max(0.0f, GrappleReelSpeed), FMath::Max(1.0f, GrappleMaxPullSpeed));
		const float RequestedSpeed = FMath::Min(ReelSpeed, FMath::Sqrt(2.0f * PullAcceleration * Error));
		const float InwardSpeed = FVector::DotProduct(InitialVelocity, Direction);
		const double TangentialSpeedSquared = FVector::VectorPlaneProject(InitialVelocity, Direction).SizeSquared();
		const float RequiredAcceleration = (RequestedSpeed - InwardSpeed) / DeltaTime
			- FVector::DotProduct(Gravity, Direction) + TangentialSpeedSquared / FMath::Max(1.0, ToAnchor.Size());
		const float AppliedAcceleration = Error > 0.01f && ReelSpeed > 0.0f
			? FMath::Clamp(RequiredAcceleration, 0.0f, PullAcceleration) : 0.0f;
		const float Along = FVector::DotProduct(Result, Direction);
		const float Limit = FMath::Max(FMath::Max(1.0f, GrappleMaxPullSpeed), Result.Size());
		// Solve |v + direction * dv| <= limit. Cap only the added pull, preserving
		// tangential and faster inherited momentum rather than clamping entry velocity.
		const float Available = FMath::Max(0.0f, -Along + FMath::Sqrt(FMath::Max(0.0f,
			Along * Along + Limit * Limit - Result.SizeSquared())));
		const float SpeedBudget = FMath::Max(0.0f, ReelSpeed - Along);
		Result += Direction * FMath::Min3(AppliedAcceleration * DeltaTime, Available, SpeedBudget);

		const FVector Input = (Acceleration / FMath::Max(1.0f, GetMaxAcceleration())).GetClampedToMaxSize(1.0f);
		const FVector Steering = FVector::VectorPlaneProject(Input, ToAnchor.GetSafeNormal());
		const float SpeedLimit = FMath::Max(FMath::Max(1.0f, MaxSwingSpeed), Result.Size());
		Result = (Result + ConstrainDirectionToPlane(Steering) * FMath::Max(0.0f, SwingAcceleration)
			* FMath::Clamp(SwingSteeringInfluence, 0.0f, 1.0f) * DeltaTime).GetClampedToMaxSize(SpeedLimit);

		const FVector Radial = ToAnchor.GetSafeNormal() * FVector::DotProduct(Result, ToAnchor.GetSafeNormal());
		Result = Radial + (Result - Radial) * FMath::Exp(-FMath::Max(0.0f, SwingDamping) * DeltaTime);
		const float MaximumSpeed = FMath::Max(1.0f, MaxSwingSpeed);
		const float InheritedExcess = FMath::Max(0.0f, static_cast<float>(InitialVelocity.Size()) - MaximumSpeed);
		// Forces cannot generate speed above the ceiling. Entry momentum above
		// it has a continuous half-second decay instead of an attachment-time clamp.
		Result = Result.GetClampedToMaxSize(MaximumSpeed + InheritedExcess * FMath::Exp(-2.0f * DeltaTime));
	}
	return Result;
}

FVector UFPParkourMovementComponent::LimitGrappleDelta(const FVector& Location, const FVector& Delta) const
{
	const FVector EndOffset = Location + Delta - GrappleAnchor;
	return GrappleAnchor + EndOffset.GetClampedToMaxSize(ActiveRopeLength) - Location;
}

FVector UFPParkourMovementComponent::ComputeGrappleDrift(const FVector& Location, FVector& DriftVelocity, float DeltaTime) const
{
	const FVector Offset = Location - GrappleAnchor;
	const FVector FreeEnd = Offset + DriftVelocity * DeltaTime;
	if (FreeEnd.SizeSquared() <= FMath::Square(ActiveRopeLength))
	{
		return FreeEnd - Offset;
	}

	// Find the instant a slack rope becomes taut. Only its outward radial
	// momentum is lost on capture; tension does no work on tangential motion.
	const double SpeedSquared = DriftVelocity.SizeSquared();
	const double Along = FVector::DotProduct(Offset, DriftVelocity);
	const double Discriminant = Along * Along + SpeedSquared
		* FMath::Max(0.0, FMath::Square(static_cast<double>(ActiveRopeLength)) - Offset.SizeSquared());
	const float FreeTime = SpeedSquared > UE_SMALL_NUMBER
		? FMath::Clamp(static_cast<float>((-Along + FMath::Sqrt(Discriminant)) / SpeedSquared), 0.0f, DeltaTime) : 0.0f;
	FVector Normal = (Offset + DriftVelocity * FreeTime).GetSafeNormal();
	DriftVelocity -= Normal * FMath::Max(0.0, FVector::DotProduct(DriftVelocity, Normal));
	const FVector Tangent = FVector::VectorPlaneProject(DriftVelocity, Normal);
	const FVector Axis = FVector::CrossProduct(Normal, Tangent).GetSafeNormal();
	const float Angle = static_cast<float>(Tangent.Size() * (DeltaTime - FreeTime) / ActiveRopeLength);
	if (!Axis.IsNearlyZero())
	{
		// Exact force-free arc drift and parallel transport preserve tangent speed.
		// Sweeping the resulting short chord stays inside the unilateral rope ball.
		const FQuat Rotation(Axis, Angle);
		Normal = Rotation.RotateVector(Normal);
		DriftVelocity = Rotation.RotateVector(DriftVelocity);
	}
	return GrappleAnchor + Normal * ActiveRopeLength - Location;
}

void UFPParkourMovementComponent::PhysGrapple(float DeltaTime, int32 Iterations)
{
	if (DeltaTime < MIN_TICK_TIME || Iterations >= MaxSimulationIterations)
	{
		return;
	}
	++Iterations;
	float RemainingTime = DeltaTime;
	// Bounded 120 Hz physics slices even when the render frame is long. Unlike
	// native mode-transition iterations, these slices never recurse into physics.
	// Under a severe hitch detach and let native falling consume the remainder.
	constexpr int32 MaxGrappleSteps = 64;
	for (int32 Step = 0; RemainingTime >= MIN_TICK_TIME && Step < MaxGrappleSteps; ++Step)
	{
		if (!HasValidData())
		{
			return;
		}
		if (!bGrappling || !IsFalling())
		{
			StartNewPhysics(RemainingTime, Iterations);
			return;
		}
		const TCHAR* ReleaseReason = !IsGrappleAnchorValid() ? TEXT("anchor invalid/moved or movement ownership changed")
			: !IsGrapplePathClear() ? TEXT("rope obstructed") : Velocity.ContainsNaN() ? TEXT("invalid velocity") : nullptr;
		if (ReleaseReason)
		{
			StopGrapple(ReleaseReason);
			Super::PhysFalling(RemainingTime, Iterations);
			return;
		}
		UpdateGrappleReelTarget();
		const float TimeTick = FMath::Min(RemainingTime, 1.0f / 120.0f);
		RemainingTime -= TimeTick;
		const FVector OldLocation = UpdatedComponent->GetComponentLocation();
		const FVector Gravity = -GetGravityDirection() * GetGravityZ();
		// Kick-drift-kick integration: gravity and input do work, tension only
		// redirects. Native air control is not also applied to the grapple.
		FVector HalfVelocity = NewFallVelocity(Velocity, Gravity, TimeTick * 0.5f);
		const FVector OldNormal = (OldLocation - GrappleAnchor).GetSafeNormal();
		if (FVector::DistSquared(OldLocation, GrappleAnchor) >= FMath::Square(FMath::Max(0.0f, ActiveRopeLength - 0.01f)))
		{
			HalfVelocity -= OldNormal * FMath::Max(0.0, FVector::DotProduct(HalfVelocity, OldNormal));
		}
		const FVector UnconstrainedHalfVelocity = HalfVelocity;
		const FVector Delta = ComputeGrappleDrift(OldLocation, HalfVelocity, TimeTick);
		Velocity = HalfVelocity;
		FHitResult Hit;
		SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);
		FVector SweepDelta = Delta;
		FVector ContactNormals[2];
		int32 ContactCount = 0;
		float UnusedTime = TimeTick * (1.0f - Hit.Time);
		for (int32 Impact = 0; Impact < 2 && Hit.bBlockingHit; ++Impact)
		{
			if (!HasValidData() || !IsFalling() || !bGrappling)
			{
				StartNewPhysics(RemainingTime + UnusedTime, Iterations);
				return;
			}
			if (Hit.bStartPenetrating)
			{
				StopGrapple(TEXT("unresolved capsule penetration"));
				Super::PhysFalling(RemainingTime, Iterations);
				return;
			}
			if (Impact == 0)
			{
				// Use the velocity at the collision time, not at the planned arc's
				// endpoint. The latter can already be descending after a grazing hit.
				Velocity = UnconstrainedHalfVelocity;
				ComputeGrappleDrift(OldLocation, Velocity, TimeTick * Hit.Time);
			}
			if (FVector::DotProduct(Velocity, GetGravityDirection()) > 1.0
				&& FVector::DotProduct(Velocity, Hit.ImpactNormal) < -1.0
				&& FVector::DotProduct(SweepDelta, Hit.ImpactNormal) < -UE_SMALL_NUMBER
				&& IsValidLandingSpot(UpdatedComponent->GetComponentLocation(), Hit))
			{
				// Genuine downward floor contact retains the native landing path,
				// including camera feedback. A grazing/ascending contact only slides.
				StopGrapple(TEXT("landed: descending onto a walkable floor"));
				ProcessLanded(Hit, RemainingTime + UnusedTime, Iterations);
				return;
			}
			HandleImpact(Hit, TimeTick, Delta);
			if (!HasValidData() || !IsFalling() || !bGrappling)
			{
				StartNewPhysics(RemainingTime + UnusedTime, Iterations);
				return;
			}
			ContactNormals[ContactCount++] = Hit.Normal;
			Velocity -= Hit.Normal * FMath::Min(0.0, FVector::DotProduct(Velocity, Hit.Normal));
			if (Impact == 1 || UnusedTime < MIN_TICK_TIME)
			{
				break;
			}
			const FVector SlideDelta = LimitGrappleDelta(UpdatedComponent->GetComponentLocation(),
				ComputeSlideVector(Velocity * UnusedTime, 1.0f, Hit.Normal, Hit));
			SweepDelta = SlideDelta;
			SafeMoveUpdatedComponent(SlideDelta, UpdatedComponent->GetComponentQuat(), true, Hit);
			UnusedTime *= 1.0f - Hit.Time;
		}
		if (!HasValidData())
		{
			return;
		}
		if (!IsFalling() || !bGrappling)
		{
			StartNewPhysics(RemainingTime, Iterations);
			return;
		}
		const FVector Offset = UpdatedComponent->GetComponentLocation() - GrappleAnchor;
		const float Distance = Offset.Size();
		if (Distance > ActiveRopeLength + 0.5f || Distance < 1.0f)
		{
			// Collision/depenetration or another movement source owns the position.
			// Release instead of dragging the capsule back through geometry.
			StopGrapple(Distance < 1.0f ? TEXT("anchor reached") : TEXT("collision displaced capsule beyond rope limit"));
			Super::PhysFalling(RemainingTime, Iterations);
			return;
		}
		// Take up only rope the swept capsule actually made available, no faster
		// than the reel speed. Neither target changes nor inward momentum can extend
		// the rope or shrink it through the player's current collision position.
		const float TakeUp = FMath::Max(0.0f, GrappleReelSpeed) * TimeTick;
		ActiveRopeLength = FMath::Min(ActiveRopeLength,
			FMath::Max3(DesiredRopeLength, ActiveRopeLength - TakeUp, Distance));
		Velocity = NewFallVelocity(Velocity, Gravity, TimeTick * 0.5f);
		const FVector Normal = Offset.GetSafeNormal();
		const bool bTaut = Distance >= ActiveRopeLength - 0.01f;
		// Resolve rope tension and contact together. Every projection is energy
		// non-increasing, including a wall meeting the rope at an oblique angle.
		for (int32 Pass = 0; Pass < 4; ++Pass)
		{
			if (bTaut)
			{
				Velocity -= Normal * FMath::Max(0.0, FVector::DotProduct(Velocity, Normal));
			}
			for (int32 Contact = 0; Contact < ContactCount; ++Contact)
			{
				Velocity -= ContactNormals[Contact] * FMath::Min(0.0, FVector::DotProduct(Velocity, ContactNormals[Contact]));
			}
		}
		if (bNotifyApex && GetGravitySpaceZ(Velocity) < 0.0)
		{
			bNotifyApex = false;
			NotifyJumpApex();
		}
		FallingSpeedLimit = FMath::Max(FallingSpeedLimit, Velocity.Size2D());
	}
	if (RemainingTime >= MIN_TICK_TIME && HasValidData())
	{
		StopGrapple(TEXT("physics hitch exceeded grapple step budget"));
		Super::PhysFalling(RemainingTime, Iterations);
	}
}

void UFPParkourMovementComponent::EndWallRun(float RemainingTime, int32 Iterations, const TCHAR* Reason)
{
	DebugWallRun(Reason);
	// Preserve velocity and let native falling consume any unused simulation time.
	SetMovementMode(MOVE_Falling);
	StartNewPhysics(RemainingTime, Iterations);
}

void UFPParkourMovementComponent::ApplyWallRunVerticalControl(float PreviousVerticalVelocity, float GravityZ, float DeltaTime, bool bHasWallContact)
{
	bWallRunVerticalStabilizing = false;
	if (!bHasWallContact || DeltaTime < MIN_TICK_TIME || Velocity.Z >= 0.0f || GravityZ > 0.0f)
	{
		// Upward motion and contact-grace intervals retain the existing gravity.
		return;
	}
	const float DriftSpeed = FMath::Max(1.0f, WallRunMaxControlledDescentSpeed);
	const float DownGravity = FMath::Max(0.0f, -GravityZ);
	// If ascent ends inside this slice, apply descent drag only after the apex.
	const float AscentTime = PreviousVerticalVelocity > 0.0f && DownGravity > UE_SMALL_NUMBER
		? FMath::Min(DeltaTime, PreviousVerticalVelocity / DownGravity) : 0.0f;
	float DownTime = DeltaTime - AscentTime;
	float DownSpeed = FMath::Max(0.0f, -PreviousVerticalVelocity);
	const float DragRate = DownGravity / DriftSpeed;
	const auto IntegrateDescent = [DownGravity, DragRate](float Speed, float Time, float UpwardAcceleration)
	{
		// Exact constant gravity/force plus linear drag over this interval.
		if (DragRate > UE_SMALL_NUMBER)
		{
			const float Equilibrium = (DownGravity - UpwardAcceleration) / DragRate;
			return Equilibrium + (Speed - Equilibrium) * FMath::Exp(-DragRate * Time);
		}
		return Speed + (DownGravity - UpwardAcceleration) * Time;
	};
	const float EntryTimeLeft = FMath::Max(0.0f, WallRunVerticalStabilizationDuration
		- (WallRunElapsed - DeltaTime + AscentTime));
	const float AssistTime = FMath::Min(DownTime, EntryTimeLeft);
	const float Strength = FMath::Max(0.0f, WallRunVerticalStabilizationStrength);
	if (DownSpeed > DriftSpeed && AssistTime > 0.0f && Strength > 0.0f)
	{
		// Stop the integrated assist at the drift target, never at an upward speed.
		// The window uses retained run time, so seam reattachment cannot refresh it.
		DownSpeed = FMath::Max(DriftSpeed, IntegrateDescent(DownSpeed, AssistTime, Strength));
		DownTime -= AssistTime;
		bWallRunVerticalStabilizing = true;
	}
	DownSpeed = IntegrateDescent(DownSpeed, DownTime, 0.0f);
	// Preserve any stricter native terminal-velocity limit. Only Z is affected;
	// the resulting movement still uses the normal swept capsule and impact path.
	Velocity.Z = FMath::Max(Velocity.Z, -DownSpeed);
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
			DetachReason = TEXT("detach: current run duration exhausted");
		}
		else if (!CharacterOwner->Controller || IsCrouching() || bWantsToCrouch)
		{
			DetachReason = TEXT("detach: crouched or no controller");
		}
		else if (HasAnimRootMotion() || CurrentRootMotion.HasOverrideVelocity())
		{
			DetachReason = TEXT("detach: root motion owns velocity");
		}
		else if (Velocity.Size2D() < FMath::Max(500.0f, WallRunMinSpeed))
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
		float TimeTick = FMath::Min(GetSimulationTimeStep(RemainingTime, Iterations), DurationLeft);
		if (Iterations < MaxSimulationIterations)
		{
			// Fast travel gets more contact/turn samples, without reaching for a
			// distant future wall or bypassing the normal native iteration budget.
			const float TravelTime = CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius()
				/ FMath::Max(1.0, Velocity.Size());
			TimeTick = FMath::Min3(TimeTick, 1.0f / 60.0f, FMath::Max(MIN_TICK_TIME, TravelTime));
		}
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
		const float TangentialSpeed = FMath::Abs(FVector::DotProduct(Velocity, Tangent));
		const float ComfortableSpeed = FMath::Max(FMath::Max(500.0f, WallRunMinSpeed), WallRunComfortSpeed);
		const float Excess = FMath::Max(0.0f, TangentialSpeed - ComfortableSpeed);
		const float TangentialReduction = Excess * (1.0f - FMath::Exp(-FMath::Max(0.0f, WallRunHighSpeedBrakingRate) * TimeTick));
		const float AdaptedSpeed = Velocity.Size2D() * (TangentialSpeed > UE_SMALL_NUMBER
			? 1.0f - TangentialReduction / TangentialSpeed : 1.0f);
		WallRunHighSpeedBraking = (Velocity.Size2D() - AdaptedSpeed) / TimeTick;
		const float Speed = FMath::Max(0.0f, AdaptedSpeed - OpposingInput * FMath::Max(0.0f, WallRunBrakingDeceleration) * TimeTick);
		Velocity.X = WallRunDirection.X * Speed;
		Velocity.Y = WallRunDirection.Y * Speed;
		const float WallGravityZ = GetGravityZ() * FMath::Clamp(WallRunGravityScale, 0.0f, 1.0f);
		Velocity = NewFallVelocity(Velocity, FVector(0.0f, 0.0f, WallGravityZ), TimeTick);
		ApplyWallRunVerticalControl(OldVelocity.Z, WallGravityZ, TimeTick, bHasContact);
		if (bHasContact)
		{
			DebugWallRun(WallRunHighSpeedBraking > 1.0f ? TEXT("active: high-speed braking") : TEXT("active: valid wall contact"));
		}

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
		EndWallRun(RemainingTime, Iterations, TEXT("detach: current run duration exhausted"));
	}
}
