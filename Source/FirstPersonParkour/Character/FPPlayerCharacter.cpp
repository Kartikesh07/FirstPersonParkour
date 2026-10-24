// Copyright Epic Games, Inc. All Rights Reserved.

#include "Character/FPPlayerCharacter.h"
#include "Character/FPGrappleCableComponent.h"
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
#include "HAL/IConsoleManager.h"
#include "UObject/ConstructorHelpers.h"

static TAutoConsoleVariable<int32> CVarParkourTransitionDebug(
	TEXT("parkour.DebugTransitions"), 0, TEXT("Log accepted, consumed, expired and rejected jump transitions: 0 = off, 1 = on."));
DEFINE_LOG_CATEGORY_STATIC(LogParkourTransitions, Log, All);

AFPPlayerCharacter::AFPPlayerCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UFPParkourMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;
	static ConstructorHelpers::FObjectFinder<UInputAction> GrappleInput(TEXT("/Game/Input/IA_Grapple.IA_Grapple"));
	GrappleAction = GrappleInput.Object;

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

	GrappleCableComponent = ObjectInitializer.CreateDefaultSubobject<UFPGrappleCableComponent>(this, TEXT("GrappleCable"));
	GrappleCableComponent->SetupAttachment(GetCapsuleComponent());
	GrappleCableComponent->AddTickPrerequisiteComponent(ParkourMovementComponent);
}

void AFPPlayerCharacter::BeginPlay()
{
	Super::BeginPlay();
	StandingCameraLocation = FirstPersonCameraComponent->GetRelativeLocation();
	SmoothedCameraZ = StandingCameraLocation.Z;
	StandingCapsuleHalfHeight = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	ResetCameraFeedback();
}

void AFPPlayerCharacter::NotifyControllerChanged()
{
	// Possession changes must not leave sprint latched on the previous pawn.
	StopSprint();
	ParkourMovementComponent->StopGrapple(TEXT("controller changed"));
	ResetCameraFeedback();
	GrappleCableComponent->HideGrappleCable();
	ParkourMovementComponent->StopSlide();
	EndJump();
	ClearBufferedJump(TEXT("rejected: controller changed"));
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
	ParkourMovementComponent->StopGrapple(TEXT("end play"));
	GrappleCableComponent->HideGrappleCable();
	StopSprint();
	EndJump();
	ClearBufferedJump(TEXT("rejected: end play"));
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
		if (GrappleAction)
		{
			EnhancedInputComponent->BindAction(GrappleAction, ETriggerEvent::Started, this, &AFPPlayerCharacter::StartGrapple);
			EnhancedInputComponent->BindAction(GrappleAction, ETriggerEvent::Completed, this, &AFPPlayerCharacter::EndGrapple);
			EnhancedInputComponent->BindAction(GrappleAction, ETriggerEvent::Canceled, this, &AFPPlayerCharacter::EndGrapple);
		}
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
			EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &AFPPlayerCharacter::EndJump);
			EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Canceled, this, &AFPPlayerCharacter::CancelJump);
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

void AFPPlayerCharacter::StartGrapple()
{
	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		FVector ViewLocation;
		FRotator ViewRotation;
		// Preserve the collision-safe rendered origin, but cosmetic pitch/roll must never steer a shot.
		PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);
		ViewRotation = PlayerController->GetControlRotation();
		if (ParkourMovementComponent->TryStartGrapple(ViewLocation, ViewRotation.Vector()))
		{
			ClearBufferedJump(TEXT("rejected: grapple accepted movement ownership"));
		}
	}
}

