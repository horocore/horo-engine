#pragma once

/** @file AgentSceneSelectionSnapshot.h @brief Bounded scene selection evidence for explicit agent consent. */

#include "Horo/Foundation/CancellationToken.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Horo::Editor {
    inline constexpr char LocalAgentWorkspacePanelId[] = "horo.local_agent";

    /** @brief Identifies which local workflow is borrowing the current selection. */
    enum class AgentSceneSelectionPurpose : std::uint8_t {
        SelectedObjects,
        ImmersivePlay
    };

    /** @brief Explicit component data scope for a viewport review. */
    enum class AgentViewportComponents : std::uint8_t {
        None,
        LocalTransforms
    };

    /** @brief Authored position, quaternion (x, y, z, w), and scale of one selected object. */
    struct AgentViewportTransform final {
        std::uint64_t object{};
        std::array<float, 3> translation{};
        std::array<float, 4> rotation{};
        std::array<float, 3> scale{};
    };
    /** @brief Editor tool mode at the time of explicit viewport capture. */
    enum class AgentViewportTool : std::uint8_t {
        Select,
        Move,
        Rotate,
        Scale
    };

    /** @brief Numeric editor camera and interaction state with no renderer or runtime handles. */
    struct AgentViewportOverview final {
        std::uint64_t revision{};
        std::array<float, 3> position{};
        std::array<float, 3> target{};
        std::array<float, 3> up{};
        float verticalExtent{}; /**< Radians for perspective, world units for orthographic. */
        float nearPlane{};
        float farPlane{};
        bool orthographic{};
        AgentViewportTool tool{};
    };

    /** @brief Owner-issued selection with camera and authored component values available by explicit scope. */
    struct AgentSceneSelectionSnapshot final {
        static constexpr std::size_t MaximumObjects = 16;
        std::uint64_t documentEpoch{};
        std::uint64_t documentRevision{};
        std::uint64_t selectionRevision{};
        std::array<std::uint64_t, MaximumObjects> objects{};
        std::size_t objectCount{};
        bool truncated{};
        CancellationToken lifetime;                    /**< Revoked by the owner when the source scene or selection changes. */
        std::optional<AgentViewportOverview> viewport; /**< Present only for a separately requested viewport review. */
        std::array<AgentViewportTransform, MaximumObjects> transforms{};
        std::size_t transformCount{};
    };

    /** @brief Eligibility of an explicitly selected authored object in a captured Play scene. */
    enum class AgentSceneReferenceState : std::uint8_t {
        Available,
        Hidden,
        Locked,
        Deleted,
        OutOfScope
    };

    /** @brief Read-only request to review the captured selection against its exact authoring and Play scope. */
    struct AgentSceneReferenceRequest final {
        std::uint64_t documentEpoch{};
        std::uint64_t documentRevision{};
        std::uint64_t worldGeneration{};
        std::span<const std::uint64_t> objects;
    };

    /** @brief Bounded reference review. Multiple requests stay ambiguous regardless of their eligibility. */
    struct AgentSceneReferenceReview final {
        struct Candidate final {
            std::uint64_t object{};
            AgentSceneReferenceState state{AgentSceneReferenceState::OutOfScope};
        };

        std::uint64_t documentRevision{};
        std::array<Candidate, AgentSceneSelectionSnapshot::MaximumObjects> candidates{};
        std::size_t count{};

        /** @brief Indicates that selection alone cannot establish the intended referent. */
        [[nodiscard]] bool RequiresClarification() const noexcept {
            return count > 1;
        }

        [[nodiscard]] std::span<const Candidate> Candidates() const & noexcept {
            return std::span{candidates}.first(count);
        }

        [[nodiscard]] std::span<const Candidate> Candidates() const && = delete;
    };
}  // namespace Horo::Editor
