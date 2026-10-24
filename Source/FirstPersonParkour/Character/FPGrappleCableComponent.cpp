#include "Character/FPGrappleCableComponent.h"
#include "Character/FPPlayerCharacter.h"
#include "Movement/FPParkourMovementComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

UFPGrappleCableComponent::UFPGrappleCableComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Camera cache has its final view by PostUpdateWork; no recursive camera queries.
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	PrimaryComponentTick.bCanEverTick = true;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	SetCastShadow(false);
	SetOnlyOwnerSee(true);
	SetVisibility(false);
	bTickInEditor = false;
	bUseComplexAsSimpleCollision = false;
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> RopeMaterial(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (RopeMaterial.Succeeded())
	{
		SetMaterial(0, RopeMaterial.Object);
	}
}

void UFPGrappleCableComponent::BeginPlay()
{
	Super::BeginPlay();
	CableMaterialInstance = CreateDynamicMaterialInstance(0);
}

void UFPGrappleCableComponent::HideGrappleCable()
{
	SetVisibility(false);
}

void UFPGrappleCableComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	const AFPPlayerCharacter* Player = Cast<AFPPlayerCharacter>(GetOwner());
	const UFPParkourMovementComponent* Movement = Player ? Player->GetParkourMovementComponent() : nullptr;
	const APlayerController* Controller = Player ? Cast<APlayerController>(Player->GetController()) : nullptr;
	const APlayerCameraManager* Camera = Controller ? Controller->PlayerCameraManager : nullptr;
	if (!bShowGrappleCable || !Movement || !Movement->IsGrappling() || !Player->IsLocallyControlled()
		|| !Camera || Camera->GetViewTarget() != Player)
	{
		HideGrappleCable();
		return; // No hidden mesh updates.
	}
	const FMinimalViewInfo& View = Camera->GetCameraCacheView();
	if (CableMaterialInstance && !AppliedCableColor.Equals(CableColor))
	{
		CableMaterialInstance->SetVectorParameterValue(TEXT("Color"), CableColor);
		AppliedCableColor = CableColor;
	}
	const FVector Origin = View.Location + View.Rotation.RotateVector(ViewAttachmentOffset);
	const FVector Anchor = Movement->GetGrappleAnchor();
	// World-sized geometry must not inherit any visual/actor scale.
	SetWorldTransform(FTransform(FQuat::Identity, Origin));
	const float PhysicalDistance = FVector::Distance(Player->GetActorLocation(), Anchor);
	const float Slack = FMath::Max(0.0f, Movement->GetActiveGrappleRopeLength() - PhysicalDistance);
	UpdateRopeMesh(Origin, Anchor, Slack);
	SetVisibility(true);
}

void UFPGrappleCableComponent::UpdateRopeMesh(const FVector& Origin, const FVector& Anchor, float Slack)
{
	const FVector Chord = Anchor - Origin;
	const float Distance = Chord.Size();
	if (Distance < 1.0f)
	{
		ClearAllMeshSections();
		BuiltSegments = BuiltSides = 0;
		return;
	}
	const int32 Segments = FMath::Clamp(NumSegments, 4, 48);
	const int32 Sides = FMath::Clamp(NumSides, 3, 12);
	const int32 RingSize = Sides + 1;
	const bool bRebuild = BuiltSegments != Segments || BuiltSides != Sides;
	const FVector Direction = Chord / Distance;
	FVector Side = FVector::CrossProduct(Direction, FVector::DownVector).GetSafeNormal();
	if (Side.IsNearlyZero())
	{
		FVector Unused;
		Direction.FindBestAxisVectors(Side, Unused);
	}
	const FVector SagDirection = FVector::CrossProduct(Side, Direction).GetSafeNormal();
	const auto PointAt = [&Chord, &SagDirection](float T, float Sag)
	{
		return Chord * T + SagDirection * (4.0f * Sag * T * (1.0f - T));
	};
	// Fit a single smooth curve to the actual available slack. With no slack
	// this is exactly a straight line, regardless of speed or frame time.
	float Sag = 0.0f;
	if (Slack > UE_KINDA_SMALL_NUMBER)
	{
		const float TargetLength = Distance + Slack;
		float Low = 0.0f;
		float High = TargetLength;
		for (int32 Search = 0; Search < 16; ++Search)
		{
			const float Candidate = (Low + High) * 0.5f;
			float Length = 0.0f;
			FVector Previous = FVector::ZeroVector;
			for (int32 Ring = 1; Ring <= Segments; ++Ring)
			{
				const FVector Point = PointAt(static_cast<float>(Ring) / Segments, Candidate);
				Length += FVector::Distance(Previous, Point);
				Previous = Point;
			}
			if (Length < TargetLength) { Low = Candidate; }
			else { High = Candidate; }
		}
		Sag = (Low + High) * 0.5f;
	}
	const int32 VertexCount = (Segments + 1) * RingSize;
	RopeVertices.SetNumUninitialized(VertexCount);
	RopeNormals.SetNumUninitialized(VertexCount);
	RopeUVs.SetNumUninitialized(VertexCount);
	RopeTangents.SetNumUninitialized(VertexCount);
	const float Radius = FMath::Clamp(CableWidth, 0.1f, 10.0f) * 0.5f;
	for (int32 Ring = 0; Ring <= Segments; ++Ring)
	{
		const float T = static_cast<float>(Ring) / Segments;
		const FVector Center = PointAt(T, Sag);
		const FVector Tangent = (Chord + SagDirection * (4.0f * Sag * (1.0f - 2.0f * T))).GetSafeNormal();
		const FVector Up = FVector::CrossProduct(Tangent, Side).GetSafeNormal();
		for (int32 Face = 0; Face <= Sides; ++Face)
		{
			const float U = static_cast<float>(Face) / Sides;
			const float Angle = 2.0f * PI * U;
			const FVector Normal = Side * FMath::Cos(Angle) + Up * FMath::Sin(Angle);
			const int32 Index = Ring * RingSize + Face;
			RopeVertices[Index] = Center + Normal * Radius;
			RopeNormals[Index] = Normal;
			RopeUVs[Index] = FVector2D(U, T * FMath::Clamp(TileMaterial, 0.1f, 32.0f));
			RopeTangents[Index] = FProcMeshTangent(Tangent, false);
		}
	}
	const TArray<FLinearColor> NoColors;
	if (bRebuild)
	{
		RopeTriangles.Reset(Segments * Sides * 6);
		for (int32 Ring = 0; Ring < Segments; ++Ring)
		{
			for (int32 Face = 0; Face < Sides; ++Face)
			{
				const int32 A = Ring * RingSize + Face;
				const int32 B = A + RingSize;
				RopeTriangles.Append({A, A + 1, B, A + 1, B + 1, B});
			}
		}
		CreateMeshSection_LinearColor(0, RopeVertices, RopeTriangles, RopeNormals, RopeUVs, NoColors, RopeTangents, false);
		BuiltSegments = Segments;
		BuiltSides = Sides;
	}
	else
	{
		UpdateMeshSection_LinearColor(0, RopeVertices, RopeNormals, RopeUVs, NoColors, RopeTangents);
	}
}
