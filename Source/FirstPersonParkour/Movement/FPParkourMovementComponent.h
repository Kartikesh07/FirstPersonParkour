// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "FPParkourMovementComponent.generated.h"

/**
 * Custom character movement component for first-person parkour locomotion.
 * Preserves native walking, falling, collision, gravity, and jumping while
 * serving as an extensible foundation for future parkour movement modes.
 */
UCLASS()
class FIRSTPERSONPARKOUR_API UFPParkourMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	UFPParkourMovementComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	virtual float GetMaxSpeed() const override;
	virtual float GetMaxAcceleration() const override;
	virtual void BeginPlay() override;
	virtual void Crouch(bool bClientSimulation = false) override;
	virtual void UnCrouch(bool bClientSimulation = false) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	void SetSprintRequested(bool bRequested) { bSprintRequested = bRequested; }
	bool IsSprinting() const { return bSprintRequested && IsMovingOnGround() && !IsCrouching(); }
	void RequestSlide() { bSlideRequested = true; }
	void StopSlide();
	bool IsSliding() const { return bSliding; }
	bool PrepareSlideJump();
	/** One attempt per input press; requires fresh contact with the active wall. */
	bool TryWallJump();
	bool IsWallRunning() const { return MovementMode == MOVE_Custom && CustomMovementMode == WallRunMode; }
	/** World-space outward wall normal, for visual effects only. Zero outside wall-running. */
	FVector GetWallRunNormal() const { return IsWallRunning() ? WallRunNormal : FVector::ZeroVector; }

	/** A slide requires sprint input and at least this horizontal speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Slide", meta = (ClampMin = "0", Units = "cm/s"))
	float SlideMinSpeed = 750.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Slide", meta = (ClampMin = "0", Units = "cm/s"))
	float SlideEndSpeed = 250.0f;

	/** Exponential drag per second; independent of walking friction and braking. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Slide", meta = (ClampMin = "0"))
	float SlideFriction = 0.65f;

	/** Multiplier on gravity projected onto the walkable floor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Slide", meta = (ClampMin = "0"))
	float SlideSlopeGravityScale = 1.0f;

	/** Maximum input-driven turn rate; steering never adds speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Slide", meta = (ClampMin = "0", Units = "deg/s"))
	float SlideSteeringRate = 45.0f;

	/** Ground walking speed; acceleration and braking approach this cap using native physics. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Movement", meta = (ClampMin = "0", Units = "cm/s"))
	float WalkSpeed = 420.0f;

	/** Ground speed while holding sprint, never lower than WalkSpeed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Movement", meta = (ClampMin = "0", Units = "cm/s"))
	float SprintSpeed = 900.0f;

	/** Time for the ground speed cap to rise from walk to sprint. Velocity uses native acceleration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Movement", meta = (ClampMin = "0.01", Units = "s"))
	float SprintAccelerationTime = 0.55f;

	/** Time for the ground speed cap to return from sprint to walk while moving. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Movement", meta = (ClampMin = "0.01", Units = "s"))
	float SprintDecelerationTime = 0.4f;

	/** Native input acceleration while walking; reaches WalkSpeed in about 0.23 seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Movement|Walking", meta = (ClampMin = "0", Units = "cm/s^2"))
	float WalkingAcceleration = 1800.0f;

	/** Native input acceleration while sprint is requested; the existing speed-cap ramp still applies. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Movement|Sprinting", meta = (ClampMin = "0", Units = "cm/s^2"))
	float SprintingAcceleration = 2200.0f;

	/** Constant native braking deceleration, in addition to friction drag. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Movement|Walking", meta = (ClampMin = "0", Units = "cm/s^2"))
	float WalkingBrakingDeceleration = 1000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Movement|Sprinting", meta = (ClampMin = "0", Units = "cm/s^2"))
	float SprintingBrakingDeceleration = 900.0f;

	/** Native ground friction controls both directional response and braking drag. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Movement|Walking", meta = (ClampMin = "0"))
	float WalkingGroundFriction = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Movement|Sprinting", meta = (ClampMin = "0"))
	float SprintingGroundFriction = 4.0f;

	/** Extra reach beyond the capsule radius. Queries use the capsule's collision responses. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", Units = "cm"))
	float WallDetectionDistance = 35.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "1", Units = "cm/s"))
	float WallRunMinSpeed = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", ClampMax = "1"))
	float WallRunGravityScale = 0.3f;

	/** Cumulative wall-running time per airborne period; only landing restores the budget. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0.01", Units = "s"))
	float WallRunMaxDuration = 1.5f;

	/** Brief probe misses don't immediately detach at seams or changes in wall height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", ClampMax = "0.25", Units = "s"))
	float WallRunContactGraceTime = 0.1f;

	/** Prevent immediate falling/custom-mode loops without consuming an unused airborne budget. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0.01", Units = "s"))
	float WallRunReattachCooldown = 0.2f;

	/** Maximum absolute surface-normal Z; rejects floors, ceilings and steeply inclined walls. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", ClampMax = "0.3"))
	float WallRunMaxNormalZ = 0.15f;

	/** Minimum fraction of horizontal velocity traveling along the wall on entry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", ClampMax = "1"))
	float WallRunMinAlignment = 0.8f;

	/** Reject rapid descents instead of cancelling their vertical momentum on entry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", Units = "cm/s"))
	float WallRunMaxEntryFallSpeed = 500.0f;

	/** Maximum change between adjacent wall normals; sharp corners detach. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", ClampMax = "60", Units = "deg"))
	float WallRunMaxCornerAngle = 35.0f;

	/** Maximum horizontal direction change per second while following wall geometry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", Units = "deg/s"))
	float WallRunTurnRate = 120.0f;

	/** Collision-swept inward correction speed; never snaps the capsule onto the wall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", Units = "cm/s"))
	float WallRunAttractionSpeed = 100.0f;

	/** Opposing movement input slows the run without reversing it or adding speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", Units = "cm/s^2"))
	float WallRunBrakingDeceleration = 700.0f;

	/** Outward movement input above this fraction of full input detaches from the wall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0.1", ClampMax = "1"))
	float WallRunDetachInputThreshold = 0.5f;

	/** Horizontal outward velocity change; independent of mass and frame time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Jump", meta = (ClampMin = "0", Units = "cm/s"))
	float WallJumpOutwardImpulse = 650.0f;

	/** Added to current vertical velocity, preserving existing ascent/descent momentum. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Jump", meta = (ClampMin = "0", Units = "cm/s"))
	float WallJumpUpwardImpulse = 600.0f;

	/** Fraction of horizontal velocity along the wall retained at takeoff. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Jump", meta = (ClampMin = "0", ClampMax = "1"))
	float WallJumpMomentumRetention = 0.95f;

	/** Same surface requires BOTH this delay and collision-confirmed separation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Jump", meta = (ClampMin = "0.01", Units = "s"))
	float WallJumpSameWallCooldown = 0.4f;

	/** Extra capsule clearance required before the jumped surface can be used again. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Jump", meta = (ClampMin = "1", Units = "cm"))
	float WallJumpMinSeparation = 60.0f;

	/** Similar faces on the same component count as the same wall; opposite faces can chain. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Jump", meta = (ClampMin = "0", ClampMax = "60", Units = "deg"))
	float WallJumpSameSurfaceAngle = 30.0f;

	/** Coplanar faces across separate components also count as the same wall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Jump", meta = (ClampMin = "0", Units = "cm"))
	float WallJumpSurfacePlaneTolerance = 15.0f;

	/** Optional local display of horizontal speed, world velocity and movement state. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Debug")
	bool bShowMovementDebug = false;

	/** Draw wall probes/normals and report throttled rejection, entry and detachment diagnostics. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Debug")
	bool bShowWallRunDebug = false;

protected:
	virtual void UpdateCharacterStateBeforeMovement(float DeltaSeconds) override;
	virtual void UpdateCharacterStateAfterMovement(float DeltaSeconds) override;
	virtual void CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration) override;
	virtual void OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode) override;
	virtual void PhysCustom(float deltaTime, int32 Iterations) override;
	virtual void PhysFalling(float deltaTime, int32 Iterations) override;

private:
	static constexpr uint8 WallRunMode = 1;
	bool IsSuitableWall(const FHitResult& Hit) const;
	const TCHAR* GetWallRejectionReason(const FHitResult& Hit) const;
	bool TraceWall(const FVector& Direction, FHitResult& Hit) const;
	bool IsWallRunDebugEnabled() const;
	void DebugWallRun(const TCHAR* Message);
	bool TryStartWallRun();
	void PhysWallRun(float DeltaTime, int32 Iterations);
	void EndWallRun(float RemainingTime, int32 Iterations, const TCHAR* Reason);
	struct FWallJumpRestriction
	{
		TWeakObjectPtr<UPrimitiveComponent> Surface;
		FVector Normal = FVector::ZeroVector;
		FVector Point = FVector::ZeroVector;
		double UnlockTime = 0.0;
		bool bHasSeparated = false;
	};
	bool IsWallJumpSurfaceBlocked(const FHitResult& Hit) const;
	void UpdateWallJumpRestrictions();
	// Keep all recent surfaces when chaining, so jumping from B cannot erase A's lock.
	TArray<FWallJumpRestriction> WallJumpRestrictions;
	FVector WallRunNormal = FVector::ZeroVector;
	FVector WallRunDirection = FVector::ZeroVector;
	float WallRunElapsed = 0.0f;
	float WallRunContactLostTime = 0.0f;
	double WallRunReattachTime = 0.0;
	double NextWallRunDebugTime = 0.0;
	FString LastWallRunDebugMessage;

	bool bSprintRequested = false;
	bool bSlideRequested = false;
	bool bSliding = false;
	float CurrentGroundSpeed = 420.0f;
	/** Takeoff speed cap, retained until landing even if sprint is released midair. */
	float FallingSpeedLimit = 0.0f;
};