void AFPPlayerCharacter::EndGrapple()
{
	ParkourMovementComponent->StopGrapple();
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
	if (bJumpInputHeld)
	{
		DebugJumpTransition(TEXT("rejected: duplicate press while held"));
		return;
	}
	bJumpInputHeld = true;
	if (!Controller || ParkourMovementComponent->IsGrappling())
	{
		DebugJumpTransition(TEXT("rejected: no controller or active grapple"));
		return;
	}
	StopJumping();
	bJumpBuffered = true;
	bBufferedWallJumpOnly = ParkourMovementComponent->IsWallRunning();
	JumpBufferExpiresAt = GetWorld()->GetTimeSeconds() + FMath::Clamp(JumpBufferDuration, 0.0f, 0.5f);
	LastJumpRejection.Reset();
	DebugJumpTransition(TEXT("accepted: jump press buffered"));
	CheckJumpInput(0.0f);
}

void AFPPlayerCharacter::CheckJumpInput(float DeltaTime)
{
	if (!bJumpBuffered)
	{
		Super::CheckJumpInput(DeltaTime);
		return;
	}
	if (!Controller || ParkourMovementComponent->IsGrappling()
		|| (!ParkourMovementComponent->IsMovingOnGround() && !ParkourMovementComponent->IsFalling()
			&& !ParkourMovementComponent->IsWallRunning()))
	{
		ClearBufferedJump(TEXT("rejected: movement ownership changed"));
		return;
	}
	if (GetWorld()->GetTimeSeconds() > JumpBufferExpiresAt)
	{
		ClearBufferedJump(TEXT("expired: jump buffer"));
		return;
	}
	const TCHAR* Rejection = TEXT("rejected: waiting for landing or valid wall attachment");
	if (ParkourMovementComponent->IsWallRunning())
	{
		if (ParkourMovementComponent->TryWallJump())
		{
			ClearBufferedJump(TEXT("consumed: wall jump"));
			return;
		}
		Rejection = TEXT("rejected: wall jump contact, clearance or surface lock; buffer retained");
	}
	else if (ParkourMovementComponent->IsMovingOnGround() && !bBufferedWallJumpOnly)
	{
		// Clear pending slide input and use native standing clearance before the native jump.
		if (ParkourMovementComponent->PrepareSlideJump())
		{
			Jump();
			Super::CheckJumpInput(DeltaTime);
			if (bWasJumping)
			{
				ClearBufferedJump(TEXT("consumed: ground/slide jump"));
				// A released tap may still consume its buffer, without latching jump hold.
				if (!bJumpInputHeld)
				{
					StopJumping();
				}
				return;
			}
			StopJumping();
		}
		Rejection = TEXT("rejected: ground jump clearance or native jump eligibility; buffer retained");
	}
	else if (ParkourMovementComponent->IsMovingOnGround() && bBufferedWallJumpOnly)
	{
		ClearBufferedJump(TEXT("rejected: wall-jump press cannot become a landing jump"));
		return;
	}
	if (LastJumpRejection != Rejection)
	{
		DebugJumpTransition(Rejection);
		LastJumpRejection = Rejection;
	}
}

void AFPPlayerCharacter::ClearBufferedJump(const TCHAR* Reason)
{
	if (bJumpBuffered)
	{
		DebugJumpTransition(Reason);
	}
	bJumpBuffered = false;
	bBufferedWallJumpOnly = false;
	JumpBufferExpiresAt = 0.0;
	LastJumpRejection.Reset();
}

void AFPPlayerCharacter::DebugJumpTransition(const TCHAR* Event) const
{
	if (bShowTransitionDebug || CVarParkourTransitionDebug.GetValueOnGameThread() != 0)
	{
		UE_LOG(LogParkourTransitions, Log, TEXT("%s: %s | mode=%s | velocity=%s"),
			*GetName(), Event, *ParkourMovementComponent->GetMovementName(), *ParkourMovementComponent->Velocity.ToCompactString());
	}
}

void AFPPlayerCharacter::CancelJump()
{
	EndJump();
	ClearBufferedJump(TEXT("rejected: input canceled"));
}

void AFPPlayerCharacter::EndJump()
{
	bJumpInputHeld = false;
	StopJumping();
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
