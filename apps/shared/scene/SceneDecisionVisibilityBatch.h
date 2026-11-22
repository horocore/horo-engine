#pragma once

/** @file SceneDecisionVisibilityBatch.h @brief Fixed-tick reuse of exact owner-side Physics visibility evidence. */

#include "Horo/AI/EnvironmentQueryPhysics.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <tuple>

namespace Horo::Application::Internal {
    /** @brief Keys detached visibility bits by the exact published world and effective query filter. */
    struct SceneDecisionVisibilityKey final {
        AI::QueryPhysicsPublication publication;
        Physics::PhysicsQueryFilter filter;

    private:
        static auto PublicationIdentity(const AI::QueryPhysicsPublication &value) noexcept {
            return std::tie(value.identity.world, value.identity.capabilityGeneration, value.sceneGeneration, value.revision,
                            value.completedTick);
        }

        static auto FilterIdentity(const Physics::PhysicsQueryFilter &value) noexcept {
            return std::tie(value.channel, value.triggers, value.requiredLayer, value.requiredProfile, value.excludedBody,
                            value.blockingOnly);
        }

    public:
        /** @brief Tests whether two batches may share provider evidence. */
        [[nodiscard]] bool operator==(const SceneDecisionVisibilityKey &other) const noexcept {
            return PublicationIdentity(publication) == PublicationIdentity(other.publication) &&
                   FilterIdentity(filter) == FilterIdentity(other.filter);
        }
    };

    /** @brief Retains successful same-tick batches without sharing query results or allocating on fixed ticks. */
    class SceneDecisionVisibilityBatch final {
    public:
        /** @brief Returns ordered bits until the next write, or an empty span for a miss. */
        [[nodiscard]] std::span<const bool> Find(const SceneDecisionVisibilityKey &key,
                                                 std::span<const Physics::PhysicsRayQuery> rays) const noexcept {
            for (const auto &entry : std::span{entries_}.first(size_)) {
                if (entry.key == key && std::ranges::equal(std::span{entry.rays}.first(entry.count), rays, SameRay))
                    return std::span{entry.clear}.first(entry.count);
            }
            return {};
        }

        /** @brief Copies a successful nonempty batch and evicts the oldest when storage is full. */
        void Store(const SceneDecisionVisibilityKey &key, std::span<const Physics::PhysicsRayQuery> rays,
                   std::span<const bool> clear) noexcept {
            if (rays.empty() || rays.size() > AI::MaximumQueryPhysicsVisibilityRays || clear.size() != rays.size())
                return;
            auto &entry = entries_[next_];
            entry.key = key;
            entry.count = rays.size();
            std::ranges::copy(rays, entry.rays.begin());
            std::ranges::copy(clear, entry.clear.begin());
            next_ = (next_ + 1) % entries_.size();
            size_ = std::min(size_ + 1, entries_.size());
        }

    private:
        /** @brief Requires exact ordered provider input, without tolerances or result reordering. */
        static bool SameRay(const Physics::PhysicsRayQuery &left, const Physics::PhysicsRayQuery &right) noexcept {
            const auto bits = [](float value) {
                return std::bit_cast<std::uint32_t>(value);
            };
            const auto values = [bits](const Math::Vec3 &vector) {
                return std::tuple{bits(vector.x), bits(vector.y), bits(vector.z)};
            };
            return values(left.origin) == values(right.origin) && values(left.direction) == values(right.direction) &&
                   bits(left.maximumDistanceMeters) == bits(right.maximumDistanceMeters);
        }

        struct Entry final {
            SceneDecisionVisibilityKey key;
            std::array<Physics::PhysicsRayQuery, AI::MaximumQueryPhysicsVisibilityRays> rays;
            std::array<bool, AI::MaximumQueryPhysicsVisibilityRays> clear{};
            std::size_t count{};
        };

        static constexpr std::size_t RetainedBatches = 8;
        std::array<Entry, RetainedBatches> entries_;
        std::size_t next_{};
        std::size_t size_{};
    };
}  // namespace Horo::Application::Internal
