#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PFParkourCourseBuilder.generated.h"

class AFPPlayerCharacter;
class UInstancedStaticMeshComponent;
class UMaterialInterface;

UENUM(BlueprintType)
enum class EPFCourseDetail : uint8
{
	Low,
	Medium,
	High
};

/** Local +X course. Starting deck surface is local Z=0. Use yaw rotation and unit scale. */
UCLASS(Blueprintable, ClassGroup=(Parkour), meta=(DisplayName="PF Parkour Course Builder"))
class FIRSTPERSONPARKOUR_API APFParkourCourseBuilder : public AActor
{
	GENERATED_BODY()

public:
	APFParkourCourseBuilder();
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;

	/** Rebuilds only this actor's instances; safe to repeat. */
	UFUNCTION(CallInEditor, Category="Course")
	void RegenerateCourse();

	/** Select the actual pawn Blueprint to account for its class-default overrides. */
	UPROPERTY(EditAnywhere, Category="Course|Generation")
	TSubclassOf<AFPPlayerCharacter> CharacterProfile;
	UPROPERTY(EditAnywhere, Category="Course|Generation")
	bool bGenerateCourse = true;
	UPROPERTY(EditAnywhere, Category="Course|Generation")
	bool bSprintSection = true;
	UPROPERTY(EditAnywhere, Category="Course|Generation")
	bool bSlideSection = true;
	UPROPERTY(EditAnywhere, Category="Course|Generation")
	bool bWallRunSection = true;
	UPROPERTY(EditAnywhere, Category="Course|Generation")
	bool bGrappleSection = true;
	UPROPERTY(EditAnywhere, Category="Course|Generation")
	bool bFinalSection = true;
	/** Optional red catch decks below gaps. No damage, reset or checkpoint behavior. */
	UPROPERTY(EditAnywhere, Category="Course|Generation")
	bool bRecoveryDecks = false;

	/** Safe walking connector between sections, not an additional jump gap. */
	UPROPERTY(EditAnywhere, Category="Course|Layout", meta=(ClampMin="0", ClampMax="3000", Units="cm"))
	float SectionSpacing = 400.0f;
	UPROPERTY(EditAnywhere, Category="Course|Layout", meta=(ClampMin="400", ClampMax="2000", Units="cm"))
	float PlatformWidth = 900.0f;
	UPROPERTY(EditAnywhere, Category="Course|Layout", meta=(ClampMin="400", ClampMax="2000", Units="cm"))
	float JumpPlatformLength = 700.0f;
	UPROPERTY(EditAnywhere, Category="Course|Layout", meta=(ClampMin="20", ClampMax="200", Units="cm"))
	float PlatformThickness = 80.0f;
	UPROPERTY(EditAnywhere, Category="Course|Sprint", meta=(ClampMin="800", ClampMax="5000", Units="cm"))
	float RunwayLength = 1600.0f;
	UPROPERTY(EditAnywhere, Category="Course|Sprint", meta=(ClampMin="1", ClampMax="6"))
	int32 JumpPlatformCount = 3;
	/** Fraction of ideal same-height sprint range, with capsule landing margin removed. */
	UPROPERTY(EditAnywhere, Category="Course|Sprint", meta=(ClampMin="0.2", ClampMax="0.55"))
	float FirstJumpFraction = 0.28f;
	UPROPERTY(EditAnywhere, Category="Course|Sprint", meta=(ClampMin="0", ClampMax="0.06"))
	float JumpFractionIncrement = 0.06f;

	UPROPERTY(EditAnywhere, Category="Course|Slide", meta=(ClampMin="4", ClampMax="30", Units="cm"))
	float TunnelClearanceMargin = 16.0f;
	UPROPERTY(EditAnywhere, Category="Course|Slide", meta=(ClampMin="150", ClampMax="600", Units="cm"))
	float TunnelLength = 300.0f;
	UPROPERTY(EditAnywhere, Category="Course|Wall Run", meta=(ClampMin="200", ClampMax="600", Units="cm"))
	float WallSeparation = 300.0f;
	UPROPERTY(EditAnywhere, Category="Course|Wall Run", meta=(ClampMin="400", ClampMax="1600", Units="cm"))
	float WallHeight = 800.0f;
	UPROPERTY(EditAnywhere, Category="Course|Wall Run", meta=(ClampMin="1.6", ClampMax="2.2"))
	float WallGapJumpMultiple = 2.0f;

	/** Limited by range, rope clearance and reel time. */
	UPROPERTY(EditAnywhere, Category="Course|Grapple", meta=(ClampMin="1000", ClampMax="2200", Units="cm"))
	float GrappleGap = 1400.0f;
	/** Underside height, increased for rope length + capsule + clearance. */
	UPROPERTY(EditAnywhere, Category="Course|Grapple", meta=(ClampMin="800", ClampMax="1800", Units="cm"))
	float GrappleTargetHeight = 950.0f;
	UPROPERTY(EditAnywhere, Category="Course|Grapple", meta=(ClampMin="1000", ClampMax="2400", Units="cm"))
	float GrappleLandingLength = 1600.0f;

