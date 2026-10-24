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
	const auto MakeDetailBatch = [&](const TCHAR* Name)
	{
		UInstancedStaticMeshComponent* Batch = MakeBatch(Name);
		Batch->SetCollisionProfileName(TEXT("NoCollision"));
		Batch->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Batch->SetCollisionResponseToAllChannels(ECR_Ignore);
		return Batch;
	};
	ShellDetails = MakeDetailBatch(TEXT("VisualShellPanels"));
	StructureDetails = MakeDetailBatch(TEXT("VisualStructure"));
	InsetDetails = MakeDetailBatch(TEXT("VisualInsets"));
	InteractiveDetails = MakeDetailBatch(TEXT("VisualCyanGuides"));
	HazardDetails = MakeDetailBatch(TEXT("VisualHazardAccents"));
	InteractiveDetails->SetCastShadow(false);
	HazardDetails->SetCastShadow(false);
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

float APFParkourCourseBuilder::ModulePitch() const
{
	switch (DetailDensity)
	{
	case EPFCourseDetail::Low: return 600.0f;
	case EPFCourseDetail::High: return 240.0f;
	default: return 400.0f;
	}
}

void APFParkourCourseBuilder::ApplyTheme()
{
	UMaterialInterface* Parents[] = { ShellMaterial, StructureMaterial, InsetMaterial, InteractiveMaterial, HazardMaterial };
	const FLinearColor Colors[] = { ShellColor, StructureColor, InsetColor, InteractiveColor, HazardColor };
	const float Roughness[] = { 0.48f, 0.65f, 0.38f, 0.32f, 0.48f };
	const float Metallic[] = { 0.15f, 0.65f, 0.7f, 0.25f, 0.05f };
	ThemeMaterials.SetNum(5);
	for (int32 Index = 0; Index < 5; ++Index)
	{
		UMaterialInterface* Parent = Parents[Index] ? Parents[Index] : BaseMaterial.Get();
		if (!ThemeMaterials[Index] || ThemeMaterials[Index]->Parent != Parent)
		{
			ThemeMaterials[Index] = Parent ? UMaterialInstanceDynamic::Create(Parent, this) : nullptr;
		}
		if (UMaterialInstanceDynamic* MID = ThemeMaterials[Index])
		{
			MID->SetVectorParameterValue(TEXT("Color"), Colors[Index]);
			MID->SetScalarParameterValue(TEXT("Roughness"), Roughness[Index]);
			MID->SetScalarParameterValue(TEXT("Metallic"), Metallic[Index]);
		}
	}
	UInstancedStaticMeshComponent* Batches[] = { Decks, Walls, Tunnels, Targets, Hazards, StartZone, FinishZone,
		ShellDetails, StructureDetails, InsetDetails, InteractiveDetails, HazardDetails };
	const int32 Slots[] = { 0, 0, 0, 3, 4, 0, 0, 0, 1, 2, 3, 4 };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Batches); ++Index)
	{
		Batches[Index]->SetMaterial(0, ThemeMaterials[Slots[Index]]);
	}
}

void APFParkourCourseBuilder::DetailBox(UInstancedStaticMeshComponent* Mesh, const FVector& Center, const FVector& Size)
{
	if (bShowDecorations)
	{
		Box(Mesh, Center, Size);
	}
}

void APFParkourCourseBuilder::Beam(UInstancedStaticMeshComponent* Mesh, const FVector& Start, const FVector& End, float Breadth)
{
	if (bShowDecorations)
	{
		const FVector Delta = End - Start;
		Mesh->AddInstance(FTransform(FQuat::FindBetweenNormals(FVector::ForwardVector, Delta.GetSafeNormal()),
			(Start + End) * 0.5f, FVector(Delta.Size(), Breadth, Breadth) / 100.0f));
	}
}

