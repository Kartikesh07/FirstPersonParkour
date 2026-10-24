#include "LevelDesign/PFParkourCourseBuilder.h"

#include "Character/FPPlayerCharacter.h"
#include "Movement/FPParkourMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "UObject/ConstructorHelpers.h"

APFParkourCourseBuilder::APFParkourCourseBuilder()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("CourseOrigin"));
	RootComponent->SetMobility(EComponentMobility::Static);
	CharacterProfile = AFPPlayerCharacter::StaticClass();
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterial> Material(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	BaseMaterial = Material.Object;

	const auto MakeBatch = [&](const TCHAR* Name)
	{
		UInstancedStaticMeshComponent* Batch = CreateDefaultSubobject<UInstancedStaticMeshComponent>(Name);
		Batch->SetupAttachment(RootComponent);
		Batch->SetMobility(EComponentMobility::Static);
		Batch->SetStaticMesh(Cube.Object);
		Batch->SetMaterial(0, BaseMaterial);
		// The existing grapple traces the capsule's channel (Pawn), not Visibility or a tag.
		Batch->SetCollisionProfileName(TEXT("BlockAll"));
		Batch->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Batch->SetGenerateOverlapEvents(false);
		Batch->SetCanEverAffectNavigation(false);
		return Batch;
	};
	Decks = MakeBatch(TEXT("PlayableDecks"));
	Walls = MakeBatch(TEXT("WallRunSurfaces"));
	Tunnels = MakeBatch(TEXT("SlideTunnel"));
	Targets = MakeBatch(TEXT("GrappleTargets"));
	Hazards = MakeBatch(TEXT("GapMarkers"));
	StartZone = MakeBatch(TEXT("StartZone"));
	FinishZone = MakeBatch(TEXT("FinishZone"));
}

void APFParkourCourseBuilder::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RegenerateCourse();
}

void APFParkourCourseBuilder::BeginPlay()
{
	Super::BeginPlay();
	// Restore transient color instances in a loaded level as well as editor/PIE copies.
	RegenerateCourse();
}

void APFParkourCourseBuilder::Box(UInstancedStaticMeshComponent* Mesh, const FVector& Center, const FVector& Size)
{
	// Engine cube is 100 cm on each side. Instances remain in actor-local space.
	Mesh->AddInstance(FTransform(FQuat::Identity, Center, Size / 100.0f));
}

void APFParkourCourseBuilder::Deck(float X, float Length, UInstancedStaticMeshComponent* Mesh)
{
	if (Length > 0.0f)
	{
		Box(Mesh ? Mesh : Decks.Get(), FVector(X + Length * 0.5f, 0, -Thickness * 0.5f),
			FVector(Length, Width, Thickness));
	}
}

void APFParkourCourseBuilder::MarkGap(float X, float Length)
{
	// Low red lips are on solid decks, leaving the actual gap entirely open.
	for (const float LipX : { X - 8.0f, X + Length + 8.0f })
	{
		Box(Hazards, FVector(LipX, 0, 1), FVector(16, Width, 2));
	}
	if (bRecoveryDecks)
	{
		Box(Hazards, FVector(X + Length * 0.5f, 0, -640), FVector(Length, Width + 400, 80));
	}
}

float APFParkourCourseBuilder::BuildWalls(float X, float Gap, float Separation, float Height)
{
	MarkGap(X, Gap);
	// Overlapping X spans on opposite sides allow transfers without a frontal end face.
	// Same-side spans have a real break; the player must jump to a different wall.
	const float WallThickness = 60.0f;
	const float Y = (Separation + WallThickness) * 0.5f;
	const auto Wall = [&](float Begin, float End, float Side)
	{
		Box(Walls, FVector(X + (Begin + End) * 0.5f, Side * Y, Height * 0.5f - 150),
			FVector(End - Begin, WallThickness, Height + 300));
	};
	Wall(-250, Gap * 0.44f, -1);
	Wall(Gap * 0.20f, Gap * 0.76f, 1);
	Wall(Gap * 0.60f, Gap + 250, -1);
	Deck(X + Gap, 1000);
	// A blue approach strip identifies the first wall's capsule lane (within probe reach).
	Box(Walls, FVector(X - 300, -Separation * 0.5f + WallApproachOffset, 1), FVector(450, 20, 2));
	return X + Gap + 1000;
}

float APFParkourCourseBuilder::BuildGrapple(float X, float Gap, float Height)
{
	MarkGap(X, Gap);
	// Broad, unobstructed underside: no posts across the aiming or capsule sweep path.
	Box(Targets, FVector(X + Gap * 0.5f, 0, Height + 50), FVector(400, Width * 0.75f, 100));
	const float Landing = FMath::Clamp(GrappleLandingLength, 1000.0f, 2400.0f);
	Deck(X + Gap, Landing);
	return X + Gap + Landing;
}

