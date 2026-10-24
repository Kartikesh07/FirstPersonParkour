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
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"

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
}

void AFPPlayerCharacter::BeginPlay()
{
	Super::BeginPlay();
	StandingCameraZ = FirstPersonCameraComponent->GetRelativeLocation().Z;
	StandingCapsuleHalfHeight = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
}

void AFPPlayerCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	FVector CameraLocation = FirstPersonCameraComponent->GetRelativeLocation();
	const float CapsuleReduction = StandingCapsuleHalfHeight - GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	const float TargetZ = StandingCameraZ + CapsuleReduction - (bIsCrouched ? SlideCameraLowering : 0.0f);
	const float Alpha = 1.0f - FMath::Exp(-FMath::Max(0.1f, SlideCameraTransitionSpeed) * DeltaSeconds);
	CameraLocation.Z = FMath::Lerp(CameraLocation.Z, TargetZ, Alpha);

	// The smoothed eye can briefly extend beyond the shortened capsule. Keep it
	// outside overhead geometry while the collision capsule passes underneath.
	const FTransform CapsuleTransform = GetCapsuleComponent()->GetComponentTransform();
	FHitResult CameraHit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(SlideCamera), false, this);
	if ((bIsCrouched || !FMath::IsNearlyEqual(CameraLocation.Z, StandingCameraZ, 0.01f))
		&& GetWorld()->SweepSingleByChannel(CameraHit, CapsuleTransform.GetLocation(),
		CapsuleTransform.TransformPosition(CameraLocation), FQuat::Identity, ECC_Camera,
		FCollisionShape::MakeSphere(6.0f), Params))
	{
		CameraLocation = CapsuleTransform.InverseTransformPosition(CameraHit.Location);
	}
	FirstPersonCameraComponent->SetRelativeLocation(CameraLocation);
}

void AFPPlayerCharacter::OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnStartCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);
	// Offset the native capsule-center shift so camera smoothing starts at its
	// previous world height; capsule collision still changes immediately.
	if (ParkourMovementComponent->IsMovingOnGround())
	{
		FirstPersonCameraComponent->SetRelativeLocation(
			FirstPersonCameraComponent->GetRelativeLocation() + FVector(0.0f, 0.0f, HalfHeightAdjust));
	}
}

void AFPPlayerCharacter::OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnEndCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);
	if (ParkourMovementComponent->IsMovingOnGround())
	{
		FirstPersonCameraComponent->SetRelativeLocation(
			FirstPersonCameraComponent->GetRelativeLocation() - FVector(0.0f, 0.0f, HalfHeightAdjust));
	}
}

void AFPPlayerCharacter::NotifyControllerChanged()
{
	// Possession changes must not leave sprint latched on the previous pawn.
	StopSprint();
	ParkourMovementComponent->StopSlide();
	RemoveDefaultMappingContext();
	Super::NotifyControllerChanged();

	// Add input mapping context when possessed by a local player controller
	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			if (DefaultMappingContext)
			{
				Subsystem->AddMappingContext(DefaultMappingContext, DefaultMappingPriority);
				InputSubsystem = Subsystem;
			}
		}
	}
}

void AFPPlayerCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopSprint();
	ParkourMovementComponent->StopSlide();
	RemoveDefaultMappingContext();
	Super::EndPlay(EndPlayReason);
}

void AFPPlayerCharacter::RemoveDefaultMappingContext()
{
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = InputSubsystem.Get())
	{
		Subsystem->RemoveMappingContext(DefaultMappingContext);
	}
	InputSubsystem.Reset();
}

void AFPPlayerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		if (SprintAction)
		{
			EnhancedInputComponent->BindAction(SprintAction, ETriggerEvent::Started, this, &AFPPlayerCharacter::StartSprint);
			EnhancedInputComponent->BindAction(SprintAction, ETriggerEvent::Completed, this, &AFPPlayerCharacter::StopSprint);
			EnhancedInputComponent->BindAction(SprintAction, ETriggerEvent::Canceled, this, &AFPPlayerCharacter::StopSprint);
		}
		if (SlideAction)
		{
			EnhancedInputComponent->BindAction(SlideAction, ETriggerEvent::Started, this, &AFPPlayerCharacter::StartSlide);
		}
		// Jump
		if (JumpAction)
		{
			EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &AFPPlayerCharacter::StartJump);
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

void AFPPlayerCharacter::StartSlide()
{
	ParkourMovementComponent->RequestSlide();
}

void AFPPlayerCharacter::StartJump()
{
	// Native UnCrouch performs the clearance query; blocked jumps remain low.
	if (ParkourMovementComponent->PrepareSlideJump())
	{
		Jump();
	}
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