void APFParkourCourseBuilder::DressDeck(float X, float Length)
{
	if (!bShowDecorations) return;
	// Visual skins are <= 0.3 cm above the original plane. No new walkable collision.
	const int32 Count = FMath::Max(1, FMath::CeilToInt(Length / ModulePitch()));
	const float Pitch = Length / Count;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float Center = X + (Index + 0.5f) * Pitch;
		DetailBox(InsetDetails, FVector(Center, 0, 0.06f), FVector(FMath::Max(0.1f, Pitch - 2), Width - 48, 0.1f));
		for (const float Side : { -1.0f, 1.0f })
		{
			DetailBox(ShellDetails, FVector(Center, Side * Width * 0.242f, 0.18f), FVector(FMath::Max(0.1f, Pitch - 6), Width * 0.48f - 28, 0.2f));
			if (DetailDensity != EPFCourseDetail::Low)
			{
				// Inset service handles contained in the deck top, away from the central route.
				DetailBox(InsetDetails, FVector(Center, Side * (Width * 0.5f - 70), 0.3f), FVector(48, 8, 0.1f));
			}
		}
	}
	DetailBox(StructureDetails, FVector(X + Length * 0.5f, 0, -Thickness - 32), FVector(Length, Width - 80, 64));
	DetailBox(InsetDetails, FVector(X + Length * 0.5f, 0, -Thickness - 75), FVector(Length * 0.92f, Width * 0.6f, 22));
	for (const float Side : { -1.0f, 1.0f })
	{
		DetailBox(StructureDetails, FVector(X + Length * 0.5f, Side * (Width * 0.5f + 12), -Thickness * 0.5f), FVector(Length, 24, Thickness));
		DetailBox(ShellDetails, FVector(X + Length * 0.5f, Side * (Width * 0.5f + 27), -20), FVector(Length, 14, 28));
		DetailBox(InteractiveDetails, FVector(X + Length * 0.5f, Side * (Width * 0.5f + 35), -16), FVector(FMath::Max(0.1f, Length - 8), 4, 6));
		if (DetailDensity != EPFCourseDetail::Low)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const float Center = X + (Index + 0.5f) * Pitch;
				Beam(StructureDetails, FVector(Center, Side * Width * 0.43f, -Thickness - 10),
					FVector(Center, Side * Width * 0.23f, -Thickness - 160), 24);
			}
		}
	}
}

void APFParkourCourseBuilder::DressWall(float Begin, float End, float Side, float Separation, float Height)
{
	if (!bShowDecorations) return;
	const float Face = Side * Separation * 0.5f;
	const float Back = Side * (Separation * 0.5f + 80);
	const float Length = End - Begin;
	const int32 Count = FMath::Max(1, FMath::CeilToInt(Length / ModulePitch()));
	const float Pitch = Length / Count;
	// Seam strips read as modular joints; they do not split or replace the collision wall.
	for (int32 Index = 0; Index <= Count; ++Index)
	{
		const float X = Begin + Index * Pitch;
		DetailBox(InsetDetails, FVector(X, Face - Side * 0.2f, Height * 0.5f - 150), FVector(4, 0.3f, Height + 290));
		DetailBox(StructureDetails, FVector(X, Back, Height * 0.5f - 150), FVector(22, 40, Height + 350));
		if (Index < Count)
		{
			// Elevated cyan dashes emphasize +X travel while keeping the eye-level face clean.
			DetailBox(InteractiveDetails, FVector(X + Pitch * 0.6f, Face - Side * 0.4f, Height - 110), FVector(Pitch * 0.65f, 0.4f, 12));
			if (DetailDensity == EPFCourseDetail::High)
			{
				DetailBox(InsetDetails, FVector(X + Pitch * 0.5f, Back + Side * 25, Height * 0.5f), FVector(Pitch - 30, 18, 160));
			}
		}
	}
	for (const float Z : { -320.0f, Height + 20.0f })
	{
		DetailBox(StructureDetails, FVector((Begin + End) * 0.5f, Back, Z), FVector(Length + 60, 110, 40));
	}
	DetailBox(ShellDetails, FVector((Begin + End) * 0.5f, Back, Height + 55), FVector(Length + 90, 140, 30));
}

void APFParkourCourseBuilder::DressTunnel(float X, float Length, float Clearance)
{
	// The visible portal begins exactly at the existing roof, never ahead of its collision.
	for (const float End : { X + 12, X + Length - 12 })
	{
		DetailBox(StructureDetails, FVector(End, 0, Clearance + 115), FVector(24, Width + 190, 70));
		DetailBox(HazardDetails, FVector(End - 13, 0, Clearance + 115), FVector(2, Width * 0.65f, 18));
		for (const float Side : { -1.0f, 1.0f })
		{
			DetailBox(StructureDetails, FVector(End, Side * (Width * 0.5f + 65), Clearance * 0.5f), FVector(24, 50, Clearance));
			DetailBox(InteractiveDetails, FVector(End - 13, Side * (Width * 0.5f + 65), Clearance * 0.5f), FVector(2, 12, Clearance * 0.65f));
		}
	}
	DetailBox(ShellDetails, FVector(X + Length * 0.5f, 0, Clearance + 90), FVector(Length, Width + 100, 20));
	const int32 Count = FMath::Max(2, FMath::CeilToInt(Length / (ModulePitch() * 0.5f)));
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float Center = X + (Index + 0.5f) * Length / Count;
		DetailBox(InsetDetails, FVector(Center, 0, Clearance + 106), FVector(Length / Count - 12, Width * 0.7f, 12));
		// Short overhead bars communicate low clearance without obscuring the exit.
		DetailBox(HazardDetails, FVector(Center, 0, Clearance - 0.15f), FVector(10, Width * 0.5f, 0.2f));
	}
}

