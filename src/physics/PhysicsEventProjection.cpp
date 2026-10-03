#include "PhysicsEventProjection.h"

#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsSaturatingAdd.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Checks one copied endpoint without resolving any live registry or native object. */
        [[nodiscard]] bool ValidEndpoint(const PhysicsEventEndpoint &endpoint) noexcept {
            return endpoint.body.IsValid() && endpoint.shape.IsValid() && endpoint.layer.IsValid() && endpoint.profile.IsValid() &&
                   endpoint.filterSchemaGeneration != 0 && endpoint.body.world == endpoint.shape.world &&
                   (!endpoint.subshape.has_value() || endpoint.subshape->IsValid());
        }

        /** @brief Checks optional material evidence copied from the callback. */
        [[nodiscard]] bool ValidMaterial(const std::optional<PhysicsEventMaterial> &material) noexcept {
            return !material.has_value() || (material->asset.IsValid() && material->assetGeneration != 0 && material->slot.IsValid());
        }

        /** @brief Rejects malformed copied point geometry and nonphysical impulse estimates. */
        [[nodiscard]] bool ValidPoint(const PhysicsContactPoint &point, const bool sensor) noexcept {
            if (!Math::IsFinite(point.positionOnFirst) || !Math::IsFinite(point.positionOnSecond) || !Math::IsFinite(point.normal) ||
                !std::isfinite(point.penetrationDepthMeters))
                return false;
            if (const float normalLengthSquared =
                    point.normal.x * point.normal.x + point.normal.y * point.normal.y + point.normal.z * point.normal.z;
                std::abs(normalLengthSquared - 1.0F) > 1.0e-3F)
                return false;
            if (sensor && point.normalImpulseEstimateNewtonSeconds.has_value())
                return false;
            return !point.normalImpulseEstimateNewtonSeconds.has_value() ||
                   (std::isfinite(*point.normalImpulseEstimateNewtonSeconds) && *point.normalImpulseEstimateNewtonSeconds >= 0.0F);
        }

        /** @brief Orders physical point identity without treating two impulse estimates as two positions. */
        [[nodiscard]] bool PointGeometryLess(const PhysicsContactPoint &left, const PhysicsContactPoint &right) noexcept {
            return std::tie(left.positionOnFirst, left.positionOnSecond, left.normal, left.penetrationDepthMeters) <
                   std::tie(right.positionOnFirst, right.positionOnSecond, right.normal, right.penetrationDepthMeters);
        }

        /** @brief Retains the smallest unique point evidence and accounts for bounded omissions. */
        void MergePoint(PhysicsContactSummary &summary, const PhysicsContactPoint &point) noexcept {
            auto end = summary.points.begin() + summary.pointCount;
            auto position = std::lower_bound(summary.points.begin(), end, point, PointGeometryLess);
            if (position != end && !PointGeometryLess(point, *position)) {
                position->normalImpulseEstimateNewtonSeconds =
                    std::max(position->normalImpulseEstimateNewtonSeconds, point.normalImpulseEstimateNewtonSeconds);
                return;
            }
            if (summary.pointCount == MaximumPhysicsContactPoints) {
                summary.omittedPointCount = SaturatingAdd(summary.omittedPointCount, 1);
                if (position == end)
                    return;
                --end;
            } else {
                ++summary.pointCount;
            }
            std::move_backward(position, end, end + 1);
            *position = point;
        }

    }  // namespace

    /** @copydoc PhysicsEventProjection::PhysicsEventProjection */
    PhysicsEventProjection::PhysicsEventProjection(const std::uint32_t maximumEvents, const std::uint32_t maximumInFlightPairs,
                                                   const PhysicsEventOverflowPolicy overflowPolicy)
        : maximumEvents_(maximumEvents), maximumInFlightPairs_(maximumInFlightPairs), overflowPolicy_(overflowPolicy),
          observations_(maximumInFlightPairs) {
        previousPairs_.reserve(maximumInFlightPairs);
        currentPairs_.reserve(maximumInFlightPairs);
        for (auto &buffer : eventBuffers_)
            buffer.reserve(maximumEvents);
    }

    /** @copydoc PhysicsEventProjection::BeginTick */
    void PhysicsEventProjection::BeginTick(const std::uint64_t simulationTick) noexcept {
        using enum std::memory_order;
        activeTick_ = simulationTick;
        callbackWrite_.store(0, seq_cst);
        callbackDropped_.store(0, seq_cst);
        callbackOverflowed_.store(false, seq_cst);
        currentPairs_.clear();
        StagingEvents().clear();
        droppedDuringTick_ = 0;
        overflowedDuringTick_ = false;
    }

    /** @copydoc PhysicsEventProjection::TryCapture */
    bool PhysicsEventProjection::TryCapture(const PhysicsContactObservation &observation) noexcept {
        using enum std::memory_order;
        if (observation.simulationTick != activeTick_ || !ValidObservation(observation)) {
            callbackDropped_.fetch_add(1, seq_cst);
            return false;
        }

        std::uint32_t index = callbackWrite_.load(seq_cst);
        for (;;) {
            if (index >= maximumInFlightPairs_) {
                callbackDropped_.fetch_add(1, seq_cst);
                callbackOverflowed_.store(true, seq_cst);
                return false;
            }
            if (callbackWrite_.compare_exchange_weak(index, index + 1, seq_cst, seq_cst))
                break;
        }

        PhysicsContactObservation copied = observation;
        Canonicalize(copied);
        observations_[index] = copied;
        return true;
    }

    /** @copydoc PhysicsEventProjection::BuildCurrentPairs */
    void PhysicsEventProjection::BuildCurrentPairs(const std::uint32_t retained) {
        currentPairs_.clear();
        for (std::uint32_t index = 0; index < retained; ++index) {
            const PhysicsContactObservation &observation = observations_[index];
            const PhysicsEventPairKey pair{.first = observation.first, .second = observation.second};
            if (!currentPairs_.empty() && currentPairs_.back().pair == pair) {
                auto &summary = currentPairs_.back().contact;
                summary.omittedPointCount = SaturatingAdd(summary.omittedPointCount, observation.contact.omittedPointCount);
                for (std::uint32_t point = 0; point < observation.contact.pointCount; ++point)
                    MergePoint(summary, observation.contact.points[point]);
                continue;
            }
            currentPairs_.push_back({.pair = pair,
                                     .firstMaterial = observation.firstMaterial,
                                     .secondMaterial = observation.secondMaterial,
                                     .contact = observation.contact,
                                     .sensor = observation.sensor});
        }
    }

    /** @copydoc PhysicsEventProjection::AppendEnter */
    void PhysicsEventProjection::AppendEnter(const PairState &pair) noexcept {
        using enum PhysicsEventKind;
        Append(pair.sensor ? TriggerEnter : ContactBegin, pair);
    }

    /** @copydoc PhysicsEventProjection::AppendExit */
    void PhysicsEventProjection::AppendExit(const PairState &pair) noexcept {
        using enum PhysicsEventKind;
        Append(pair.sensor ? TriggerExit : ContactEnd, pair);
    }

    /** @copydoc PhysicsEventProjection::ReconcileLifecycle */
    void PhysicsEventProjection::ReconcileLifecycle() noexcept {
        using enum PhysicsEventKind;
        std::size_t previousIndex = 0;
        std::size_t currentIndex = 0;
        while (previousIndex < previousPairs_.size() || currentIndex < currentPairs_.size()) {
            if (previousIndex == previousPairs_.size()) {
                const PairState &current = currentPairs_[currentIndex];
                AppendEnter(current);
                ++currentIndex;
            } else if (currentIndex == currentPairs_.size()) {
                const PairState &previous = previousPairs_[previousIndex];
                AppendExit(previous);
                ++previousIndex;
            } else {
                const PairState &previous = previousPairs_[previousIndex];
                const PairState &current = currentPairs_[currentIndex];
                if (previous.pair < current.pair) {
                    AppendExit(previous);
                    ++previousIndex;
                } else if (current.pair < previous.pair) {
                    AppendEnter(current);
                    ++currentIndex;
                } else {
                    AppendMatchedTransition(previous, current);
                    ++previousIndex;
                    ++currentIndex;
                }
            }
        }
    }

    /** @copydoc PhysicsEventProjection::CompleteTick */
    Result<PhysicsEventProjectionResult> PhysicsEventProjection::CompleteTick(const std::uint64_t simulationTick) {
        if (simulationTick == 0 || activeTick_ != simulationTick)
            return Result<PhysicsEventProjectionResult>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics event projection received an unopened or stale tick."));

        auto &staging = StagingEvents();
        staging.clear();
        const std::uint32_t observed = callbackWrite_.load(std::memory_order::seq_cst);
        const std::uint32_t retained = std::min(observed, maximumInFlightPairs_);
        droppedDuringTick_ = callbackDropped_.load(std::memory_order::seq_cst);
        overflowedDuringTick_ = callbackOverflowed_.load(std::memory_order::seq_cst);

        std::ranges::sort(observations_.begin(), observations_.begin() + retained,
                          [](const PhysicsContactObservation &left, const PhysicsContactObservation &right) {
            return left < right;
        });
        BuildCurrentPairs(retained);
        ReconcileLifecycle();

        if (overflowedDuringTick_ && overflowPolicy_ == PhysicsEventOverflowPolicy::FailTick) {
            staging.clear();
            return Result<PhysicsEventProjectionResult>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded,
                          "Physics event projection exceeded its fixed callback or publication capacity."));
        }

        previousPairs_.swap(currentPairs_);
        publishedBuffer_ ^= std::size_t{1};
        publishedTick_ = simulationTick;
        activeTick_ = 0;
        return Result<PhysicsEventProjectionResult>::Success({.publishedRecordCount = static_cast<std::uint32_t>(staging.size()),
                                                              .droppedRecordCount = droppedDuringTick_,
                                                              .overflowed = overflowedDuringTick_});
    }

    /** @copydoc PhysicsEventProjection::AbortTick */
    void PhysicsEventProjection::AbortTick() noexcept {
        using enum std::memory_order;
        callbackWrite_.store(0, seq_cst);
        callbackDropped_.store(0, seq_cst);
        callbackOverflowed_.store(false, seq_cst);
        activeTick_ = 0;
        currentPairs_.clear();
        StagingEvents().clear();
        droppedDuringTick_ = 0;
        overflowedDuringTick_ = false;
    }

    /** @copydoc PhysicsEventProjection::SuppressBody */
    void PhysicsEventProjection::SuppressBody(const BodyHandle body) noexcept {
        const auto touches = [body](const PhysicsEventPairKey &pair) {
            return pair.first.body == body || pair.second.body == body;
        };
        const std::uint32_t retained = std::min(callbackWrite_.load(std::memory_order::seq_cst), maximumInFlightPairs_);
        std::uint32_t kept{};
        for (std::uint32_t index = 0; index < retained; ++index) {
            if (observations_[index].first.body != body && observations_[index].second.body != body)
                observations_[kept++] = observations_[index];
        }
        callbackWrite_.store(kept, std::memory_order::seq_cst);
        std::erase_if(previousPairs_, [touches](const PairState &pair) {
            return touches(pair.pair);
        });
        std::erase_if(currentPairs_, [touches](const PairState &pair) {
            return touches(pair.pair);
        });
        for (auto &buffer : eventBuffers_)
            std::erase_if(buffer, [touches](const PhysicsEventRecord &event) {
                return touches(event.pair);
            });
    }

    /** @copydoc PhysicsEventProjection::Reset */
    void PhysicsEventProjection::Reset() noexcept {
        AbortTick();
        previousPairs_.clear();
        for (auto &buffer : eventBuffers_)
            buffer.clear();
        publishedBuffer_ = 0;
        publishedTick_ = 0;
    }

    /** @copydoc PhysicsEventProjection::PublishedEvents */
    std::span<const PhysicsEventRecord> PhysicsEventProjection::PublishedEvents() const noexcept {
        return eventBuffers_[publishedBuffer_];
    }

    /** @copydoc PhysicsEventProjection::CopyPublishedEvents */
    PhysicsEventReadCompletion PhysicsEventProjection::CopyPublishedEvents(const std::span<PhysicsEventRecord> records,
                                                                           const std::uint32_t maximumRecords) const noexcept {
        const auto source = PublishedEvents();
        const auto count = std::min({source.size(), records.size(), static_cast<std::size_t>(maximumRecords)});
        std::copy_n(source.begin(), count, records.begin());
        return {static_cast<std::uint32_t>(count), count < source.size(), static_cast<std::uint32_t>(source.size() - count),
                droppedDuringTick_};
    }

    /** @copydoc PhysicsEventProjection::PublishedTick */
    std::uint64_t PhysicsEventProjection::PublishedTick() const noexcept {
        return publishedTick_;
    }

    /** @copydoc PhysicsEventProjection::DroppedRecordCount */
    std::uint64_t PhysicsEventProjection::DroppedRecordCount() const noexcept {
        return droppedDuringTick_;
    }

    /** @copydoc PhysicsEventProjection::ValidObservation */
    bool PhysicsEventProjection::ValidObservation(const PhysicsContactObservation &observation) noexcept {
        const std::array valid{observation.simulationTick != 0,
                               ValidEndpoint(observation.first),
                               ValidEndpoint(observation.second),
                               observation.first != observation.second,
                               observation.first.body.world == observation.second.body.world,
                               observation.first.filterSchemaGeneration == observation.second.filterSchemaGeneration,
                               ValidMaterial(observation.firstMaterial),
                               ValidMaterial(observation.secondMaterial),
                               observation.contact.pointCount > 0,
                               observation.contact.pointCount <= MaximumPhysicsContactPoints};
        return std::ranges::all_of(valid, std::identity{}) &&
               std::all_of(observation.contact.points.begin(), observation.contact.points.begin() + observation.contact.pointCount,
                           [&observation](const PhysicsContactPoint &point) {
            return ValidPoint(point, observation.sensor);
        });
    }

    /** @copydoc PhysicsEventProjection::Canonicalize */
    void PhysicsEventProjection::Canonicalize(PhysicsContactObservation &observation) noexcept {
        if (observation.second < observation.first) {
            std::swap(observation.first, observation.second);
            std::swap(observation.firstMaterial, observation.secondMaterial);
            for (std::uint32_t index = 0; index < observation.contact.pointCount; ++index) {
                auto &point = observation.contact.points[index];
                std::swap(point.positionOnFirst, point.positionOnSecond);
                point.normal = -point.normal;
            }
        }
        PhysicsContactSummary normalized;
        normalized.omittedPointCount = observation.contact.omittedPointCount;
        for (std::uint32_t index = 0; index < observation.contact.pointCount; ++index)
            MergePoint(normalized, observation.contact.points[index]);
        observation.contact = normalized;
    }

    /** @copydoc PhysicsEventProjection::Append */
    void PhysicsEventProjection::Append(const PhysicsEventKind kind, const PairState &state) noexcept {
        auto &staging = StagingEvents();
        if (staging.size() >= maximumEvents_) {
            droppedDuringTick_ = SaturatingAdd(droppedDuringTick_, 1);
            overflowedDuringTick_ = true;
            return;
        }
        staging.push_back({.simulationTick = activeTick_,
                           .kind = kind,
                           .pair = state.pair,
                           .firstMaterial = state.firstMaterial,
                           .secondMaterial = state.secondMaterial,
                           .contact = state.contact});
    }

    /** @copydoc PhysicsEventProjection::AppendMatchedTransition */
    void PhysicsEventProjection::AppendMatchedTransition(const PairState &previous, const PairState &current) noexcept {
        using enum PhysicsEventKind;
        if (previous.sensor == current.sensor) {
            if (!current.sensor)
                Append(ContactPersist, current);
            return;
        }
        AppendExit(previous);
        AppendEnter(current);
    }

    /** @copydoc PhysicsEventProjection::StagingEvents */
    std::vector<PhysicsEventRecord> &PhysicsEventProjection::StagingEvents() noexcept {
        return eventBuffers_[publishedBuffer_ ^ std::size_t{1}];
    }
}  // namespace Horo::Physics::Detail