	/** All visual modules are non-colliding, including Pawn/Visibility/Camera traces. */
	UPROPERTY(EditAnywhere, Category="Course|Visuals")
	bool bShowDecorations = true;
	UPROPERTY(EditAnywhere, Category="Course|Visuals")
	bool bShowBackgroundArchitecture = true;
	/** Changes panel/rib spacing only; never changes the course layout. */
	UPROPERTY(EditAnywhere, Category="Course|Visuals")
	EPFCourseDetail DetailDensity = EPFCourseDetail::Medium;
	UPROPERTY(EditAnywhere, Category="Course|Theme")
	FLinearColor ShellColor = FLinearColor(0.82f, 0.87f, 0.90f);
	UPROPERTY(EditAnywhere, Category="Course|Theme")
	FLinearColor StructureColor = FLinearColor(0.025f, 0.038f, 0.055f);
	UPROPERTY(EditAnywhere, Category="Course|Theme")
	FLinearColor InsetColor = FLinearColor(0.22f, 0.29f, 0.34f);
	UPROPERTY(EditAnywhere, Category="Course|Theme")
	FLinearColor InteractiveColor = FLinearColor(0.015f, 0.72f, 0.95f);
	UPROPERTY(EditAnywhere, Category="Course|Theme")
	FLinearColor HazardColor = FLinearColor(1.0f, 0.12f, 0.025f);
	/** Optional reusable parent materials. Color vector parameter is tinted by the theme.
	 * Roughness/Metallic scalar parameters are set when exposed by the parent. */
	UPROPERTY(EditAnywhere, Category="Course|Materials")
	TObjectPtr<UMaterialInterface> ShellMaterial;
	UPROPERTY(EditAnywhere, Category="Course|Materials")
	TObjectPtr<UMaterialInterface> StructureMaterial;
	UPROPERTY(EditAnywhere, Category="Course|Materials")
	TObjectPtr<UMaterialInterface> InsetMaterial;
	UPROPERTY(EditAnywhere, Category="Course|Materials")
	TObjectPtr<UMaterialInterface> InteractiveMaterial;
	UPROPERTY(EditAnywhere, Category="Course|Materials")
	TObjectPtr<UMaterialInterface> HazardMaterial;

	UPROPERTY(VisibleAnywhere, Category="Course|Resolved")
	FString DesignReport;
	UPROPERTY(VisibleAnywhere, Category="Course|Resolved", meta=(Units="cm"))
	float GeneratedLength = 0.0f;

private:
	UPROPERTY(VisibleAnywhere, Category="Course|Meshes")
	TObjectPtr<UInstancedStaticMeshComponent> Decks;
	UPROPERTY(VisibleAnywhere, Category="Course|Meshes")
	TObjectPtr<UInstancedStaticMeshComponent> Walls;
	UPROPERTY(VisibleAnywhere, Category="Course|Meshes")
	TObjectPtr<UInstancedStaticMeshComponent> Tunnels;
	UPROPERTY(VisibleAnywhere, Category="Course|Meshes")
	TObjectPtr<UInstancedStaticMeshComponent> Targets;
	UPROPERTY(VisibleAnywhere, Category="Course|Meshes")
	TObjectPtr<UInstancedStaticMeshComponent> Hazards;
	UPROPERTY(VisibleAnywhere, Category="Course|Meshes")
	TObjectPtr<UInstancedStaticMeshComponent> StartZone;
	UPROPERTY(VisibleAnywhere, Category="Course|Meshes")
	TObjectPtr<UInstancedStaticMeshComponent> FinishZone;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> BaseMaterial;
	UPROPERTY(VisibleAnywhere, Category="Course|Meshes")
	TObjectPtr<UInstancedStaticMeshComponent> ShellDetails;
	UPROPERTY(VisibleAnywhere, Category="Course|Meshes")
	TObjectPtr<UInstancedStaticMeshComponent> StructureDetails;
	UPROPERTY(VisibleAnywhere, Category="Course|Meshes")
	TObjectPtr<UInstancedStaticMeshComponent> InsetDetails;
	UPROPERTY(VisibleAnywhere, Category="Course|Meshes")
	TObjectPtr<UInstancedStaticMeshComponent> InteractiveDetails;
	UPROPERTY(VisibleAnywhere, Category="Course|Meshes")
	TObjectPtr<UInstancedStaticMeshComponent> HazardDetails;
	/** Five shared transient MIDs, reused across gameplay and decorative batches. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UMaterialInstanceDynamic>> ThemeMaterials;

	void Box(UInstancedStaticMeshComponent* Mesh, const FVector& Center, const FVector& Size);
	void DetailBox(UInstancedStaticMeshComponent* Mesh, const FVector& Center, const FVector& Size);
	void Beam(UInstancedStaticMeshComponent* Mesh, const FVector& Start, const FVector& End, float Breadth);
	void DressDeck(float X, float Length);
	void DressWall(float Begin, float End, float Side, float Separation, float Height);
	void DressTunnel(float X, float Length, float Clearance);
	void Gateway(float X, bool bFinish);
	void BuildBackground();
	void ApplyTheme();
	float ModulePitch() const;
	void Deck(float X, float Length, UInstancedStaticMeshComponent* Mesh = nullptr);
	void MarkGap(float X, float Length);
	float BuildWalls(float X, float Gap, float Separation, float Height);
	float BuildGrapple(float X, float Gap, float Height);
	float Width = 900.0f;
	float Thickness = 80.0f;
	float WallApproachOffset = 52.0f;
	float GatewayClearance = 600.0f;
};
