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
	/** Camera trace and capsule-clearance validation; never changes position or entry velocity. */
	bool TryStartGrapple(const FVector& ViewLocation, const FVector& ViewDirection);
	void StopGrapple(const TCHAR* Reason = TEXT("input released/canceled"));
	bool IsGrappling() const { return bGrappling; }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Grapple", meta = (ClampMin = "1", Units = "cm"))
	float GrappleRange = 3000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Grapple", meta = (ClampMin = "0", Units = "cm/s^2"))
	float GrapplePullAcceleration = 2400.0f;

	/** Cap on grapple-generated speed. Faster inherited momentum is never truncated. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Grapple", meta = (ClampMin = "1", Units = "cm/s"))
	float GrappleMaxPullSpeed = 1800.0f;

	/** Minimum attachment distance; never detaches an active swing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Grapple", meta = (ClampMin = "1", Units = "cm"))
	float GrappleArrivalDistance = 150.0f;

	/** Reel target, capped at attachment length. Ground clearance may request a shorter target. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Grapple|Swing", meta = (DisplayName = "Target Rope Length", ClampMin = "100", Units = "cm"))
	float GrappleRopeLength = 600.0f;

	/** Explicit slack added to actual attachment distance; zero starts taut. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Grapple|Swing", meta = (ClampMin = "0", Units = "cm"))
	float GrappleInitialSlack = 0.0f;

	/** Maximum rope take-up and pull-driven inward speed. Does not truncate inherited inward momentum. Zero disables reeling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Grapple|Swing", meta = (ClampMin = "0", Units = "cm/s"))
	float GrappleReelSpeed = 600.0f;

	/** Requested capsule-bottom clearance at the bottom of an overhead swing. Requires sufficient pull force and time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Grapple|Swing", meta = (ClampMin = "0", Units = "cm"))
	float GrappleMinSwingClearance = 100.0f;

	/** Tangential input acceleration before SteeringInfluence; applied as a force, never an impulse. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Grapple|Swing", meta = (ClampMin = "0", Units = "cm/s^2"))
	float SwingAcceleration = 800.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Grapple|Swing", meta = (ClampMin = "0", ClampMax = "1"))
	float SwingSteeringInfluence = 0.35f;

	/** Exponential tangential drag per second. Zero permits an undamped pendulum. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Grapple|Swing", meta = (ClampMin = "0"))
	float SwingDamping = 0.02f;

	/** Ceiling for generated speed; faster inherited momentum decays smoothly rather than snapping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Grapple|Swing", meta = (ClampMin = "1", Units = "cm/s"))
	float MaxSwingSpeed = 2400.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Grapple")
	bool bShowGrappleDebug = true;

	/** Rope/target distance, state and release reason. Also enabled with parkour.DebugGrapple 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Debug")
	bool bShowGrappleStateDebug = false;
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "500", Units = "cm/s"))
	float WallRunMinSpeed = 500.0f;

	/** Excess tangential speed decays toward this value; slower players receive no boost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "500", Units = "cm/s"))
	float WallRunComfortSpeed = 1100.0f;

	/** Exponential excess-speed decay per second; 2.5 removes about 92 percent in one second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0"))
	float WallRunHighSpeedBrakingRate = 2.5f;

	/** Brief momentum-based entry after release, independent of camera-facing direction. Never overrides collision locks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", Units = "s"))
	float WallRunGrappleHandoffWindow = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", ClampMax = "1"))
	float WallRunGravityScale = 0.3f;

	/** Additional upward braking acceleration during entry; only removes excess descent, never creates ascent. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run|Vertical", meta = (ClampMin = "0", Units = "cm/s^2"))
	float WallRunVerticalStabilizationStrength = 4000.0f;

	/** Terminal downward drift under wall-run gravity/vertical drag. Kept positive to avoid hovering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run|Vertical", meta = (ClampMin = "1", Units = "cm/s"))
	float WallRunMaxControlledDescentSpeed = 100.0f;

	/** Entry-assist window in the current run's simulation time; ordinary reattachment does not restart it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run|Vertical", meta = (ClampMin = "0", Units = "s"))
	float WallRunVerticalStabilizationDuration = 0.25f;

	/** Time allowed per run. A wall jump to a different valid surface starts a new run;
	 *  same-wall reattachments and detachments without a wall jump retain elapsed time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0.01", Units = "s"))
	float WallRunMaxDuration = 1.5f;

	/** Brief probe misses don't immediately detach at seams or changes in wall height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", ClampMax = "0.25", Units = "s"))
	float WallRunContactGraceTime = 0.1f;

	/** Prevent immediate falling/custom-mode loops; detachment does not refresh run duration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0.01", Units = "s"))
	float WallRunReattachCooldown = 0.2f;

	/** Maximum absolute surface-normal Z; rejects floors, ceilings and steeply inclined walls. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", ClampMax = "0.3"))
	float WallRunMaxNormalZ = 0.15f;

	/** Minimum fraction of horizontal velocity traveling along the wall on entry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0.5", ClampMax = "1"))
	float WallRunMinAlignment = 0.65f;

	/** Reject rapid descents instead of cancelling their vertical momentum on entry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", Units = "cm/s"))
	float WallRunMaxEntryFallSpeed = 500.0f;

	/** Fast entries may descend at this fraction of tangential speed, bounded by the absolute high-speed limit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", ClampMax = "1"))
	float WallRunMaxEntryDescentRatio = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Run", meta = (ClampMin = "0", Units = "cm/s"))
	float WallRunMaxHighSpeedEntryFallSpeed = 1200.0f;

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

	/** Added to vertical velocity, then bounded below by the minimum successful upward takeoff speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Jump", meta = (ClampMin = "0", Units = "cm/s"))
	float WallJumpUpwardImpulse = 600.0f;

	/** Descending into a wall must not cancel the jump. Successful takeoff also has to pass native capsule clearance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Wall Jump", meta = (ClampMin = "1", Units = "cm/s"))
	float WallJumpMinUpwardTakeoffSpeed = 600.0f;

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
	virtual FVector NewFallVelocity(const FVector& InitialVelocity, const FVector& Gravity, float DeltaTime) const override;
	virtual void HandleImpact(const FHitResult& Hit, float TimeSlice, const FVector& MoveDelta) override;

private:
	bool IsGrapplePathClear(bool bCheckCapsulePath = false) const;
	bool IsGrappleAnchorValid() const;
	void UpdateGrappleReelTarget();
	bool IsGrappleStateDebugEnabled() const;
	FString GetGrappleDebugStatus(const TCHAR* State, const TCHAR* Reason) const;
	void PhysGrapple(float DeltaTime, int32 Iterations);
	/** Drift freely while slack, then follow a great-circle arc at the rope limit. */
	FVector ComputeGrappleDrift(const FVector& Location, FVector& DriftVelocity, float DeltaTime) const;
	FVector LimitGrappleDelta(const FVector& Location, const FVector& Delta) const;
	bool bGrappling = false;
	float ActiveRopeLength = 0.0f;
	float DesiredRopeLength = 0.0f;
	bool bGrappleClearanceLimited = false;
	FString LastGrappleReleaseStatus = TEXT("Grapple: detached | release: none");
	FVector GrappleAnchor = FVector::ZeroVector;
	FVector GrappleNormal = FVector::ZeroVector;
	TWeakObjectPtr<UPrimitiveComponent> GrappleSurface;
	FTransform GrappleSurfaceTransform;

	static constexpr uint8 WallRunMode = 1;
	bool IsSuitableWall(const FHitResult& Hit) const;
	const TCHAR* GetWallRejectionReason(const FHitResult& Hit) const;
	bool TraceWall(const FVector& Direction, FHitResult& Hit) const;
	bool IsWallRunDebugEnabled() const;
	void DebugWallRun(const TCHAR* Message);
	bool TryStartWallRun(const FHitResult* ImpactHit = nullptr, const FVector* ApproachVelocity = nullptr);
	bool IsGrappleWallRunHandoffActive() const;
	// Native falling impacts retain pre-deflection approach data for the next bounded slice.
	bool bCaptureFallingWallImpact = false;
	FHitResult FallingWallImpact;
	FVector FallingWallApproachVelocity = FVector::ZeroVector;
	bool bGrappleWallRunHandoffPending = false;
	double GrappleWallRunReleaseTime = -1.0;
	FVector GrappleWallRunReleaseVelocity = FVector::ZeroVector;
	float WallRunEntryAlignment = 0.0f;
	float WallRunEntryTangentialSpeed = 0.0f;
	float WallRunEntryFallSpeedLimit = 500.0f;
	float WallRunHighSpeedBraking = 0.0f;
	float WallRunEntryVerticalVelocity = 0.0f;
	bool bWallRunVerticalStabilizing = false;
	FVector LastWallJumpTakeoffVelocity = FVector::ZeroVector;
	void ApplyWallRunVerticalControl(float PreviousVerticalVelocity, float GravityZ, float DeltaTime, bool bHasWallContact);
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
	bool MatchesWallSurface(const FHitResult& Hit, const FWallJumpRestriction& Surface) const;
	bool IsWallJumpSurfaceBlocked(const FHitResult& Hit) const;
	void UpdateWallJumpRestrictions();
	// Keep all recent surfaces when chaining, so jumping from B cannot erase A's lock.
	TArray<FWallJumpRestriction> WallJumpRestrictions;
	// Independent of expiring collision locks: unlocking the same wall cannot grant a new run.
	FWallJumpRestriction WallJumpRunSource;
	bool bWallJumpRunPending = false;
	uint32 WallRunNumber = 0;
	FVector WallRunNormal = FVector::ZeroVector;
	FVector WallRunDirection = FVector::ZeroVector;
	/** Simulation time in the current run, retained through ordinary reattachments. */
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
