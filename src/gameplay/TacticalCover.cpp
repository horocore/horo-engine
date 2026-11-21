#include "Horo/Gameplay/TacticalCover.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::Gameplay {
    namespace {
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Issues a process-local generation without reusing a retired provider identity. */
        TacticalCoverProviderGeneration IssueProviderGeneration() noexcept {
            static std::atomic_uint64_t next{1};
            auto value = next.load(std::memory_order_relaxed);
            while (value != std::numeric_limits<std::uint64_t>::max())
                if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
                    return TacticalCoverProviderGeneration::Create(value).Value();
            return {};
        }

        /** @brief Checks the complete candidate descriptor before inserting any new point. */
        bool ValidPoint(const Runtime::RuntimeSceneView &scene, const TacticalCoverPoint &point) {
            return point.id.IsValid() && scene.Get(point.anchor).HasValue() && point.stance < TacticalCoverStance::Count &&
                   Math::IsFinite(point.direction) && std::abs(Math::LengthSquared(point.direction) - 1.0F) < 0.001F;
        }
    }  // namespace

    TacticalCoverRegistry::TacticalCoverRegistry() noexcept : provider_(IssueProviderGeneration()) {}

    bool TacticalCoverRegistry::OnOwnerLane() const noexcept {
        return std::this_thread::get_id() == owner_;
    }

    /** @copydoc TacticalCoverRegistry::IsCurrent */
    bool TacticalCoverRegistry::IsCurrent(const Runtime::RuntimeSceneView &scene) const noexcept {
        return scene.IsCurrent() && scene.RuntimeId() == scene_;
    }

    /** @copydoc TacticalCoverRegistry::Find */
    std::vector<TacticalCoverRegistry::Entry>::const_iterator TacticalCoverRegistry::Find(const TacticalCoverHandle cover) const noexcept {
        if (cover.provider != provider_)
            return points_.end();
        const auto found = std::ranges::lower_bound(points_, cover.id, {}, [](const Entry &entry) {
            return entry.candidate.handle.id;
        });
        return found != points_.end() && found->candidate.handle == cover ? found : points_.end();
    }

    /** @copydoc TacticalCoverRegistry::IsOccupied */
    bool TacticalCoverRegistry::IsOccupied(const Runtime::RuntimeSceneView &scene, const Entry &entry) {
        return entry.reservation && scene.Get(entry.reservation->occupant).HasValue();
    }

    /** @copydoc TacticalCoverRegistry::Publish */
    Result<void> TacticalCoverRegistry::Publish(const Runtime::RuntimeSceneView &scene, const std::uint64_t revision,
                                                const std::span<const TacticalCoverPoint> points) {
        if (!OnOwnerLane())
            return Failure<void>(AI::AIErrors::EnvironmentQueryContextInvalid);
        if (!provider_.IsValid())
            return Failure<void>(AI::AIErrors::EnvironmentQueryLimitExceeded);
        if (!scene.IsCurrent() || (scene_.IsValid() && scene.RuntimeId() != scene_) || revision <= revision_)
            return Failure<void>(AI::AIErrors::EnvironmentQueryContextStale);
        if (points.size() > MaximumPoints)
            return Failure<void>(AI::AIErrors::EnvironmentQueryLimitExceeded);
        try {
            std::vector<Entry> candidate;
            candidate.reserve(points.size());
            for (const auto &point : points) {
                if (!ValidPoint(scene, point))
                    return Failure<void>(AI::AIErrors::EnvironmentQueryContextInvalid);
                candidate.push_back({{point, {point.id, point.anchor, revision, provider_}}, {}});
            }
            const auto id = [](const Entry &entry) {
                return entry.candidate.point.id;
            };
            std::ranges::sort(candidate, {}, id);
            if (std::ranges::adjacent_find(candidate, {}, id) != candidate.end())
                return Failure<void>(AI::AIErrors::EnvironmentQueryIdentityConflict);
            for (auto &entry : candidate) {
                const auto previous = std::ranges::lower_bound(points_, id(entry), {}, id);
                if (previous != points_.end() && previous->candidate.point == entry.candidate.point) {
                    entry.candidate.handle = previous->candidate.handle;
                    if (IsOccupied(scene, *previous))
                        entry.reservation = previous->reservation;
                }
            }
            points_.swap(candidate);
            scene_ = scene.RuntimeId();
            revision_ = revision;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Failure<void>(AI::AIErrors::EnvironmentQueryLimitExceeded);
        }
    }

    /** @copydoc TacticalCoverRegistry::Capture */
    Result<std::vector<TacticalCoverCandidate>> TacticalCoverRegistry::Capture(const Runtime::RuntimeSceneView &scene,
                                                                               const std::size_t maximumItems) const {
        using Candidates = std::vector<TacticalCoverCandidate>;
        if (!OnOwnerLane())
            return Failure<Candidates>(AI::AIErrors::EnvironmentQueryContextInvalid);
        if (!IsCurrent(scene))
            return Failure<Candidates>(AI::AIErrors::EnvironmentQueryContextStale);
        if (maximumItems == 0 || maximumItems > AI::EnvironmentQuerySchemaLimits::MaximumResults)
            return Failure<Candidates>(AI::AIErrors::EnvironmentQueryLimitExceeded);
        try {
            Candidates candidates;
            candidates.reserve(std::min(points_.size(), maximumItems));
            for (const auto &entry : points_) {
                if (scene.Get(entry.candidate.handle.anchor).HasError())
                    continue;
                if (candidates.size() == maximumItems)
                    return Failure<Candidates>(AI::AIErrors::EnvironmentQueryLimitExceeded);
                candidates.push_back(entry.candidate);
            }
            return Result<Candidates>::Success(std::move(candidates));
        } catch (const std::bad_alloc &) {
            return Failure<Candidates>(AI::AIErrors::EnvironmentQueryLimitExceeded);
        }
    }

    Result<TacticalCoverCandidate> TacticalCoverRegistry::Resolve(const Runtime::RuntimeSceneView &scene,
                                                                  const TacticalCoverHandle cover) const {
        if (!OnOwnerLane())
            return Failure<TacticalCoverCandidate>(AI::AIErrors::EnvironmentQueryContextInvalid);
        const auto entry = Find(cover);
        if (!IsCurrent(scene) || entry == points_.end() || scene.Get(cover.anchor).HasError())
            return Failure<TacticalCoverCandidate>(AI::AIErrors::EnvironmentQueryContextStale);
        return Result<TacticalCoverCandidate>::Success(entry->candidate);
    }

    /** @copydoc TacticalCoverRegistry::Score */
    Result<std::array<AI::QueryTestSample, 3>> TacticalCoverRegistry::Score(const Runtime::RuntimeSceneView &scene,
                                                                            const TacticalCoverHandle cover,
                                                                            AI::QueryTestSample reachability,
                                                                            AI::QueryTestSample threatVisibility,
                                                                            const AI::QueryStageId contentionStage) const {
        using Samples = std::array<AI::QueryTestSample, 3>;
        if (!OnOwnerLane())
            return Failure<Samples>(AI::AIErrors::EnvironmentQueryContextInvalid);
        const auto entry = Find(cover);
        if (!IsCurrent(scene) || entry == points_.end() || scene.Get(cover.anchor).HasError())
            return Failure<Samples>(AI::AIErrors::EnvironmentQueryContextStale);
        if (!reachability.stage.IsValid() || !threatVisibility.stage.IsValid() || !contentionStage.IsValid() ||
            reachability.stage == threatVisibility.stage || reachability.stage == contentionStage ||
            threatVisibility.stage == contentionStage)
            return Failure<Samples>(AI::AIErrors::EnvironmentQuerySchemaInvalid);
        if (threatVisibility.filter)
            threatVisibility.filter = !*threatVisibility.filter;
        Samples samples{reachability, threatVisibility,
                        AI::QueryTestSample{.stage = contentionStage, .filter = !IsOccupied(scene, *entry)}};
        std::ranges::sort(samples, {}, &AI::QueryTestSample::stage);
        return Result<Samples>::Success(samples);
    }

    /** @copydoc TacticalCoverRegistry::TryReserve */
    Result<std::optional<TacticalCoverReservation>> TacticalCoverRegistry::TryReserve(const Runtime::RuntimeSceneView &scene,
                                                                                      const TacticalCoverHandle cover,
                                                                                      const Runtime::EntityRef occupant) {
        using Receipt = std::optional<TacticalCoverReservation>;
        if (!OnOwnerLane())
            return Failure<Receipt>(AI::AIErrors::EnvironmentQueryContextInvalid);
        const auto entry = Find(cover);
        if (!IsCurrent(scene) || entry == points_.end() || scene.Get(cover.anchor).HasError() || scene.Get(occupant).HasError())
            return Failure<Receipt>(AI::AIErrors::EnvironmentQueryContextStale);
        if (IsOccupied(scene, *entry))
            return Result<Receipt>::Success(entry->reservation->occupant == occupant ? entry->reservation : std::nullopt);
        if (nextReservation_ == std::numeric_limits<std::uint64_t>::max())
            return Failure<Receipt>(AI::AIErrors::EnvironmentQueryLimitExceeded);
        auto &slot = points_[static_cast<std::size_t>(entry - points_.cbegin())];
        slot.reservation = TacticalCoverReservation{cover, occupant, ++nextReservation_};
        return Result<Receipt>::Success(slot.reservation);
    }

    /** @copydoc TacticalCoverRegistry::Release */
    bool TacticalCoverRegistry::Release(const TacticalCoverReservation receipt) noexcept {
        if (!OnOwnerLane())
            return false;
        const auto entry = Find(receipt.cover);
        if (entry == points_.end() || entry->reservation != receipt)
            return false;
        points_[static_cast<std::size_t>(entry - points_.cbegin())].reservation.reset();
        return true;
    }
}  // namespace Horo::Gameplay
