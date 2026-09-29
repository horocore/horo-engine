#pragma once

/**
 * @file PerceptionSpatialBroadphase.h
 * @brief Scene-scoped immutable spatial candidate snapshots for headless perception workers.
 */

#include "Horo/AI/PerceptionDescriptorRegistry.h"
#include "Horo/Math/WorldCoordinate64.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::AI {
    /** @brief Fixed hard ceilings for one scene publication and allocation-free candidate query. */
    struct PerceptionSpatialLimits final {
        static constexpr std::size_t Sources = 8192;
        static constexpr std::size_t Listeners = 2048;
        static constexpr std::size_t SensesPerRecord = 16;
        static constexpr std::size_t CandidatesPerQuery = 128;
        static constexpr std::uint64_t RadiusMillimeters = 1'000'000;
    };

    /** @brief Owner-supplied, declared source facts captured after the scene structural safe point. */
    struct PerceptionSpatialSource final {
        Runtime::EntityRef entity;        /**< Exact active scene and entity generation. */
        Math::WorldCoordinate64 position; /**< Canonical global position, never a rebased float view. */
        std::uint64_t layers{};           /**< Non-zero source layer bitset. */
        std::uint64_t affiliation{};      /**< Host-defined affiliation identity; zero denotes unaligned. */
        std::array<SenseTypeId, PerceptionSpatialLimits::SensesPerRecord> senses{}; /**< Supported typed senses. */
        std::size_t senseCount{};                                                   /**< Active non-empty prefix of senses. */
    };

    /** @brief Owner-supplied listener position and exact generation captured at the same safe point. */
    struct PerceptionSpatialListener final {
        Runtime::EntityRef entity;                                                  /**< Exact active scene and entity generation. */
        Math::WorldCoordinate64 position;                                           /**< Canonical global listener position. */
        std::array<SenseTypeId, PerceptionSpatialLimits::SensesPerRecord> senses{}; /**< Enabled typed senses. */
        std::size_t senseCount{};                                                   /**< Active non-empty prefix of senses. */
    };

    /** @brief Affiliation test applied to a source's host-defined stable affiliation identity. */
    enum class PerceptionAffiliationFilter : std::uint8_t {
        Any,
        Same,
        Different,
        Count,
    };

    /** @brief Bounded worker query against one fixed publication. */
    struct PerceptionSpatialQuery final {
        Runtime::EntityRef listener;       /**< Listener in the same immutable publication. */
        SenseTypeId sense;                 /**< Exact sense supported by both listener and source. */
        std::uint64_t radiusMillimeters{}; /**< Inclusive exact range, at most RadiusMillimeters. */
        std::uint64_t visibleLayers{};     /**< Non-zero bitset intersected with source layers. */
        PerceptionAffiliationFilter affiliationFilter{PerceptionAffiliationFilter::Any};
        std::uint64_t affiliation{};                                                /**< Comparison identity for Same or Different. */
        std::size_t maximumCandidates{PerceptionSpatialLimits::CandidatesPerQuery}; /**< In [1, CandidatesPerQuery]. */
        bool includeSelf{}; /**< Normally a listener does not sense its own source record. */
    };

    /** @brief One immutable candidate value; consumers must revalidate liveness before acting in a later scene revision. */
    struct PerceptionSpatialCandidate final {
        Runtime::EntityRef entity;
        Math::WorldCoordinate64 position;
        std::uint64_t layers{};
        std::uint64_t affiliation{};
    };

    /** @brief Fixed-storage, stable-identity-ordered result and explicit capacity truncation. */
    struct PerceptionSpatialResult final {
        std::array<PerceptionSpatialCandidate, PerceptionSpatialLimits::CandidatesPerQuery> candidates{};
        std::size_t count{};
        bool truncated{};
        std::size_t examinedSources{}; /**< Number of leaf records inspected after spatial pruning. */
    };

    /** @brief Immutable scene-generation snapshot borrowed by workers without any Scene pointer or callback. */
    class PerceptionSpatialSnapshot final {
    public:
        /** @brief Returns the exact source scene incarnation. @return Non-zero runtime scene ID. */
        [[nodiscard]] Runtime::SceneRuntimeId Scene() const noexcept;
        /** @brief Returns the owner's non-zero safe-point publication revision. @return Revision value. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;
        /** @brief Returns the admitted source count. @return At most Sources. */
        [[nodiscard]] std::size_t SourceCount() const noexcept;
        /** @brief Returns the admitted listener count. @return At most Listeners. */
        [[nodiscard]] std::size_t ListenerCount() const noexcept;
        /**
         * @brief Queries the frozen spatial index with no allocation or mutable scene access.
         * @param query Valid exact listener, sense, range and filters.
         * @return Identity-ordered bounded candidates, or typed invalid/missing-listener failure.
         */
        [[nodiscard]] Result<PerceptionSpatialResult> Query(const PerceptionSpatialQuery &query) const;

    private:
        friend class PerceptionSpatialBroadphase;

        struct Node final {
            std::array<std::int64_t, 3> minimum{};
            std::array<std::int64_t, 3> maximum{};
            std::size_t begin{};
            std::size_t end{};
            std::size_t left{};
            std::size_t right{};
        };

        [[nodiscard]] std::size_t BuildNode(std::size_t begin, std::size_t end);
        void Visit(std::size_t node, const PerceptionSpatialListener &listener, const PerceptionSpatialQuery &query,
                   PerceptionSpatialResult &result) const;

        Runtime::SceneRuntimeId scene_;
        std::uint64_t revision_{};
        std::vector<PerceptionSpatialSource> sources_;
        std::vector<PerceptionSpatialListener> listeners_;
        std::vector<Node> nodes_;
    };

    /** @brief Owner-thread transactional publication of declared scene facts at a structural safe point. */
    class PerceptionSpatialBroadphase final {
    public:
        /**
         * @brief Validates and publishes one complete listener/source set after a Scene structural commit.
         * @details The host projects only declared perception participants. A failed publication leaves Current unchanged;
         * retained old snapshots remain historical values, and new worker dispatch must use the newly published snapshot.
         * @param scene Fresh owner-thread scene view acquired after the safe point.
         * @param revision Non-zero, increasing per scene; a new scene incarnation starts a new sequence.
         * @param listeners Borrowed declared listeners and their captured global positions.
         * @param sources Borrowed declared sources and their captured global positions.
         * @return Complete immutable snapshot or a typed invalid, stale, conflict or limit failure.
         * @throws std::bad_alloc When bounded private snapshot storage cannot be allocated.
         */
        [[nodiscard]] Result<std::shared_ptr<const PerceptionSpatialSnapshot>> Publish(const Runtime::RuntimeSceneView &scene,
                                                                                       std::uint64_t revision,
                                                                                       std::span<const PerceptionSpatialListener> listeners,
                                                                                       std::span<const PerceptionSpatialSource> sources);

        /** @brief Returns the latest successfully published snapshot. @return Shared immutable snapshot or null before publication. */
        [[nodiscard]] std::shared_ptr<const PerceptionSpatialSnapshot> Current() const noexcept;

    private:
        std::shared_ptr<const PerceptionSpatialSnapshot> current_;
    };
}  // namespace Horo::AI
