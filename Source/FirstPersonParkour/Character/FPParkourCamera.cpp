#include "Character/FPPlayerCharacter.h"

#include "Camera/CameraComponent.h"
#include "Character/FPGrappleCableComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "Movement/FPParkourMovementComponent.h"

namespace
{
	// Exact solution for a constant target over Dt, including under/critical/over damping.
	// Unlike an Euler spring this remains stable across low frame rates and hitches.
	template<typename T>
	void CameraSpring(T& Value, T& Velocity, const T& Target, float Frequency, float Damping, float Dt)
	{
		if (Dt <= 0.0f) { return; }
		const float W = 2.0f * PI * FMath::Clamp(Frequency, 0.5f, 12.0f);
		const float Z = FMath::Clamp(Damping, 0.7f, 2.0f);
		const T Y = Value - Target;
		if (FMath::Abs(Z - 1.0f) < 0.001f)
		{
			const float E = FMath::Exp(-W * Dt);
			const T C = Velocity + Y * W;
			Value = Target + (Y + C * Dt) * E;
			Velocity = (Velocity - C * (W * Dt)) * E;
		}
		else if (Z < 1.0f)
		{
			const float WD = W * FMath::Sqrt(1.0f - Z * Z);
			const float E = FMath::Exp(-Z * W * Dt);
			const float C = FMath::Cos(WD * Dt);
			const float S = FMath::Sin(WD * Dt);
			Value = Target + (Y * C + (Velocity + Y * (Z * W)) * (S / WD)) * E;
			Velocity = (Velocity * C - (Velocity * (Z * W) + Y * (W * W)) * (S / WD)) * E;
		}
		else
		{
			const float R1 = -W * (Z - FMath::Sqrt(Z * Z - 1.0f));
			const float R2 = -W * (Z + FMath::Sqrt(Z * Z - 1.0f));
			const T C1 = (Velocity - Y * R2) / (R1 - R2);
			const T C2 = Y - C1;
			const float E1 = FMath::Exp(R1 * Dt), E2 = FMath::Exp(R2 * Dt);
			Value = Target + C1 * E1 + C2 * E2;
			Velocity = C1 * (R1 * E1) + C2 * (R2 * E2);
		}
	}

	float Approach(float Current, float Target, float Rate, float Dt)
	{
		return FMath::Lerp(Current, Target, 1.0f - FMath::Exp(-FMath::Max(0.1f, Rate) * Dt));
	}

	void LimitAxis(double& Value, double& Velocity, float Limit)
	{
		const double Bound = FMath::Max(0.0f, Limit);
		if (FMath::Abs(Value) >= Bound)
		{
			Value = FMath::Clamp(Value, -Bound, Bound);
			if (Value * Velocity >= 0.0 || Bound == 0.0) { Velocity = 0.0; }
		}
	}
}

void AFPPlayerCharacter::FCameraFeedbackPulse::Sample(double Now, FVector& Offset, FVector& Rotation, float& FOV) const
{
	Offset = Rotation = FVector::ZeroVector;
	FOV = 0.0f;
	if (StartTime < 0.0 || Now < StartTime || Now >= StartTime + Duration) { return; }
	const float Age = static_cast<float>((Now - StartTime) / Duration);
	const auto Smooth = [](float T) { return T * T * (3.0f - 2.0f * T); };
	constexpr float Attack = 0.16f;
	const float Weight = Age < Attack ? Smooth(Age / Attack) : 1.0f - Smooth((Age - Attack) / (1.0f - Attack));
	const float Carry = (1.0f - Smooth(Age)) * (1.0f - Weight);
	Offset = StartOffset * Carry + PeakOffset * Weight;
	Rotation = StartRotation * Carry + PeakRotation * Weight;
	FOV = StartFOV * Carry + PeakFOV * Weight;
}

