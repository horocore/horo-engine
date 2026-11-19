#include "Horo/XR/XRLocomotionPolicy.h"

#include "Horo/XR/XRErrors.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <numbers>
#include <utility>

namespace Horo::XR {
    namespace {
        using Operation = std::optional<XRLocomotionOperation>;
        constexpr double NanosecondsPerSecond = 1'000'000'000.0;
        constexpr Math::Vec3 Up{0.0F, 1.0F, 0.0F};

        /** @brief Preserves typed XR diagnostics at the policy boundary. */
        template <typename T> Result<T> RejectPolicy(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Tests an authored finite positive bound. */
        bool Positive(float value) noexcept {
            return std::isfinite(value) && value > 0.0F;
        }

        /** @brief Checks the signed normalized action range, including zero. */
        bool UnitAxis(float value) noexcept {
            return std::abs(value) <= 1.0F;
        }

        /** @brief Checks that the current profile has finite values and distinct hysteresis thresholds. */
        bool ValidTurn(const XRSnapTurn &turn) noexcept {
            return Positive(turn.angleRadians) && turn.angleRadians <= std::numbers::pi_v<float> && Positive(turn.engage) &&
                   turn.engage <= 1.0F && turn.release >= 0.0F && turn.release < turn.engage;
        }

        /** @brief Checks authored bounds for modes that consume fresh pose samples. */
        bool ValidPhysicalProfile(float speed, float shapeLimit, std::int64_t trackingGap) noexcept {
            return Positive(speed) && Positive(shapeLimit) && trackingGap > 0;
        }

        /** @brief Admits profile settings before replacing previous valid state. */
        bool ValidProfile(const XRLocomotionProfile &profile) {
            if (profile.turn && !ValidTurn(*profile.turn))
                return false;
            if (const auto *move = std::get_if<XRSmoothMove>(&profile.translation))
                return Positive(move->maximumSpeed) && move->deadZone >= 0.0F && move->deadZone < 1.0F;
            if (const auto *move = std::get_if<XRArmSwingMove>(&profile.translation))
                return ValidPhysicalProfile(move->maximumSpeed, move->gain, profile.maximumTrackingGapNanoseconds);
            if (const auto *move = std::get_if<XRRoomScaleMove>(&profile.translation))
                return ValidPhysicalProfile(move->maximumSpeed, move->maximumSampleDistance, profile.maximumTrackingGapNanoseconds);
            return !profile.translation.valueless_by_exception();
        }

        /** @brief Checks committed action values and provided calibrated pose coordinates. */
        bool ValidSample(const XRLocomotionSample &sample) noexcept {
            const auto finite = [](const std::optional<Math::Vec3> &position) {
                return !position || Math::IsFinite(*position);
            };
            return sample.fixedStepNanoseconds > 0 && UnitAxis(sample.move.x) && UnitAxis(sample.move.y) && UnitAxis(sample.turn) &&
                   std::abs(sample.gameplayYawRadians) <= std::numbers::pi_v<float> && finite(sample.head) && finite(sample.leftHand) &&
                   finite(sample.rightHand);
        }

        /** @brief Turns off policy emission on focus, visibility or tracking loss. */
        bool Available(const XRTrackingSnapshot &tracking) noexcept {
            return tracking.Focus() == XRTrackingFocusState::Focused && tracking.Visibility() == XRTrackingVisibilityState::Visible &&
                   tracking.State() == XRTrackingState::Active;
        }

        /** @brief Tests whether two observations describe the same calibrated spatial and action binding. */
        bool SameReference(const XRInteractionCutoff &left, const XRInteractionCutoff &right) noexcept {
            return left.session == right.session && left.origin == right.origin && left.bindingRevision == right.bindingRevision;
        }

        /** @brief Preserves the authored speed bound after rounding a float direction. */
        Math::Vec3 BoundVelocity(Math::Vec3 velocity, float limit) noexcept {
            const double length = std::hypot(static_cast<double>(velocity.x), static_cast<double>(velocity.z));
            if (length <= limit)
                return velocity;
            return velocity * static_cast<float>(std::nextafter(limit, 0.0F) / length);
        }

        /** @brief Projects a radial, rescaled stick magnitude onto gameplay heading. */
        Math::Vec3 SmoothVelocity(const XRSmoothMove &policy, Math::Vec2 move, Math::Quaternion heading) noexcept {
            const float magnitude = Math::Length(move);
            if (magnitude <= policy.deadZone)
                return {};
            const float scale = (std::min(magnitude, 1.0F) - policy.deadZone) / (1.0F - policy.deadZone);
            const auto direction = heading.Rotate({move.x / magnitude, 0.0F, -move.y / magnitude});
            return BoundVelocity(direction * (scale * policy.maximumSpeed), policy.maximumSpeed);
        }

        /** @brief Projects one calibrated hand displacement at double precision without a float subtraction overflow. */
        double ForeAft(Math::Vec3 previous, Math::Vec3 current, Math::Vec3 forward) noexcept {
            const double x = static_cast<double>(current.x) - previous.x;
            const double z = static_cast<double>(current.z) - previous.z;
            return x * forward.x + z * forward.z;
        }

        /** @brief Pairs opposite arm movement and bounds its forward speed. */
        Math::Vec3 SwingVelocity(const XRArmSwingMove &policy, Math::Vec3 forward, double left, double right,
                                 std::int64_t elapsed) noexcept {
            if ((left > 0.0) == (right > 0.0))
                return {};
            const double paired = std::min(std::abs(left), std::abs(right));
            const double speed = paired * NanosecondsPerSecond / static_cast<double>(elapsed) * policy.gain;
            return BoundVelocity(forward * static_cast<float>(std::min(speed, static_cast<double>(policy.maximumSpeed))),
                                 policy.maximumSpeed);
        }

        /** @brief Converts one horizontal tracking-space delta into bounded velocity for the destination fixed tick. */
        Math::Vec3 RoomVelocity(const XRRoomScaleMove &policy, Math::Vec3 previous, Math::Vec3 current, std::int64_t tick) noexcept {
            const double x = static_cast<double>(current.x) - previous.x;
            const double z = static_cast<double>(current.z) - previous.z;
            const double distance = std::hypot(x, z);
            if (distance == 0.0 || distance > policy.maximumSampleDistance)
                return {};
            const double speed =
                std::min(distance * NanosecondsPerSecond / static_cast<double>(tick), static_cast<double>(policy.maximumSpeed));
            return BoundVelocity({static_cast<float>(x / distance * speed), 0.0F, static_cast<float>(z / distance * speed)},
                                 policy.maximumSpeed);
        }

        /** @brief Produces one absolute heading on an engage crossing after observed release. */
        std::optional<Math::Quaternion> SnapHeading(const std::optional<XRSnapTurn> &turn, float action, float heading,
                                                    bool &armed) noexcept {
            if (!turn)
                return {};
            if (std::abs(action) <= turn->release)
                armed = true;
            if (std::abs(action) < turn->engage || !std::exchange(armed, false))
                return {};
            return Math::Quaternion::FromAxisAngle(Up, heading + std::copysign(turn->angleRadians, action));
        }

        /** @brief Applies the neutral transition without accepting a held action as a new edge. */
        Operation NeutralOperation() {
            return XRLocomotionMotion{.velocityMetersPerSecond = Math::Vec3{}};
        }

        /** @brief Checks that the gameplay query did not return a foreign spatial epoch or malformed placement. */
        bool ValidDestination(const XRLocomotionTeleport &target, const XRInteractionCutoff &cutoff) noexcept {
            const auto &space = target.space;
            const Math::Vec4 heading{target.heading.x, target.heading.y, target.heading.z, target.heading.w};
            return target.destinationRevision != 0 && space.id.IsValid() && space.id.session == cutoff.session &&
                   space.kind == XRSpaceKind::World && space.worldOriginRevision == cutoff.origin && Math::IsFinite(target.position) &&
                   std::abs(Math::LengthSquared(heading) - 1.0F) <= 0.001F;
        }
    }  // namespace

