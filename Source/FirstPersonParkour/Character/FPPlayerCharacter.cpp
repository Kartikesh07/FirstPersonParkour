// Copyright Epic Games, Inc. All Rights Reserved.

#include "Character/FPPlayerCharacter.h"
#include "Movement/FPParkourMovementComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputCoreTypes.h"
#include "GameFramework/PlayerController.h"

AFPPlayerCharacter::AFPPlayerCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UFPParkourMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;

	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(40.0f, 96.0f);

	// First person camera drives pitch; pawn rotates with controller yaw
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = true;
	bUseControllerRotationRoll = false;

	// Cache custom movement component and configure rotation behavior
	ParkourMovementComponent = Cast<UFPParkourMovementComponent>(GetCharacterMovement());
	if (ParkourMovementComponent)
	{
		ParkourMovementComponent->bOrientRotationToMovement = false;
	}

	// Create and attach first-person camera to the capsule at eye height
	FirstPersonCameraComponent = ObjectInitializer.CreateDefaultSubobject<UCameraComponent>(this, TEXT("FirstPersonCamera"));
	FirstPersonCameraComponent->SetupAttachment(GetCapsuleComponent());
	FirstPersonCameraComponent->SetRelativeLocation(FVector(0.0f, 0.0f, 64.0f));
	FirstPersonCameraComponent->bUsePawnControlRotation = true;

	SprintAction = ObjectInitializer.CreateDefaultSubobject<UInputAction>(this, TEXT("SprintAction"));
	SprintAction->ValueType = EInputActionValueType::Boolean;
	SprintMappingContext = ObjectInitializer.CreateDefaultSubobject<UInputMappingContext>(this, TEXT("SprintMappingContext"));
}

void AFPPlayerCharacter::NotifyControllerChanged()
{
	// Possession changes must not leave sprint latched on the previous pawn.
	StopSprint();
	RemoveSprintMappingContext();
	Super::NotifyControllerChanged();

	// Add input mapping context when possessed by a local player controller
	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			if (DefaultMappingContext)
			{
				Subsystem->AddMappingContext(DefaultMappingContext, DefaultMappingPriority);
			}
			// Map after archetype initialization so the binding references this pawn's action.
			SprintMappingContext->UnmapAll();
			SprintMappingContext->MapKey(SprintAction, EKeys::LeftShift);
			Subsystem->AddMappingContext(SprintMappingContext, DefaultMappingPriority);
			SprintInputSubsystem = Subsystem;
		}
	}
}

void AFPPlayerCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopSprint();
	RemoveSprintMappingContext();
	Super::EndPlay(EndPlayReason);
}

void AFPPlayerCharacter::RemoveSprintMappingContext()
{
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = SprintInputSubsystem.Get())
	{
		Subsystem->RemoveMappingContext(SprintMappingContext);
	}
	SprintInputSubsystem.Reset();
}

void AFPPlayerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		EnhancedInputComponent->BindAction(SprintAction, ETriggerEvent::Started, this, &AFPPlayerCharacter::StartSprint);
		EnhancedInputComponent->BindAction(SprintAction, ETriggerEvent::Completed, this, &AFPPlayerCharacter::StopSprint);
		EnhancedInputComponent->BindAction(SprintAction, ETriggerEvent::Canceled, this, &AFPPlayerCharacter::StopSprint);
		// Jump
		if (JumpAction)
		{
			EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
			EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);
		}

		// Move (WASD)
		if (MoveAction)
		{
			EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AFPPlayerCharacter::Move);
		}

		// Look (Mouse)
		if (LookAction)
		{
			EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AFPPlayerCharacter::Look);
		}
	}
}

void AFPPlayerCharacter::StartSprint()
{
	ParkourMovementComponent->SetSprintRequested(true);
}

void AFPPlayerCharacter::StopSprint()
{
	ParkourMovementComponent->SetSprintRequested(false);
}

void AFPPlayerCharacter::Move(const FInputActionValue& Value)
{
	const FVector2D MovementVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// Get camera-relative yaw orientation
		const FRotator ControlRotation = Controller->GetControlRotation();
		const FRotator YawRotation(0.0f, ControlRotation.Yaw, 0.0f);

		// Determine forward and right directions relative to camera/controller yaw
		const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
		const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		// Apply movement input (Y: Forward/Backward, X: Right/Left)
		AddMovementInput(ForwardDirection, MovementVector.Y);
		AddMovementInput(RightDirection, MovementVector.X);
	}
}

void AFPPlayerCharacter::Look(const FInputActionValue& Value)
{
	const FVector2D LookAxisVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// Apply yaw and pitch mouse input
		AddControllerYawInput(LookAxisVector.X);
		AddControllerPitchInput(LookAxisVector.Y);
	}
}
