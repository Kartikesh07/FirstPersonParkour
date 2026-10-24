// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "FPGameMode.generated.h"

/**
 * Base Game Mode for First Person Parkour.
 * Automatically configures AFPPlayerCharacter as the default pawn.
 */
UCLASS()
class FIRSTPERSONPARKOUR_API AFPGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AFPGameMode();
};