    XRLocomotionPolicy::XRLocomotionPolicy(XRLocomotionProfile profile, XRSessionId session, Input::PlayerId player,
                                           std::uint64_t assignmentRevision, std::weak_ptr<const IXRLocomotionDestinationOwner> destination)
        : profile_(std::move(profile)), session_(session), player_(player), assignmentRevision_(assignmentRevision),
          destination_(std::move(destination)) {}

    /** @copydoc XRLocomotionPolicy::Create */
    Result<XRLocomotionPolicy> XRLocomotionPolicy::Create(XRLocomotionProfile profile, XRSessionId session, Input::PlayerId player,
                                                          std::uint64_t assignmentRevision,
                                                          std::weak_ptr<const IXRLocomotionDestinationOwner> destination) {
        if (!session.IsValid() || assignmentRevision == 0 || !ValidProfile(profile))
            return RejectPolicy<XRLocomotionPolicy>(XRErrors::OperationInvalid);
        return Result<XRLocomotionPolicy>::Success({std::move(profile), session, player, assignmentRevision, std::move(destination)});
    }

    /** @copydoc XRLocomotionPolicy::SetProfile */
    Result<void> XRLocomotionPolicy::SetProfile(XRLocomotionProfile profile) {
        if (stopped_ || evaluating_)
            return RejectPolicy<void>(XRErrors::OperationUnavailable);
        if (!ValidProfile(profile))
            return RejectPolicy<void>(XRErrors::OperationInvalid);
        profile_ = std::move(profile);
        history_ = {};
        return Result<void>::Success();
    }

