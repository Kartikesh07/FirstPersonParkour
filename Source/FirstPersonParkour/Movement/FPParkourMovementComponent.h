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

	/** Optional local display of horizontal speed, world velocity and movement state. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Debug")
	bool bShowMovementDebug = false;

protected:
	virtual void UpdateCharacterStateBeforeMovement(float DeltaSeconds) override;
	virtual void UpdateCharacterStateAfterMovement(float DeltaSeconds) override;
	virtual void CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration) override;
	virtual void OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode) override;
	virtual void PhysCustom(float deltaTime, int32 Iterations) override;

private:
	bool bSprintRequested = false;
	bool bSlideRequested = false;
	bool bSliding = false;
	float CurrentGroundSpeed = 420.0f;
	/** Takeoff speed cap, retained until landing even if sprint is released midair. */
	float FallingSpeedLimit = 0.0f;
};
