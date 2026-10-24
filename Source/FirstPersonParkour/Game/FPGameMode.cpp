// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/FPGameMode.h"
#include "Character/FPPlayerCharacter.h"

AFPGameMode::AFPGameMode()
{
	DefaultPawnClass = AFPPlayerCharacter::StaticClass();
}