    /** @brief Resolves at most one teleport edge through the installed owner. */
    Result<std::optional<XRLocomotionTeleport>> XRLocomotionPolicy::SelectTeleport(const XRLocomotionSample &sample, History &next) const {
        using Destination = std::optional<XRLocomotionTeleport>;
        if (!std::holds_alternative<XRTeleportMove>(profile_.translation))
            return Result<Destination>::Success({});
        if (!sample.teleportPressed) {
            next.teleportArmed = true;
            return Result<Destination>::Success({});
        }
        if (!std::exchange(next.teleportArmed, false))
            return Result<Destination>::Success({});
        const auto owner = destination_.lock();
        if (!owner)
            return RejectPolicy<Destination>(XRErrors::OperationUnsupported);
        auto result = owner->Resolve(sample);
        if (result.HasValue() && result.Value() && !ValidDestination(*result.Value(), sample.scope.tracking))
            return RejectPolicy<Destination>(XRErrors::CoordinateSpaceIncompatible);
        return result;
    }

    /** @brief Resolves the selected translation and combines it with the current turn edge. */
    Result<Operation> XRLocomotionPolicy::Select(const XRLocomotionSample &sample, History &next) const {
        const auto &cutoff = sample.scope.tracking;
        const auto elapsed = cutoff.sampledAt.Nanoseconds() - next.cutoff.sampledAt.Nanoseconds();
        if (elapsed < 0)
            return RejectPolicy<Operation>(XRErrors::TrackingSnapshotStale);
        auto destination = SelectTeleport(sample, next);
        if (destination.HasError())
            return Result<Operation>::Failure(destination.ErrorValue());
        if (destination.Value()) {
            next.snapArmed = false;
            return Result<Operation>::Success(*std::move(destination).Value());
        }
        const auto turn = SnapHeading(profile_.turn, sample.turn, sample.gameplayYawRadians, next.snapArmed);
        const auto velocity = Velocity(sample, next, elapsed);
        if (!velocity && !turn)
            return Result<Operation>::Success({});
        return Result<Operation>::Success(XRLocomotionMotion{velocity, turn});
    }

    /** @brief Selects one horizontal translation policy without accumulating an input backlog. */
    std::optional<Math::Vec3> XRLocomotionPolicy::Velocity(const XRLocomotionSample &sample, const History &previous,
                                                           std::int64_t elapsed) const noexcept {
        if (const auto *policy = std::get_if<XRSmoothMove>(&profile_.translation)) {
            const auto heading = Math::Quaternion::FromAxisAngle(Up, sample.gameplayYawRadians);
            return SmoothVelocity(*policy, sample.move, heading);
        }
        const bool fresh = elapsed > 0 && elapsed <= profile_.maximumTrackingGapNanoseconds;
        if (const auto *policy = std::get_if<XRRoomScaleMove>(&profile_.translation)) {
            return fresh && previous.head && sample.head ? RoomVelocity(*policy, *previous.head, *sample.head, sample.fixedStepNanoseconds)
                                                         : Math::Vec3{};
        }
        if (const auto *policy = std::get_if<XRArmSwingMove>(&profile_.translation))
            return fresh ? ArmVelocity(*policy, sample, previous, elapsed) : Math::Vec3{};
        return {};
    }

