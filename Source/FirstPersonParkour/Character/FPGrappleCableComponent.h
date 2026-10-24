#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"
#include "FPGrappleCableComponent.generated.h"

/** Stable rope geometry, with no free particles, spring solver or gameplay collision. */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class FIRSTPERSONPARKOUR_API UFPGrappleCableComponent : public UProceduralMeshComponent
{
	GENERATED_BODY()

public:
	UFPGrappleCableComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	void HideGrappleCable();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grapple Visuals")
	bool bShowGrappleCable = true;

	/** Camera-local centimeters: X forward, Y right, Z up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grapple Visuals", meta = (Units = "cm"))
	FVector ViewAttachmentOffset = FVector(30.0f, 18.0f, -16.0f);

	/** Tint for the engine BasicShapeMaterial. Custom material overrides should expose a Color vector parameter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grapple Visuals")
	FLinearColor CableColor = FLinearColor(0.65f, 0.23f, 0.045f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cable Rendering", meta = (ClampMin = "0.1", ClampMax = "10", Units = "cm"))
	float CableWidth = 2.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cable Rendering", meta = (ClampMin = "4", ClampMax = "48"))
	int32 NumSegments = 20;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cable Rendering", meta = (ClampMin = "3", ClampMax = "12"))
	int32 NumSides = 6;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cable Rendering", meta = (ClampMin = "0.1", ClampMax = "32"))
	float TileMaterial = 1.0f;

private:
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> CableMaterialInstance;
	FLinearColor AppliedCableColor = FLinearColor::Transparent;
	void UpdateRopeMesh(const FVector& Origin, const FVector& Anchor, float Slack);
	TArray<FVector> RopeVertices;
	TArray<FVector> RopeNormals;
	TArray<FVector2D> RopeUVs;
	TArray<FProcMeshTangent> RopeTangents;
	TArray<int32> RopeTriangles;
	int32 BuiltSegments = 0;
	int32 BuiltSides = 0;
};
