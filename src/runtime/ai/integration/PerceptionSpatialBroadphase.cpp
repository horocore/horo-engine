#include "Horo/AI/PerceptionSpatialBroadphase.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <ranges>
#include <utility>

namespace Horo::AI {
    namespace {
        constexpr std::size_t LeafSources = 8;

        /** @brief Computes exact unsigned separation without signed subtraction overflow. */
        [[nodiscard]] std::uint64_t Separation(const std::int64_t first, const std::int64_t second) noexcept {
            return first >= second ? static_cast<std::uint64_t>(first) - static_cast<std::uint64_t>(second)
                                   : static_cast<std::uint64_t>(second) - static_cast<std::uint64_t>(first);
        }

        /** @brief Returns distance to an inclusive axis interval. */
        [[nodiscard]] std::uint64_t AxisGap(const std::int64_t point, const std::int64_t minimum, const std::int64_t maximum) noexcept {
            if (point < minimum)
                return Separation(point, minimum);
            if (point > maximum)
                return Separation(point, maximum);
            return 0;
        }

        /** @brief Tests a global millimeter point against an exact bounded sphere. */
        [[nodiscard]] bool WithinRadius(const std::array<std::int64_t, 3> &point, const std::array<std::int64_t, 3> &center,
                                        const std::uint64_t radius) noexcept {
            std::uint64_t squared{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const auto distance = Separation(point[axis], center[axis]);
                if (distance > radius)
                    return false;
                squared += distance * distance;
            }
            return squared <= radius * radius;
        }

        /** @brief Validates exact scene membership and a non-empty duplicate-free sense set. */
        template <typename Record> [[nodiscard]] Result<void> ValidateRecord(const Runtime::RuntimeSceneView &scene, const Record &record) {
            if (!record.entity.IsValid() || record.senseCount == 0 || record.senseCount > record.senses.size())
                return Result<void>::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
            if (record.entity.runtime != scene.RuntimeId() || scene.Get(record.entity).HasError())
                return Result<void>::Failure(MakeError(AIErrors::PerceptionSpatialStale));
            for (std::size_t index = 0; index < record.senseCount; ++index) {
                if (!record.senses[index].IsValid())
                    return Result<void>::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (record.senses[prior] == record.senses[index])
                        return Result<void>::Failure(MakeError(AIErrors::PerceptionSpatialConflict));
            }
            return Result<void>::Success();
        }

        /** @brief Tests a sorted active sense prefix without retaining a descriptor or callback. */
        template <typename Record> [[nodiscard]] bool Supports(const Record &record, const SenseTypeId sense) noexcept {
            return std::binary_search(record.senses.begin(), record.senses.begin() + record.senseCount, sense);
        }

        /** @brief Filters a source before the exact range check. */
        [[nodiscard]] bool Matches(const PerceptionSpatialSource &source, const PerceptionSpatialListener &listener,
                                   const PerceptionSpatialQuery &query) noexcept {
            if (!query.includeSelf && source.entity == listener.entity)
                return false;
            if ((source.layers & query.visibleLayers) == 0 || !Supports(source, query.sense))
                return false;
            if (query.affiliationFilter == PerceptionAffiliationFilter::Same && source.affiliation != query.affiliation)
                return false;
            if (query.affiliationFilter == PerceptionAffiliationFilter::Different && source.affiliation == query.affiliation)
                return false;
            return WithinRadius(source.position.Millimeters(), listener.position.Millimeters(), query.radiusMillimeters);
        }

        /** @brief Keeps the first bounded identities independent of BVH traversal order. */
        void InsertCandidate(const PerceptionSpatialSource &source, const std::size_t maximum, PerceptionSpatialResult &result) {
            const auto record = PerceptionSpatialCandidate{.entity = source.entity,
                                                           .position = source.position,
                                                           .layers = source.layers,
                                                           .affiliation = source.affiliation};
            const auto first = result.candidates.begin();
            const auto insertion = std::lower_bound(first, first + result.count, source.entity,
                                                    [](const PerceptionSpatialCandidate &entry, const Runtime::EntityRef entity) {
                return entry.entity < entity;
            });
            const auto index = static_cast<std::size_t>(insertion - first);
            if (result.count == maximum) {
                result.truncated = true;
                if (index == maximum)
                    return;
                std::move_backward(first + index, first + maximum - 1, first + maximum);
            } else {
                std::move_backward(first + index, first + result.count, first + result.count + 1);
                ++result.count;
            }
            result.candidates[index] = record;
        }
    }  // namespace