void APFParkourCourseBuilder::Gateway(float X, bool bFinish)
{
	for (const float Side : { -1.0f, 1.0f })
	{
		const float Y = Side * (Width * 0.5f + 160);
		DetailBox(StructureDetails, FVector(X, Y, GatewayClearance * 0.5f - 80), FVector(150, 120, GatewayClearance + 160));
		DetailBox(ShellDetails, FVector(X - 10, Y, GatewayClearance * 0.5f), FVector(170, 155, GatewayClearance * 0.72f));
		DetailBox(InsetDetails, FVector(X - 98, Y, GatewayClearance * 0.5f), FVector(6, 110, 180));
		DetailBox(InteractiveDetails, FVector(X - 103, Y, GatewayClearance * 0.5f), FVector(4, 16, 140));
		DetailBox(StructureDetails, FVector(X, Y, -Thickness - 50), FVector(280, 260, 100));
	}
	DetailBox(StructureDetails, FVector(X, 0, GatewayClearance + 60), FVector(180, Width + 440, 120));
	DetailBox(ShellDetails, FVector(X, 0, GatewayClearance + 140), FVector(230, Width + 500, 40));
	DetailBox(InteractiveDetails, FVector(X - 94, 0, GatewayClearance + 60), FVector(8, Width + 180, 16));
	if (bFinish)
	{
		// Two bands and a chequer pattern make finish distinct without text/font actors.
		for (int32 Row = 0; Row < 2; ++Row)
		{
			for (int32 Column = 0; Column < 10; ++Column)
			{
				DetailBox((Row + Column) % 2 ? StructureDetails.Get() : InteractiveDetails.Get(),
					FVector(X - 100 + Row * 40, (Column - 4.5f) * Width / 10, 0.4f), FVector(38, Width / 10 - 2, 0.2f));
			}
		}
	}
	else
	{
		for (int32 Index = 0; Index < 3; ++Index)
		{
			const FVector Tip(X + 120 + Index * 100, 0, 0.4f);
			// Flat chevron arms: a floor marking rather than a raised obstacle.
			for (const float Side : { -1.0f, 1.0f })
			{
				const FVector Start = Tip + FVector(-45, Side * 60, 0);
				const FVector Delta = Tip - Start;
				if (bShowDecorations)
				{
					InteractiveDetails->AddInstance(FTransform(Delta.Rotation(), (Start + Tip) * 0.5f,
						FVector(Delta.Size(), 5, 0.2f) / 100.0f));
				}
			}
		}
	}
}

void APFParkourCourseBuilder::BuildBackground()
{
	if (!bShowDecorations || !bShowBackgroundArchitecture) return;
	// Separate outboard bays, no geometry across gaps or beyond the start/finish lane.
	const float Pitch = ModulePitch() * 6;
	const int32 Count = FMath::CeilToInt(GeneratedLength / Pitch);
	for (const float Side : { -1.0f, 1.0f })
	{
		const float Y = Side * (Width * 0.5f + 1800);
		for (const float Z : { -850.0f, 350.0f })
		{
			DetailBox(StructureDetails, FVector(GeneratedLength * 0.5f, Y, Z), FVector(GeneratedLength, 180, 100));
		}
		DetailBox(ShellDetails, FVector(GeneratedLength * 0.5f, Y, 425), FVector(GeneratedLength, 220, 50));
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float X = (Index + 0.5f) * GeneratedLength / Count;
		const float Height = 1500 + (Index % 3) * 260;
		for (const float Side : { -1.0f, 1.0f })
		{
			const float Y = Side * (Width * 0.5f + 1800);
			DetailBox(StructureDetails, FVector(X, Y, Height * 0.5f - 900), FVector(240, 300, Height));
			DetailBox(ShellDetails, FVector(X + 10, Y, Height * 0.5f - 600), FVector(280, 360, Height * 0.55f));
			DetailBox(InsetDetails, FVector(X, Y - Side * 186, Height * 0.5f - 600), FVector(160, 12, Height * 0.42f));
			DetailBox(InteractiveDetails, FVector(X - 148, Y, Height * 0.5f - 600), FVector(8, 180, 220));
			Beam(StructureDetails, FVector(X, Y, -850), FVector(X + 420, Y + Side * 240, -1200), 90);
		}
	}
}

