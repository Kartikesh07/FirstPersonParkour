// Copyright Epic Games, Inc. All Rights Reserved.

#include "Movement/FPParkourMovementComponent.h"

UFPParkourMovementComponent::UFPParkourMovementComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UFPParkourMovementComponent::PhysCustom(float deltaTime, int32 Iterations)
{
	Super::PhysCustom(deltaTime, Iterations);

	// Custom parkour physics modes (sprint, slide, wall-run, grapple) will be routed here.
}