    /** @copydoc PerceptionSpatialSnapshot::Scene */
    Runtime::SceneRuntimeId PerceptionSpatialSnapshot::Scene() const noexcept {
        return scene_;
    }

    /** @copydoc PerceptionSpatialSnapshot::Revision */
    std::uint64_t PerceptionSpatialSnapshot::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc PerceptionSpatialSnapshot::SourceCount */
    std::size_t PerceptionSpatialSnapshot::SourceCount() const noexcept {
        return sources_.size();
    }

    /** @copydoc PerceptionSpatialSnapshot::ListenerCount */
    std::size_t PerceptionSpatialSnapshot::ListenerCount() const noexcept {
        return listeners_.size();
    }

    /** @brief Partitions one source range by its widest exact global axis. */
    std::size_t PerceptionSpatialSnapshot::BuildNode(const std::size_t begin, const std::size_t end) {
        const auto index = nodes_.size();
        nodes_.emplace_back();
        auto minimum = sources_[begin].position.Millimeters();
        auto maximum = minimum;
        for (std::size_t source = begin + 1; source < end; ++source) {
            const auto point = sources_[source].position.Millimeters();
            for (std::size_t axis = 0; axis < 3; ++axis) {
                minimum[axis] = std::min(minimum[axis], point[axis]);
                maximum[axis] = std::max(maximum[axis], point[axis]);
            }
        }
        nodes_[index].minimum = minimum;
        nodes_[index].maximum = maximum;
        nodes_[index].begin = begin;
        nodes_[index].end = end;
        if (end - begin <= LeafSources)
            return index;

        std::size_t axis{};
        for (std::size_t candidate = 1; candidate < 3; ++candidate)
            if (Separation(maximum[candidate], minimum[candidate]) > Separation(maximum[axis], minimum[axis]))
                axis = candidate;
        const auto middle = std::midpoint(begin, end);
        std::nth_element(sources_.begin() + begin, sources_.begin() + middle, sources_.begin() + end,
                         [axis](const PerceptionSpatialSource &first, const PerceptionSpatialSource &second) {
            const auto firstAxis = first.position.Millimeters()[axis];
            const auto secondAxis = second.position.Millimeters()[axis];
            return firstAxis == secondAxis ? first.entity < second.entity : firstAxis < secondAxis;
        });
        const auto left = BuildNode(begin, middle);
        const auto right = BuildNode(middle, end);
        nodes_[index].left = left;
        nodes_[index].right = right;
        return index;
    }

    /** @brief Prunes non-intersecting BVH nodes before testing source records. */
    void PerceptionSpatialSnapshot::Visit(const std::size_t index, const PerceptionSpatialListener &listener,
                                          const PerceptionSpatialQuery &query, PerceptionSpatialResult &result) const {
        const auto &node = nodes_[index];
        const auto center = listener.position.Millimeters();
        std::uint64_t squared{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto gap = AxisGap(center[axis], node.minimum[axis], node.maximum[axis]);
            if (gap > query.radiusMillimeters)
                return;
            squared += gap * gap;
        }
        if (squared > query.radiusMillimeters * query.radiusMillimeters)
            return;
        if (node.end - node.begin > LeafSources) {
            Visit(node.left, listener, query, result);
            Visit(node.right, listener, query, result);
            return;
        }
        for (std::size_t source = node.begin; source < node.end; ++source) {
            ++result.examinedSources;
            if (Matches(sources_[source], listener, query))
                InsertCandidate(sources_[source], query.maximumCandidates, result);
        }
    }

    /** @copydoc PerceptionSpatialSnapshot::ListenerPosition */
    Result<Math::WorldCoordinate64> PerceptionSpatialSnapshot::ListenerPosition(const Runtime::EntityRef listener) const {
        if (!listener.IsValid() || listener.runtime != scene_)
            return Result<Math::WorldCoordinate64>::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
        const auto found = std::ranges::lower_bound(listeners_, listener, {}, &PerceptionSpatialListener::entity);
        if (found == listeners_.end() || found->entity != listener)
            return Result<Math::WorldCoordinate64>::Failure(MakeError(AIErrors::PerceptionSpatialListenerMissing));
        return Result<Math::WorldCoordinate64>::Success(found->position);
    }