void APFParkourCourseBuilder::Deck(float X, float Length, UInstancedStaticMeshComponent* Mesh)
{
	if (Length > 0.0f)
	{
		Box(Mesh ? Mesh : Decks.Get(), FVector(X + Length * 0.5f, 0, -Thickness * 0.5f),
			FVector(Length, Width, Thickness));
		DressDeck(X, Length);
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
		DressWall(X + Begin, X + End, Side, Separation, Height);
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
	// Everything is above the target underside or outboard of the swing lane.
	const float Center = X + Gap * 0.5f;
	const float Outboard = Width * 0.5f + 350.0f;
	for (const float Side : { -1.0f, 1.0f })
	{
		DetailBox(StructureDetails, FVector(Center, Side * Outboard, (Height - 200) * 0.5f), FVector(180, 140, Height + 600));
		DetailBox(ShellDetails, FVector(Center, Side * Outboard, Height + 210), FVector(360, 220, 180));
		DetailBox(InsetDetails, FVector(Center, Side * Outboard, Height + 330), FVector(230, 250, 60));
		DetailBox(InteractiveDetails, FVector(Center - 183, Side * Outboard, Height + 210), FVector(8, 160, 90));
		Beam(StructureDetails, FVector(Center, Side * Outboard, Height - 100),
			FVector(Center, Side * Width * 0.25f, Height + 310), 65);
		// Cyan side rails identify the actual broad attachment slab; no fake targets below it.
		DetailBox(InteractiveDetails, FVector(Center, Side * (Width * 0.375f + 8), Height + 45), FVector(400, 16, 36));
	}
	DetailBox(StructureDetails, FVector(Center, 0, Height + 260), FVector(180, Outboard * 2, 160));
	DetailBox(ShellDetails, FVector(Center, 0, Height + 370), FVector(240, Outboard * 2 + 160, 60));
	DetailBox(InsetDetails, FVector(Center, 0, Height + 140), FVector(250, Width * 0.55f, 80));
	for (int32 Index = -1; Index <= 1; ++Index)
	{
		DetailBox(HazardDetails, FVector(Center + Index * 95, 0, Height + 405), FVector(36, Width * 0.5f, 10));
	}
	const float Landing = FMath::Clamp(GrappleLandingLength, 1000.0f, 2400.0f);
	Deck(X + Gap, Landing);
	return X + Gap + Landing;
}

void APFParkourCourseBuilder::RegenerateCourse()
{
	UInstancedStaticMeshComponent* Batches[] = { Decks, Walls, Tunnels, Targets, Hazards, StartZone, FinishZone,
		ShellDetails, StructureDetails, InsetDetails, InteractiveDetails, HazardDetails };
	for (UInstancedStaticMeshComponent* Batch : Batches)
	{
		Batch->ClearInstances();
	}
	ApplyTheme();
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
	GatewayClearance = FMath::Max(600.0f, Apex + HalfHeight * 2.0f + 200.0f);
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
	Gateway(180, false);
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
		DressTunnel(X, Tunnel, Clearance);
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
	Gateway(X + 700, true);
	GeneratedLength = X + 1000;
	BuildBackground();
	DesignReport = FString::Printf(TEXT("Profile %s | capsule R/H %.0f/%.0f, crouch H %.0f | sprint %.0f | gravity %.0f | jump range/apex %.0f/%.0f | runway %.0f | tunnel %.0f x %.0f clearance | wall gap/separation %.0f/%.0f | grapple gap/underside %.0f/%.0f | lip attach %.0f (range %.0f), nominal reel %.2fs | length %.0f cm"),
		*Character->GetClass()->GetName(), Radius, HalfHeight, CrouchHalf, Sprint, Gravity, JumpRange, Apex,
		Runway, Tunnel, Clearance, WallGap, Separation, SwingGap, AnchorHeight, AttachDistance, Move->GrappleRange, ReelTime, GeneratedLength);
	int32 VisualCount = 0;
	for (UInstancedStaticMeshComponent* Batch : { ShellDetails.Get(), StructureDetails.Get(), InsetDetails.Get(), InteractiveDetails.Get(), HazardDetails.Get() })
	{
		VisualCount += Batch->GetInstanceCount();
	}
	DesignReport += FString::Printf(TEXT(" | M8.2 visual instances %d (5 non-colliding batches)"), VisualCount);
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
