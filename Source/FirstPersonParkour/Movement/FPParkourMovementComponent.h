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
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	void SetSprintRequested(bool bRequested) { bSprintRequested = bRequested; }
	bool IsSprinting() const { return bSprintRequested && IsMovingOnGround() && !IsCrouching(); }

	/** Ground walking speed; acceleration and braking approach this cap using native physics. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Movement", meta = (ClampMin = "0", Units = "cm/s"))
	float WalkSpeed = 600.0f;

	/** Ground speed while holding sprint, never lower than WalkSpeed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Movement", meta = (ClampMin = "0", Units = "cm/s"))
	float SprintSpeed = 1000.0f;

	/** Optional local display of horizontal speed, world velocity and movement state. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parkour|Debug")
	bool bShowMovementDebug = false;

protected:
	virtual void OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode) override;
	virtual void PhysCustom(float deltaTime, int32 Iterations) override;

private:
	bool bSprintRequested = false;
	/** Takeoff speed cap, retained until landing even if sprint is released midair. */
	float FallingSpeedLimit = 0.0f;
};