void APFParkourCourseBuilder::RegenerateCourse()
{
	UInstancedStaticMeshComponent* Batches[] = { Decks, Walls, Tunnels, Targets, Hazards, StartZone, FinishZone };
	const FLinearColor Colors[] = {
		FLinearColor(0.55f, 0.60f, 0.65f), FLinearColor(0.06f, 0.28f, 0.8f),
		FLinearColor(1.0f, 0.42f, 0.04f), FLinearColor(0.02f, 0.85f, 0.95f),
		FLinearColor(0.8f, 0.025f, 0.025f), FLinearColor(0.08f, 0.75f, 0.12f),
		FLinearColor(0.65f, 0.08f, 0.85f)
	};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Batches); ++Index)
	{
		Batches[Index]->ClearInstances();
		// Reuse existing MIDs instead of accumulating construction-created objects.
		UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Batches[Index]->GetMaterial(0));
		if (!MID && BaseMaterial)
		{
			MID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
			Batches[Index]->SetMaterial(0, MID);
		}
		if (MID)
		{
			MID->SetVectorParameterValue(TEXT("Color"), Colors[Index]);
		}
	}
	GeneratedLength = 0;
	DesignReport = TEXT("Generation disabled; all instances cleared.");
	if (!bGenerateCourse)
	{
		return;
	}

	const AFPPlayerCharacter* Character = CharacterProfile
		? CharacterProfile->GetDefaultObject<AFPPlayerCharacter>() : GetDefault<AFPPlayerCharacter>();
	const UFPParkourMovementComponent* Move = Cast<UFPParkourMovementComponent>(Character->GetCharacterMovement());
	if (!Move || !GetWorld())
	{
		DesignReport = TEXT("Cannot resolve a parkour character profile or world.");
		return;
	}
	const float Radius = Character->GetCapsuleComponent()->GetUnscaledCapsuleRadius();
	const float HalfHeight = Character->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	WallApproachOffset = Radius + FMath::Max(2.0f, Move->WallDetectionDistance * 0.35f);
	const float CrouchHalf = FMath::Max(Radius, Move->GetCrouchedHalfHeight());
	const float Gravity = FMath::Max(1.0f, FMath::Abs(GetWorld()->GetGravityZ() * Move->GravityScale));
	const float Sprint = FMath::Max(Move->WalkSpeed, Move->SprintSpeed);
	const float Flight = 2.0f * FMath::Max(1.0f, Move->JumpZVelocity) / Gravity;
	const float JumpRange = Sprint * Flight;
	const float Apex = FMath::Square(Move->JumpZVelocity) / (2.0f * Gravity);
	const float SafeRange = FMath::Max(100.0f, JumpRange - 2.0f * Radius);
	const float Runway = FMath::Max(FMath::Clamp(RunwayLength, 800.0f, 5000.0f),
		Sprint * (Move->SprintAccelerationTime + Sprint / FMath::Max(1.0f, Move->SprintingAcceleration) + 0.4f));
	Width = FMath::Max(FMath::Clamp(PlatformWidth, 400.0f, 2000.0f), Radius * 8.0f);
	Thickness = FMath::Clamp(PlatformThickness, 20.0f, 200.0f);
	const float Spacing = FMath::Clamp(SectionSpacing, 0.0f, 3000.0f);
	const float PadLength = FMath::Max(FMath::Clamp(JumpPlatformLength, 400.0f, 2000.0f), Sprint * 0.65f);
	const float Clearance = FMath::Clamp(2.0f * CrouchHalf + TunnelClearanceMargin,
		2.0f * CrouchHalf + 4.0f, FMath::Max(2.0f * CrouchHalf + 4.0f, HalfHeight * 2.0f - 20.0f));
	// Reserve >50% of the flat-slide stopping distance for entry, exit and the jump.
	const float SlideDistance = (Sprint - Move->SlideEndSpeed) / FMath::Max(0.01f, Move->SlideFriction);
	const float Tunnel = FMath::Clamp(TunnelLength, 100.0f, FMath::Max(100.0f, SlideDistance * 0.35f));
	const float Separation = FMath::Clamp(WallSeparation, Radius * 2.0f + 100.0f,
		Radius * 2.0f + FMath::Max(100.0f, Move->WallJumpOutwardImpulse * 0.55f));
	const float Height = FMath::Max(FMath::Clamp(WallHeight, 400.0f, 1600.0f), HalfHeight * 4.0f);
	// Each individual wall segment stays well inside one wall-run's time budget.
	const float WallGap = FMath::Min(JumpRange * FMath::Clamp(WallGapJumpMultiple, 1.6f, 2.2f),
		Sprint * Move->WallRunMaxDuration * 1.2f);
	const float AnchorHeight = FMath::Max(FMath::Clamp(GrappleTargetHeight, 800.0f, 1800.0f),
		Move->GrappleRopeLength + HalfHeight + Move->GrappleMinSwingClearance + 150.0f);
	const float SwingGap = FMath::Min(FMath::Clamp(GrappleGap, 1000.0f, 2200.0f), Move->GrappleRange * 0.55f);
	const float AttachDistance = FVector(SwingGap * 0.5f, 0, AnchorHeight - HalfHeight).Size();
	const float ReelTime = FMath::Max(0.0f, AttachDistance + Move->GrappleInitialSlack - Move->GrappleRopeLength)
		/ FMath::Max(1.0f, Move->GrappleReelSpeed);

	float X = 0;
	Deck(X, 800, StartZone);
	X += 800;
	const auto Connector = [&]()
	{
		Deck(X, Spacing);
		X += Spacing;
	};
	if (bSprintSection)
	{
		Deck(X, Runway);
		X += Runway;
		for (int32 Index = 0; Index < FMath::Clamp(JumpPlatformCount, 1, 6); ++Index)
		{
			const float Fraction = FMath::Clamp(FirstJumpFraction + Index * JumpFractionIncrement, 0.2f, 0.55f);
			const float Gap = SafeRange * Fraction;
			MarkGap(X, Gap);
			X += Gap;
			Deck(X, PadLength);
			X += PadLength;
		}
		Connector();
	}
	if (bSlideSection)
	{
		Deck(X, Runway);
		X += Runway;
		// A full-width roof and outside rails prevent walking around within the lane.
		Deck(X, Tunnel + 800);
		Box(Tunnels, FVector(X + Tunnel * 0.5f, 0, Clearance + 40), FVector(Tunnel, Width + 80, 80));
		for (const float Side : { -1.0f, 1.0f })
		{
			Box(Tunnels, FVector(X + Tunnel * 0.5f, Side * (Width + 40) * 0.5f, Clearance * 0.5f),
				FVector(Tunnel, 40, Clearance));
		}
		// Enough standing-capsule clearance beyond the roof before a jump can be accepted.
		const float ObstacleHeight = FMath::Min(Apex * 0.70f, Move->MaxStepHeight + 18.0f);
		Box(Tunnels, FVector(X + Tunnel + 260, 0, ObstacleHeight * 0.5f),
			FVector(70, Width, ObstacleHeight));
		X += Tunnel + 800;
		Connector();
	}
	if (bWallRunSection)
	{
		Deck(X, Runway);
		X = BuildWalls(X + Runway, WallGap, Separation, Height);
		Connector();
	}
	if (bGrappleSection)
	{
		Deck(X, Runway);
		X = BuildGrapple(X + Runway, SwingGap, AnchorHeight);
		Connector();
	}
	if (bFinalSection)
	{
		Deck(X, Runway);
		X += Runway;
		const float Gap = SafeRange * 0.38f;
		MarkGap(X, Gap);
		X += Gap;
		Deck(X, PadLength);
		X += PadLength;
		// The wall landing doubles as the next takeoff runway; no section connector.
		X = BuildWalls(X, WallGap * 0.9f, Separation, Height);
		X = BuildGrapple(X, SwingGap, AnchorHeight);
	}
	Deck(X, 1000, FinishZone);
	GeneratedLength = X + 1000;
	DesignReport = FString::Printf(TEXT("Profile %s | capsule R/H %.0f/%.0f, crouch H %.0f | sprint %.0f | gravity %.0f | jump range/apex %.0f/%.0f | runway %.0f | tunnel %.0f x %.0f clearance | wall gap/separation %.0f/%.0f | grapple gap/underside %.0f/%.0f | lip attach %.0f (range %.0f), nominal reel %.2fs | length %.0f cm"),
		*Character->GetClass()->GetName(), Radius, HalfHeight, CrouchHalf, Sprint, Gravity, JumpRange, Apex,
		Runway, Tunnel, Clearance, WallGap, Separation, SwingGap, AnchorHeight, AttachDistance, Move->GrappleRange, ReelTime, GeneratedLength);
	if (!GetActorScale3D().Equals(FVector::OneVector) || !FMath::IsNearlyZero(GetActorRotation().Pitch)
		|| !FMath::IsNearlyZero(GetActorRotation().Roll))
	{
		DesignReport += TEXT(" | WARNING: use unit scale, zero pitch/roll to preserve sizing.");
	}
	if (Sprint <= Move->SlideMinSpeed || Sprint <= Move->WallRunMinSpeed || Clearance >= HalfHeight * 2.0f
		|| WallGap <= JumpRange || SwingGap <= JumpRange || AttachDistance >= Move->GrappleRange * 0.85f
		|| AnchorHeight - HalfHeight <= Move->GrappleRopeLength || Move->GrappleReelSpeed <= 0
		|| Move->GrapplePullAcceleration <= Gravity || ReelTime > SwingGap / FMath::Max(1.0f, Sprint))
	{
		DesignReport += TEXT(" | WARNING: profile outside course design envelope; retune geometry and verify manually.");
	}
}
