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

protected:
	virtual void PhysCustom(float deltaTime, int32 Iterations) override;
};