    /** @brief Requires tracked positions from both hands before deriving paired movement. */
    Math::Vec3 XRLocomotionPolicy::ArmVelocity(const XRArmSwingMove &policy, const XRLocomotionSample &sample, const History &previous,
                                               std::int64_t elapsed) noexcept {
        if (!previous.leftHand || !previous.rightHand || !sample.leftHand || !sample.rightHand)
            return {};
        const auto forward = Math::Quaternion::FromAxisAngle(Up, sample.gameplayYawRadians).Rotate({0.0F, 0.0F, -1.0F});
        return SwingVelocity(policy, forward, ForeAft(*previous.leftHand, *sample.leftHand, forward),
                             ForeAft(*previous.rightHand, *sample.rightHand, forward), elapsed);
    }

    /** @brief Fences owner lifecycle, tick ordering, player assignment and the current publication. */
    Result<void> XRLocomotionPolicy::ValidateScope(const XRLocomotionSample &sample, const XRTrackingSnapshot &tracking) const {
        if (stopped_ || evaluating_)
            return RejectPolicy<void>(XRErrors::OperationUnavailable);
        if (sample.tick <= tick_ || sample.sequence <= sequence_ || sample.scope.player != player_ ||
            sample.scope.assignmentRevision != assignmentRevision_)
            return RejectPolicy<void>(XRErrors::IdentityStale);
        if (const auto &cutoff = sample.scope.tracking; cutoff.session != session_ || cutoff != DescribeXRInteractionCutoff(tracking))
            return RejectPolicy<void>(XRErrors::TrackingSnapshotStale);
        return Result<void>::Success();
    }

    /** @brief Keeps tentative state private and contains foreign callback failures. */
    Result<Operation> XRLocomotionPolicy::ResolveOperation(const XRLocomotionSample &sample, History &next) {
        if (next.neutralRequired) {
            next.snapArmed = profile_.turn && std::abs(sample.turn) <= profile_.turn->release;
            next.teleportArmed = !sample.teleportPressed;
            return Result<Operation>::Success(NeutralOperation());
        }
        evaluating_ = true;
        try {
            auto operation = Select(sample, next);
            evaluating_ = false;
            return operation;
        } catch (const std::bad_alloc &) {
            evaluating_ = false;
            return RejectPolicy<Operation>(XRErrors::CapacityExceeded);
        } catch (...) {
            // Foreign query owners may throw a non-standard exception.
            evaluating_ = false;
            return RejectPolicy<Operation>(XRErrors::OperationUnavailable);
        }
    }

    /** @brief Publishes only a successful active observation; repeated poses never become a new baseline. */
    void XRLocomotionPolicy::Commit(const XRLocomotionSample &sample, History &next) {
        if (const auto &cutoff = sample.scope.tracking; next.neutralRequired || cutoff.sampledAt != next.cutoff.sampledAt) {
            next.cutoff = cutoff;
            next.head = sample.head;
            next.leftHand = sample.leftHand;
            next.rightHand = sample.rightHand;
        }
        next.neutralRequired = false;
        history_ = std::move(next);
        tick_ = sample.tick;
        sequence_ = sample.sequence;
    }

    /** @copydoc XRLocomotionPolicy::Evaluate */
    Result<std::optional<XRLocomotionIntent>> XRLocomotionPolicy::Evaluate(const XRLocomotionSample &sample,
                                                                           const XRTrackingSnapshot &tracking) {
        using Intent = std::optional<XRLocomotionIntent>;
        if (auto valid = ValidateScope(sample, tracking); valid.HasError())
            return Result<Intent>::Failure(valid.ErrorValue());
        if (!Available(tracking)) {
            history_ = {};
            tick_ = sample.tick;
            sequence_ = sample.sequence;
            return Result<Intent>::Success({});
        }
        if (!ValidSample(sample))
            return RejectPolicy<Intent>(XRErrors::OperationInvalid);
        auto next = SameReference(history_.cutoff, sample.scope.tracking) ? history_ : History{};
        auto operation = ResolveOperation(sample, next);
        if (stopped_)
            return RejectPolicy<Intent>(XRErrors::OperationUnavailable);
        if (operation.HasError())
            return Result<Intent>::Failure(operation.ErrorValue());
        Commit(sample, next);
        if (operation.Value())
            return Result<Intent>::Success(XRLocomotionIntent{sample.scope, sample.tick, sample.sequence, *std::move(operation).Value()});
        return Result<Intent>::Success({});
    }

    /** @copydoc XRLocomotionPolicy::Shutdown */
    void XRLocomotionPolicy::Shutdown() noexcept {
        history_ = {};
        stopped_ = true;
        destination_.reset();
    }
}  // namespace Horo::XR