    /** @copydoc PerceptionSpatialSnapshot::Query */
    Result<PerceptionSpatialResult> PerceptionSpatialSnapshot::Query(const PerceptionSpatialQuery &query) const {
        if (!query.listener.IsValid() || query.listener.runtime != scene_ || !query.sense.IsValid() || query.visibleLayers == 0 ||
            query.radiusMillimeters > PerceptionSpatialLimits::RadiusMillimeters || query.maximumCandidates == 0 ||
            query.maximumCandidates > PerceptionSpatialLimits::CandidatesPerQuery ||
            query.affiliationFilter >= PerceptionAffiliationFilter::Count)
            return Result<PerceptionSpatialResult>::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
        const auto listener = std::ranges::lower_bound(listeners_, query.listener, {}, &PerceptionSpatialListener::entity);
        if (listener == listeners_.end() || listener->entity != query.listener)
            return Result<PerceptionSpatialResult>::Failure(MakeError(AIErrors::PerceptionSpatialListenerMissing));
        if (!Supports(*listener, query.sense))
            return Result<PerceptionSpatialResult>::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
        PerceptionSpatialResult result;
        if (!nodes_.empty())
            Visit(0, *listener, query, result);
        return Result<PerceptionSpatialResult>::Success(std::move(result));
    }

    /** @copydoc PerceptionSpatialBroadphase::Publish */
    Result<std::shared_ptr<const PerceptionSpatialSnapshot>> PerceptionSpatialBroadphase::Publish(
        const Runtime::RuntimeSceneView &scene, const std::uint64_t revision, const std::span<const PerceptionSpatialListener> listeners,
        const std::span<const PerceptionSpatialSource> sources) {
        using SnapshotResult = Result<std::shared_ptr<const PerceptionSpatialSnapshot>>;
        if (!scene.IsCurrent() || !scene.RuntimeId().IsValid())
            return SnapshotResult::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        if (revision == 0)
            return SnapshotResult::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
        if (current_ && current_->Scene() == scene.RuntimeId() && revision <= current_->Revision())
            return SnapshotResult::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        if (listeners.size() > PerceptionSpatialLimits::Listeners || sources.size() > PerceptionSpatialLimits::Sources)
            return SnapshotResult::Failure(MakeError(AIErrors::PerceptionSpatialLimitExceeded));

        auto next = std::make_shared<PerceptionSpatialSnapshot>();
        next->scene_ = scene.RuntimeId();
        next->revision_ = revision;
        next->listeners_.assign(listeners.begin(), listeners.end());
        next->sources_.assign(sources.begin(), sources.end());
        for (auto &listener : next->listeners_) {
            if (const auto valid = ValidateRecord(scene, listener); valid.HasError())
                return SnapshotResult::Failure(valid.ErrorValue());
            std::sort(listener.senses.begin(), listener.senses.begin() + listener.senseCount);
        }
        for (auto &source : next->sources_) {
            if (const auto valid = ValidateRecord(scene, source); valid.HasError())
                return SnapshotResult::Failure(valid.ErrorValue());
            if (source.layers == 0)
                return SnapshotResult::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
            std::sort(source.senses.begin(), source.senses.begin() + source.senseCount);
        }
        std::ranges::sort(next->listeners_, {}, &PerceptionSpatialListener::entity);
        std::ranges::sort(next->sources_, {}, &PerceptionSpatialSource::entity);
        if (std::ranges::adjacent_find(next->listeners_,
                                       [](const auto &first, const auto &second) {
            return first.entity == second.entity;
        }) != next->listeners_.end() ||
            std::ranges::adjacent_find(next->sources_, [](const auto &first, const auto &second) {
            return first.entity == second.entity;
        }) != next->sources_.end())
            return SnapshotResult::Failure(MakeError(AIErrors::PerceptionSpatialConflict));
        if (!next->sources_.empty()) {
            next->nodes_.reserve(next->sources_.size() * 2);
            (void)next->BuildNode(0, next->sources_.size());
        }
        if (!scene.IsCurrent())
            return SnapshotResult::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        current_ = std::move(next);
        return SnapshotResult::Success(current_);
    }

    /** @copydoc PerceptionSpatialBroadphase::Current */
    std::shared_ptr<const PerceptionSpatialSnapshot> PerceptionSpatialBroadphase::Current() const noexcept {
        return current_;
    }
}  // namespace Horo::AI