void AFPPlayerCharacter::TriggerCameraEvent(ECameraEvent Source, const FVector& Offset, const FVector& Rotation, float FOV, float Duration)
{
	const double Now = GetWorld()->GetTimeSeconds();
	// One event channel: landings replace takeoff/release feedback instead of summing it.
	FVector PreviousOffset, PreviousRotation;
	float PreviousFOV;
	CameraEvent.Sample(Now, PreviousOffset, PreviousRotation, PreviousFOV);
	CameraEvent.StartOffset = PreviousOffset;
	CameraEvent.StartRotation = PreviousRotation;
	CameraEvent.StartFOV = PreviousFOV;
	CameraEvent.PeakOffset = Offset;
	CameraEvent.PeakRotation = Rotation;
	// A single excursion above the state candidate. Retriggering never uses the previous
	// event or rendered FOV as its baseline, so rapid transitions cannot stack boosts.
	CameraEvent.PeakFOV = FOV > 0.0f ? CameraStateFOV + FOV : 0.0f;
	CameraEvent.StartTime = Now;
	CameraEvent.Duration = FMath::Clamp(Duration, 0.05f, 1.0f);
	CameraEventSource = Source;
}

void AFPPlayerCharacter::NotifyWallJumpCamera(const FVector& JumpedWallNormal)
{
	bWallJumpLandingPending = true;
	if (bEnableWallJumpCameraEffects && !JumpedWallNormal.ContainsNaN())
	{
		const FVector Away = JumpedWallNormal.GetSafeNormal2D();
		TriggerCameraEvent(ECameraEvent::WallJump, Away * FMath::Clamp(WallJumpCameraImpulseIntensity, 0.0f, 3.0f)
			- FVector::UpVector * 0.7f, FVector(-0.8f, 0, 0),
			FMath::Clamp(WallJumpCameraFOVPulse, 0.0f, 5.0f), WallJumpCameraRecoveryDuration);
	}
}

void AFPPlayerCharacter::NotifyGrappleVisuals(bool bAttached, const FVector& Momentum)
{
	if (!bAttached) { GrappleCableComponent->HideGrappleCable(); }
	if (!bEnableGrappleCameraEffects || !IsLocallyControlled() || Momentum.ContainsNaN()) { return; }
	const FVector Direction = bAttached
		? (ParkourMovementComponent->GetGrappleAnchor() - GetActorLocation()).GetSafeNormal() : Momentum.GetSafeNormal();
	const float Strength = bAttached ? 1.0f
		: FMath::Clamp(Momentum.Size() / FMath::Max(1.0f, GrappleCameraFullEffectSpeed), 0.0f, 1.0f);
	TriggerCameraEvent(ECameraEvent::Grapple,
		Direction * Strength * FMath::Clamp(bAttached ? GrappleAttachCameraImpulse : GrappleReleaseCameraImpulse, 0.0f, 2.0f),
		FVector(bAttached ? -0.6f : 0.4f, 0, 0) * Strength,
		Strength * FMath::Clamp(bAttached ? GrappleAttachFOVPulse : GrappleReleaseFOVPulse, 0.0f, 5.0f),
		bAttached ? GrappleAttachFeedbackDuration : GrappleReleaseFeedbackDuration);
}

void AFPPlayerCharacter::OnJumped_Implementation()
{
	Super::OnJumped_Implementation();
	if (bEnableJumpCameraFeedback)
	{
		TriggerCameraEvent(ECameraEvent::Jump, -FVector::UpVector * FMath::Clamp(JumpCameraImpulse, 0.0f, 6.0f),
			FVector(-0.7f, 0, 0), 0.0f, JumpCameraFeedbackDuration);
	}
}

void AFPPlayerCharacter::Landed(const FHitResult& Hit)
{
	// Native Landed provides pre-impact velocity. No velocity differencing can create a second landing.
	const bool bWallProfile = bWallJumpLandingPending;
	bCameraLandingWasWallJump = bWallProfile;
	const bool bEnabled = bWallProfile ? bEnableWallJumpCameraEffects && bEnableWallJumpLandingFeedback : bEnableLandingCameraFeedback;
	const float Impact = FMath::Max(0.0f, -FVector::DotProduct(GetVelocity(), Hit.ImpactNormal));
	const float MinSpeed = FMath::Max(0.0f, bWallProfile ? WallJumpLandingMinSpeed : LandingCameraMinSpeed);
	const float FullSpeed = FMath::Max(MinSpeed + 1.0f, bWallProfile ? WallJumpLandingFullEffectSpeed : LandingCameraFullEffectSpeed);
	const float Strength = bEnabled ? FMath::Clamp((Impact - MinSpeed) / (FullSpeed - MinSpeed), 0.0f, 1.0f) : 0.0f;
	TriggerCameraEvent(ECameraEvent::Landing,
		-FVector::UpVector * Strength * FMath::Clamp(bWallProfile ? WallJumpLandingIntensity : LandingCameraIntensity, 0.0f, 10.0f),
		FVector(-FMath::Clamp(LandingCameraPitch, 0.0f, 3.0f) * Strength, 0, 0), 0.0f,
		bWallProfile ? WallJumpLandingRecoveryDuration : LandingCameraRecoveryDuration);
	bWallJumpLandingPending = false;
	Super::Landed(Hit);
}

