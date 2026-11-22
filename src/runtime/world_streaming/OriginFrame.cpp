#include "Horo/WorldStreaming/OriginFrame.h"

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::WorldStreaming {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        template <typename Identity> [[nodiscard]] Result<Identity> NextIdentity(const Identity current) {
            if (!current.IsValid())
                return Failure<Identity>(WorldStreamingErrors::IdentityInvalid);
            if (current.Value() == std::numeric_limits<std::uint64_t>::max())
                return Failure<Identity>(WorldStreamingErrors::GenerationExhausted);
            return Identity::Create(current.Value() + 1);
        }

        [[nodiscard]] bool LocalAxisIsValid(const float meters) noexcept {
            if (!std::isfinite(meters) || std::abs(meters) > static_cast<float>(OriginFrame::MaximumLocalHalfExtentMillimeters) / 1000.0F)
                return false;
            const double millimeters = static_cast<double>(meters) * 1000.0;
            const auto roundedMillimeters = static_cast<std::int64_t>(std::llround(millimeters));
            const float reconstructed = static_cast<float>(roundedMillimeters) / 1000.0F;
            if (std::fpclassify(meters) == FP_ZERO)
                return true;
            return std::bit_cast<std::uint32_t>(reconstructed) == std::bit_cast<std::uint32_t>(meters);
        }

        [[nodiscard]] Result<std::int64_t> CheckedDelta(const std::int64_t global, const std::int64_t origin) {
            const bool positive = global >= origin;
            const std::uint64_t magnitude = positive ? static_cast<std::uint64_t>(global) - static_cast<std::uint64_t>(origin)
                                                     : static_cast<std::uint64_t>(origin) - static_cast<std::uint64_t>(global);
            if (magnitude > static_cast<std::uint64_t>(OriginFrame::MaximumLocalHalfExtentMillimeters))
                return Failure<std::int64_t>(WorldStreamingErrors::OriginFrameRangeExceeded);
            const auto signedMagnitude = static_cast<std::int64_t>(magnitude);
            return Result<std::int64_t>::Success(positive ? signedMagnitude : -signedMagnitude);
        }

        [[nodiscard]] Result<std::int64_t> CheckedCoordinate(const std::int64_t origin, const float localMeters) {
            const double scaled = static_cast<double>(localMeters) * 1000.0;
            const auto delta = static_cast<std::int64_t>(std::llround(scaled));
            if ((delta > 0 && origin > std::numeric_limits<std::int64_t>::max() - delta) ||
                (delta < 0 && origin < std::numeric_limits<std::int64_t>::min() - delta))
                return Failure<std::int64_t>(WorldStreamingErrors::OriginFrameRangeExceeded);
            return Result<std::int64_t>::Success(origin + delta);
        }
    }  // namespace

    /** @copydoc NextOriginGeneration */
    Result<OriginGeneration> NextOriginGeneration(const OriginGeneration current) {
        return NextIdentity(current);
    }

    /** @copydoc NextOriginFrameRevision */
    Result<OriginFrameRevision> NextOriginFrameRevision(const OriginFrameRevision current) {
        return NextIdentity(current);
    }

    /** @copydoc OriginFrameBinding::IsValid */
    bool OriginFrameBinding::IsValid() const noexcept {
        return identity.IsValid() && revision.IsValid() && generation.IsValid();
    }

    /** @copydoc OriginLocalCoordinate::Create */
    Result<OriginLocalCoordinate> OriginLocalCoordinate::Create(const Math::Vec3 value, const OriginFrameId frame,
                                                                const OriginGeneration generation) {
        if (!frame.IsValid() || !generation.IsValid() || !Math::IsFinite(value))
            return Failure<OriginLocalCoordinate>(WorldStreamingErrors::OriginFrameInvalid);
        if (std::abs(value.x) > static_cast<float>(OriginFrame::MaximumLocalHalfExtentMillimeters) / 1000.0F ||
            std::abs(value.y) > static_cast<float>(OriginFrame::MaximumLocalHalfExtentMillimeters) / 1000.0F ||
            std::abs(value.z) > static_cast<float>(OriginFrame::MaximumLocalHalfExtentMillimeters) / 1000.0F)
            return Failure<OriginLocalCoordinate>(WorldStreamingErrors::OriginFrameRangeExceeded);
        if (!LocalAxisIsValid(value.x) || !LocalAxisIsValid(value.y) || !LocalAxisIsValid(value.z))
            return Failure<OriginLocalCoordinate>(WorldStreamingErrors::OriginFramePrecisionLoss);
        return Result<OriginLocalCoordinate>::Success(OriginLocalCoordinate{value, frame, generation});
    }

    /** @copydoc OriginFrame::Create */
    Result<OriginFrame> OriginFrame::Create(const OriginFrameBinding binding, const Math::WorldCoordinate64 &origin) {
        if (!binding.IsValid())
            return Failure<OriginFrame>(WorldStreamingErrors::OriginFrameInvalid);
        return Result<OriginFrame>::Success(OriginFrame{binding, origin});
    }

    /** @copydoc OriginFrame::ToLocal */
    Result<OriginLocalCoordinate> OriginFrame::ToLocal(const Math::WorldCoordinate64 &global) const {
        const auto globalMillimeters = global.Millimeters();
        const auto originMillimeters = origin_.Millimeters();
        std::array<std::int64_t, 3> delta{};
        for (std::size_t index = 0; index < delta.size(); ++index) {
            auto axis = CheckedDelta(globalMillimeters[index], originMillimeters[index]);
            if (axis.HasError())
                return Result<OriginLocalCoordinate>::Failure(axis.ErrorValue());
            delta[index] = axis.Value();
        }
        const Math::Vec3 local{static_cast<float>(delta[0]) / 1000.0F, static_cast<float>(delta[1]) / 1000.0F,
                               static_cast<float>(delta[2]) / 1000.0F};
        return Result<OriginLocalCoordinate>::Success(OriginLocalCoordinate{local, binding_.identity, binding_.generation});
    }

    /** @copydoc OriginFrame::ToGlobal */
    Result<Math::WorldCoordinate64> OriginFrame::ToGlobal(const OriginLocalCoordinate &local) const {
        if (local.Frame() != binding_.identity || local.Generation() != binding_.generation)
            return Failure<Math::WorldCoordinate64>(WorldStreamingErrors::OriginFrameStale);
        const auto originMillimeters = origin_.Millimeters();
        const auto value = local.Value();
        std::array<std::int64_t, 3> global{};
        const std::array axes{value.x, value.y, value.z};
        for (std::size_t index = 0; index < axes.size(); ++index) {
            auto axis = CheckedCoordinate(originMillimeters[index], axes[index]);
            if (axis.HasError())
                return Failure<Math::WorldCoordinate64>(WorldStreamingErrors::OriginFrameRangeExceeded);
            global[index] = axis.Value();
        }
        return Result<Math::WorldCoordinate64>::Success(Math::WorldCoordinate64::FromMillimeters(global[0], global[1], global[2]));
    }

    /** @copydoc OriginFrameLease::Get */
    Result<OriginFrame> OriginFrameLease::Get() const {
        if (!IsCurrent())
            return Failure<OriginFrame>(WorldStreamingErrors::OriginFrameStale);
        return Result<OriginFrame>::Success(frame_);
    }

    /** @copydoc OriginFrameOwner::Create */
    Result<std::unique_ptr<OriginFrameOwner>> OriginFrameOwner::Create(OriginFrame initial) {
        try {
            auto leaseState = std::make_shared<std::atomic_bool>(true);
            return Result<std::unique_ptr<OriginFrameOwner>>::Success(
                std::make_unique<OriginFrameOwner>(ConstructionKey{}, std::move(initial), std::move(leaseState)));
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<OriginFrameOwner>>(WorldStreamingErrors::OriginFrameStorageUnavailable);
        }
    }

    /** @copydoc OriginFrameOwner::Lease */
    Result<OriginFrameLease> OriginFrameOwner::Lease() const {
        if (!active_)
            return Failure<OriginFrameLease>(WorldStreamingErrors::OriginFrameLifecycleUnavailable);
        return Result<OriginFrameLease>::Success(OriginFrameLease{activeFrame_, leaseState_});
    }

    /** @copydoc OriginFrameOwner::StageReplacement */
    Result<void> OriginFrameOwner::StageReplacement(OriginFrame candidate) {
        if (!active_)
            return Result<void>::Failure(MakeError(WorldStreamingErrors::OriginFrameLifecycleUnavailable));
        const auto &active = activeFrame_.Binding();
        if (const auto &replacement = candidate.Binding();
            stagedFrame_.has_value() || replacement.identity != active.identity || NextOriginFrameRevision(active.revision).HasError() ||
            NextOriginGeneration(active.generation).HasError() || replacement.revision.Value() != active.revision.Value() + 1 ||
            replacement.generation.Value() != active.generation.Value() + 1)
            return Result<void>::Failure(MakeError(WorldStreamingErrors::OriginFrameStale));
        stagedFrame_ = std::move(candidate);
        return Result<void>::Success();
    }

    /** @copydoc OriginFrameOwner::CancelReplacement */
    void OriginFrameOwner::CancelReplacement() noexcept {
        stagedFrame_.reset();
    }

    /** @copydoc OriginFrameOwner::PublishReplacement */
    Result<void> OriginFrameOwner::PublishReplacement() {
        if (!active_ || !stagedFrame_.has_value())
            return Result<void>::Failure(MakeError(WorldStreamingErrors::OriginFrameLifecycleUnavailable));
        try {
            auto replacementLeaseState = std::make_shared<std::atomic_bool>(true);
            const auto previousLeaseState = leaseState_;
            activeFrame_ = *stagedFrame_;
            stagedFrame_.reset();
            leaseState_ = std::move(replacementLeaseState);
            previousLeaseState->store(false);
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(WorldStreamingErrors::OriginFrameStorageUnavailable));
        }
    }

    /** @copydoc OriginFrameOwner::Shutdown */
    void OriginFrameOwner::Shutdown() noexcept {
        if (!active_)
            return;
        active_ = false;
        stagedFrame_.reset();
        leaseState_->store(false);
    }

    /** @copydoc OriginFrameOwner::~OriginFrameOwner */
    OriginFrameOwner::~OriginFrameOwner() noexcept {
        Shutdown();
    }
}  // namespace Horo::WorldStreaming