void AFPPlayerCharacter::ResetCameraFeedback()
{
	CameraEvent = FCameraFeedbackPulse();
	CameraEventSource = ECameraEvent::None;
	CameraPosition = CameraPositionVelocity = CameraRotation = CameraRotationVelocity = FVector::ZeroVector;
	CameraFOV = CameraFOVVelocity = CameraStateFOV = 0.0f;
	FilteredCameraAcceleration = PreviousCameraVelocity = PreviousCameraLocation = FVector::ZeroVector;
	CameraCollisionOffset = FVector::ZeroVector;
	SmoothedSprintFOV = SprintCameraMotionWeight = SprintCameraMotionPhase = 0.0f;
	WallCameraWeight = GrappleCameraWeight = SlideCameraWeight = AirCameraWeight = 0.0f;
	bHasCameraHistory = bWasCameraWallRunning = bWallJumpLandingPending = bCameraLandingWasWallJump = false;
	LastCameraUpdateTime = -1.0;
}

void AFPPlayerCharacter::UpdateParkourCamera(float Dt, double Now, const FRotator& ViewRotation)
{
	const UFPParkourMovementComponent* Move = ParkourMovementComponent;
	const FVector Velocity = GetVelocity();
	const float Speed = Velocity.Size2D();
	const float Sprint = FMath::Max(1.0f, Move->SprintSpeed);
	const bool bWall = Move->IsWallRunning();
	const bool bGrapple = Move->IsGrappling();
	const bool bSlide = Move->IsSliding() || bIsCrouched;
	const bool bAir = Move->IsFalling() && !bGrapple;
	const bool bGround = Move->IsMovingOnGround() && !bSlide;
	const FRotationMatrix FlatView(FRotator(0, ViewRotation.Yaw, 0));
	const FVector Forward = FlatView.GetUnitAxis(EAxis::X), Right = FlatView.GetUnitAxis(EAxis::Y);
	FVector Acceleration = FVector::ZeroVector;
	float Turn = 0.0f;
	if (bHasCameraHistory && Dt > UE_SMALL_NUMBER && Dt <= 0.25f)
	{
		Acceleration = ((Velocity - PreviousCameraVelocity) / Dt).GetClampedToMaxSize(8000.0f);
		Turn = FMath::Clamp(FMath::FindDeltaAngleDegrees(PreviousCameraYaw, ViewRotation.Yaw)
			/ (Dt * FMath::Max(1.0f, CameraTurnFullSpeed)), -1.0f, 1.0f);
	}
	// Impact and takeoff accelerations belong exclusively to their event, not inertia.
	if (CameraEventSource == ECameraEvent::Landing || CameraEventSource == ECameraEvent::Jump
		|| CameraEventSource == ECameraEvent::WallJump)
	{
		if (Now < CameraEvent.StartTime + 0.12) { Acceleration.Z = 0.0; }
	}
	const float FilterAlpha = 1.0f - FMath::Exp(-FMath::Clamp(CameraAccelerationFilterSpeed, 1.0f, 30.0f) * Dt);
	FilteredCameraAcceleration = FMath::Lerp(FilteredCameraAcceleration, Acceleration, FilterAlpha);
	const FVector HorizontalAcceleration(FilteredCameraAcceleration.X, FilteredCameraAcceleration.Y, 0);
	const FVector Inertia = HorizontalAcceleration / FMath::Max(1.0f, CameraInertiaFullAcceleration);
	const float SpeedWeight = FMath::Clamp((Speed - Move->WalkSpeed) / FMath::Max(1.0f, Sprint - Move->WalkSpeed), 0.0f, 1.0f);
	const float TravelWeight = FMath::Clamp(Speed / Sprint, 0.0f, 1.0f);
	WallCameraWeight = Approach(WallCameraWeight, bWall && bEnableWallRunCameraEffects ? 1.0f : 0.0f,
		bWall ? WallRunCameraBlendInSpeed : WallRunCameraBlendOutSpeed, Dt);
	GrappleCameraWeight = Approach(GrappleCameraWeight, bGrapple && bEnableGrappleCameraEffects ? 1.0f : 0.0f, GrappleCameraBlendSpeed, Dt);
	SlideCameraWeight = Approach(SlideCameraWeight, bSlide && bEnableSlideCameraEffects ? 1.0f : 0.0f,
		bSlide ? SlideCameraTransitionSpeed : SlideJumpCameraRestoreSpeed, Dt);
	AirCameraWeight = Approach(AirCameraWeight, bAir && bEnableAirborneCameraFeedback ? 1.0f : 0.0f, AirborneCameraBlendSpeed, Dt);
	SprintCameraMotionWeight = Approach(SprintCameraMotionWeight, bGround && bEnableSprintMotion ? TravelWeight : 0.0f,
		SprintCameraMotionBlendSpeed, Dt);

	// Phase follows distance, rather than a timer that keeps bobbing when stopped.
	const float Distance = bHasCameraHistory ? FVector::Dist2D(GetActorLocation(), PreviousCameraLocation) : Speed * Dt;
	if (bGround || bWall)
	{
		const float Stride = Sprint / FMath::Clamp(bWall ? WallRunCameraMotionFrequency : SprintCameraMotionFrequency, 0.5f, 4.0f);
		SprintCameraMotionPhase = FMath::Fmod(SprintCameraMotionPhase
			+ 2.0f * PI * FMath::Min(Distance, Speed * Dt * 1.5f) / Stride, 2.0f * PI);
	}
	FVector Position = FVector::ZeroVector, Rotation = FVector::ZeroVector;
	if (bEnableCameraInertia)
	{
		Position -= Inertia.GetClampedToMaxSize(1.0f) * FMath::Clamp(CameraInertiaDistance, 0.0f, 4.0f);
		Position -= Right * Turn * 0.45f;
		Rotation.X -= FMath::Clamp(FVector::DotProduct(Inertia, Forward), -1.0, 1.0) * FMath::Clamp(CameraInertiaPitch, 0.0f, 3.0f);
		Rotation.Z -= FMath::Clamp(FVector::DotProduct(Velocity, Right) / Sprint * 0.5
			+ FVector::DotProduct(Inertia, Right) * 0.5, -1.0, 1.0) * FMath::Clamp(CameraStrafeRoll, 0.0f, 4.0f);
		Rotation.Z -= Turn * FMath::Clamp(CameraTurnRoll, 0.0f, 3.0f);
	}
	const float Phase = SprintCameraMotionPhase;
	Position += FVector::UpVector * FMath::Cos(2.0f * Phase) * SprintCameraMotionWeight
		* FMath::Clamp(SprintCameraMotionAmplitude, 0.0f, 3.0f);
	Position += Right * FMath::Sin(Phase) * SprintCameraMotionWeight * SprintCameraMotionAmplitude * 0.35f;
	Position -= Velocity.GetSafeNormal2D() * SpeedWeight * SprintCameraMotionWeight * FMath::Clamp(SprintCameraMomentumOffset, 0.0f, 1.0f);
	Position += Forward * SlideCameraWeight * FMath::Clamp(SlideCameraForwardLean, 0.0f, 4.0f);
	Rotation.X -= SlideCameraWeight * FMath::Clamp(SlideCameraPitch, 0.0f, 5.0f);

	const float VerticalWeight = FMath::Clamp(Velocity.Z / FMath::Max(1.0f, AirborneCameraFullEffectSpeed), -1.0f, 1.0f);
	const float FallAcceleration = FMath::Clamp(FilteredCameraAcceleration.Z / FMath::Max(1.0f, FMath::Abs(Move->GetGravityZ())), -1.0, 1.0);
	Position.Z += AirCameraWeight * (-VerticalWeight * FMath::Clamp(AirborneCameraOffset, 0.0f, 3.0f) + FallAcceleration * 0.25f);
	Rotation.X += AirCameraWeight * (VerticalWeight + FallAcceleration * 0.3f) * FMath::Clamp(AirborneCameraPitch, 0.0f, 3.0f);

	const FVector TowardWall = -Move->GetWallRunNormal();
	const float WallSide = FMath::Clamp(FVector::DotProduct(TowardWall, Right), -1.0, 1.0);
	Rotation.Z -= WallSide * WallCameraWeight * FMath::Clamp(WallRunCameraTiltAngle, 0.0f, 20.0f);
	Position += TowardWall * WallCameraWeight * FMath::Clamp(WallRunCameraOffset, 0.0f, 8.0f);
	Position.Z += FMath::Sin(Phase) * WallCameraWeight * FMath::Clamp(WallRunCameraMotionAmplitude, 0.0f, 0.5f);
	if (bWall && !bWasCameraWallRunning && bEnableWallRunCameraEffects)
	{
		TriggerCameraEvent(ECameraEvent::WallContact, -FVector::UpVector * FMath::Clamp(WallRunCameraEntryImpulse, 0.0f, 2.0f),
			FVector(-0.4f, 0, 0), 0, WallRunCameraEntryDuration);
	}
	bWasCameraWallRunning = bWall;

	float GrappleFOV = 0.0f;
	if (bGrapple && bEnableGrappleCameraEffects)
	{
		const FVector ToAnchor = Move->GetGrappleAnchor() - GetActorLocation();
		const FVector Rope = ToAnchor.GetSafeNormal();
		const float Slack = FMath::Max(0.0f, Move->GetActiveGrappleRopeLength() - ToAnchor.Size());
		const FVector Tangent = FVector::VectorPlaneProject(Velocity, Rope);
		const float Swing = FMath::Clamp(Tangent.Size() / FMath::Max(1.0f, GrappleCameraFullEffectSpeed), 0.0f, 1.0f);
		// A cosmetic tension estimate: tautness + swing speed + inward force excluding gravity.
		const FVector SpecificForce = FilteredCameraAcceleration - FVector(0, 0, Move->GetGravityZ());
		const float Pull = FMath::Clamp(FVector::DotProduct(SpecificForce, Rope) / FMath::Max(1.0f, Move->GrapplePullAcceleration), 0.0, 1.0);
		const float Tension = (1.0f - FMath::Clamp(Slack / 12.0f, 0.0f, 1.0f)) * (0.2f + 0.5f * Swing + 0.3f * Pull);
		Position += Rope * Tension * GrappleCameraWeight * FMath::Clamp(GrappleTensionCameraOffset, 0.0f, 4.0f);
		Rotation.X -= FMath::Clamp(FVector::DotProduct(SpecificForce, Forward) / 3000.0, -1.0, 1.0) * GrappleCameraWeight * 0.7f;
		Rotation.Z -= FMath::Clamp(FVector::DotProduct(Tangent, Right) / FMath::Max(1.0f, GrappleCameraFullEffectSpeed), -1.0, 1.0)
			* Tension * GrappleCameraWeight * FMath::Clamp(GrappleSwingCameraRoll, 0.0f, 4.0f);
		GrappleFOV = GrappleCameraWeight * FMath::Max(Swing, Tension) * FMath::Clamp(GrappleTensionFOVBoost, 0.0f, 12.0f);
	}

	const float SprintFOVTarget = bEnableSprintFOV ? SpeedWeight * FMath::Clamp(SprintCameraFOVBoost, 0.0f, 15.0f) : 0.0f;
	SmoothedSprintFOV = Approach(SmoothedSprintFOV, SprintFOVTarget,
		SprintFOVTarget > SmoothedSprintFOV ? SprintCameraBlendInSpeed : SprintCameraBlendOutSpeed, Dt);
	float FOV = FMath::Max3(SmoothedSprintFOV, WallCameraWeight * TravelWeight * FMath::Clamp(WallRunCameraFOVBoost, 0.0f, 15.0f), GrappleFOV);
	CameraStateFOV = FOV;
	const bool bEventEnabled =
		(CameraEventSource == ECameraEvent::Jump && bEnableJumpCameraFeedback)
		|| (CameraEventSource == ECameraEvent::Landing && (bCameraLandingWasWallJump
			? bEnableWallJumpCameraEffects && bEnableWallJumpLandingFeedback : bEnableLandingCameraFeedback))
		|| (CameraEventSource == ECameraEvent::WallJump && bEnableWallJumpCameraEffects)
		|| (CameraEventSource == ECameraEvent::WallContact && bEnableWallRunCameraEffects)
		|| (CameraEventSource == ECameraEvent::Grapple && bEnableGrappleCameraEffects);
	if (bEventEnabled)
	{
		FVector EventPosition, EventRotation;
		float EventFOV;
		CameraEvent.Sample(Now, EventPosition, EventRotation, EventFOV);
		Position += EventPosition;
		Rotation += EventRotation;
		FOV = FMath::Max(FOV, EventFOV);
	}
	// Fade screen-space banking when looking vertically to avoid a disorienting horizon.
	const float Horizon = FMath::Square(FMath::Cos(FMath::DegreesToRadians(ViewRotation.Pitch)));
	Rotation *= Horizon;
	const float Intensity = bEnableCameraEffects ? FMath::Clamp(CameraEffectsIntensity, 0.0f, 2.0f) : 0.0f;
	const float PositionLimit = FMath::Clamp(CameraMaxPositionOffset, 0.0f, 15.0f);
	const float PitchLimit = FMath::Clamp(CameraMaxPitch, 0.0f, 6.0f);
	const float RollLimit = FMath::Clamp(CameraMaxRoll, 0.0f, 15.0f);
	const float FOVLimit = FMath::Clamp(CameraMaxFOVBoost, 0.0f, 20.0f);
	Position = (Position * Intensity).GetClampedToMaxSize(PositionLimit);
	Rotation = FVector(FMath::Clamp(Rotation.X * Intensity, -double(PitchLimit), double(PitchLimit)), 0,
		FMath::Clamp(Rotation.Z * Intensity, -double(RollLimit), double(RollLimit)));
	CameraSpring(CameraPosition, CameraPositionVelocity, Position, CameraPositionFrequency, CameraSpringDampingRatio, Dt);
	CameraSpring(CameraRotation, CameraRotationVelocity, Rotation, CameraRotationFrequency, CameraSpringDampingRatio, Dt);
	CameraSpring(CameraFOV, CameraFOVVelocity, FMath::Clamp(FOV * Intensity, 0.0f, FOVLimit), CameraFOVFrequency, CameraSpringDampingRatio, Dt);
	if (PositionLimit <= UE_SMALL_NUMBER)
	{
		CameraPosition = CameraPositionVelocity = FVector::ZeroVector;
	}
	else if (CameraPosition.SizeSquared() > FMath::Square(PositionLimit))
	{
		CameraPosition = CameraPosition.GetClampedToMaxSize(PositionLimit);
		const FVector Normal = CameraPosition.GetSafeNormal();
		CameraPositionVelocity -= Normal * FMath::Max(0.0, FVector::DotProduct(CameraPositionVelocity, Normal));
	}
	LimitAxis(CameraRotation.X, CameraRotationVelocity.X, PitchLimit);
	LimitAxis(CameraRotation.Z, CameraRotationVelocity.Z, RollLimit);
	if (CameraFOV < 0.0f || CameraFOV > FOVLimit)
	{
		CameraFOV = FMath::Clamp(CameraFOV, 0.0f, FOVLimit);
		CameraFOVVelocity = 0;
	}
	PreviousCameraVelocity = Velocity;
	PreviousCameraLocation = GetActorLocation();
	PreviousCameraYaw = ViewRotation.Yaw;
	bHasCameraHistory = true;
}

FVector AFPPlayerCharacter::ResolveCameraCollision(const FVector& Origin, const FVector& Desired) const
{
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ParkourCamera), false, this);
	const FCollisionShape Probe = FCollisionShape::MakeSphere(FMath::Clamp(SlideCameraProbeRadius, 0.1f, 20.0f));
	FHitResult CameraHit, PawnHit;
	bool bHit = GetWorld()->SweepSingleByChannel(CameraHit, Origin, Desired, FQuat::Identity, ECC_Camera, Probe, Params);
	FCollisionResponseParams Response;
	GetCapsuleComponent()->InitSweepCollisionParams(Params, Response);
	if (GetWorld()->SweepSingleByChannel(PawnHit, Origin, Desired, FQuat::Identity,
		GetCapsuleComponent()->GetCollisionObjectType(), Probe, Params, Response) && (!bHit || PawnHit.Time < CameraHit.Time))
	{
		CameraHit = PawnHit;
		bHit = true;
	}
	return bHit ? (CameraHit.bStartPenetrating ? Origin : CameraHit.Location) : Desired;
}

void AFPPlayerCharacter::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	const double Now = GetWorld()->GetTimeSeconds();
	float Dt = LastCameraUpdateTime < 0.0 ? FMath::Max(0.0f, DeltaTime) : FMath::Max(0.0, Now - LastCameraUpdateTime);
	const bool bDiscontinuity = LastCameraUpdateTime > Now || (bHasCameraHistory &&
		FVector::Dist(GetActorLocation(), PreviousCameraLocation) > FMath::Max(600.0f, GetVelocity().Size() * Dt * 2.0f));
	if (bDiscontinuity)
	{
		ResetCameraFeedback();
		Dt = 0.0f;
	}
	// Preserve native crouch-resize compensation. All extra feedback lives in OutResult.
	const float Reduction = StandingCapsuleHalfHeight - GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	const float Lowering = bEnableSlideCameraEffects ? FMath::Clamp(SlideCameraLowering, 0.0f, 110.0f) : Reduction;
	const float TargetZ = StandingCameraLocation.Z + Reduction - (bIsCrouched ? Lowering : 0.0f);
	const float Rate = bIsCrouched ? SlideCameraTransitionSpeed
		: ParkourMovementComponent->IsFalling() ? SlideJumpCameraRestoreSpeed : SlideCameraRestoreSpeed;
	SmoothedCameraZ = bDiscontinuity ? TargetZ : Approach(SmoothedCameraZ, TargetZ, Rate, Dt);
	const FTransform Capsule = GetCapsuleComponent()->GetComponentTransform();
	FVector Eye = StandingCameraLocation;
	Eye.Z = SmoothedCameraZ;
	const FVector BaseEye = ResolveCameraCollision(Capsule.GetLocation(), Capsule.TransformPosition(Eye));
	FirstPersonCameraComponent->SetRelativeLocation(Capsule.InverseTransformPosition(BaseEye));
	Super::CalcCamera(DeltaTime, OutResult);
	// CalcCamera may be requested more than once per world frame. Never advance twice.
	if (Dt > 0.0f) { UpdateParkourCamera(Dt, Now, OutResult.Rotation); }
	LastCameraUpdateTime = Now;
	CameraCollisionOffset *= FMath::Exp(-FMath::Clamp(CameraCollisionReleaseSpeed, 1.0f, 30.0f) * Dt);
	const FVector Desired = OutResult.Location + CameraPosition + CameraCollisionOffset;
	const FVector Safe = ResolveCameraCollision(Capsule.GetLocation(), Desired);
	CameraCollisionOffset += Safe - Desired;
	OutResult.Location = Safe;
	// Also gate the rendered response so rapidly looking straight up cannot carry a
	// residual spring tilt through the vertical axis. The base mouse view is untouched.
	const float ViewComfort = FMath::Square(FMath::Cos(FMath::DegreesToRadians(OutResult.Rotation.Pitch)));
	OutResult.Rotation.Pitch = FRotator::NormalizeAxis(OutResult.Rotation.Pitch + CameraRotation.X * ViewComfort);
	OutResult.Rotation.Roll = FRotator::NormalizeAxis(OutResult.Rotation.Roll + CameraRotation.Z * ViewComfort);
	const float MinFOV = FMath::Clamp(CameraMinFOV, 50.0f, 120.0f);
	const float MaxFOV = FMath::Max(MinFOV, FMath::Clamp(CameraMaxFOV, 70.0f, 140.0f));
	OutResult.FOV = FMath::Clamp(OutResult.FOV + CameraFOV, MinFOV, MaxFOV);
	OutResult.FirstPersonFOV = FMath::Clamp(OutResult.FirstPersonFOV + CameraFOV, MinFOV, MaxFOV);
}

void AFPPlayerCharacter::RestoreCameraAfterCapsuleResize(const FVector& PreviousEyeLocation)
{
	const FVector Restored = GetCapsuleComponent()->GetComponentTransform().InverseTransformPosition(PreviousEyeLocation);
	SmoothedCameraZ += Restored.Z - FirstPersonCameraComponent->GetRelativeLocation().Z;
	FirstPersonCameraComponent->SetRelativeLocation(Restored);
}
