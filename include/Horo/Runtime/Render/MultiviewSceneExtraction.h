#pragma once

/** @file MultiviewSceneExtraction.h
 * @brief Bounded immutable scene extraction and view-index mapping shared by XR rendering paths.
 */
#include "Horo/Math/SceneMath.h"
#include "Horo/Runtime/Render/MaterialBinding.h"

#include <array>
#include <span>

namespace Horo::Render {
    /** @brief Explicit rendering policy; fallback never changes the selected backend. */
    enum class MultiviewExecutionPolicy : std::uint8_t {
        Multipass,
        PreferInstanced,
        RequireInstanced
    };
    /** @brief Selected execution mechanism captured before submission. */
    enum class MultiviewExecutionMode : std::uint8_t {
        Multipass,
        Instanced
    };

    /** @brief Exact producer publication and selected-device fences; zero closes admission. */
    struct MultiviewPublication final {
        std::uint64_t sceneIncarnation{};
        std::uint64_t frame{};
        std::uint64_t originRevision{};
        std::uint64_t viewRevision{};
        std::uint64_t deviceIncarnation{};
        std::uint64_t capabilityRevision{};
        /** @brief Checks all lifetime fences. @return True when every fence is non-zero. */
        [[nodiscard]] bool IsValid() const noexcept;
        auto operator<=>(const MultiviewPublication &) const noexcept = default;
    };

    /** @brief Effective selected-backend support; indexed shaders alone do not imply layer routing. */
    struct MultiviewCapabilities final {
        std::uint32_t maximumViews{};
        bool multipass{};
        bool indexedShaders{};
        bool instancedTargetRouting{};
    };

    /** @brief One runtime-ordered view expressed in renderer math, independent of native eye enums. */
    struct MultiviewSceneView final {
        std::uint32_t index{}; /**< Dense shader index; semantic identity remains with the producer. */
        Math::Mat4 worldToClip;
        Math::ClipDepthRange depthRange{Math::ClipDepthRange::ZeroToOne};
    };

    /** @brief One presentation value extracted from the canonical scene, shared by all views.
     * @details These exact resident generation identities own no GPU resources. Submission must resolve and pin them
     * using the existing frontend resource/material lease authority before any native encoding.
     */
    struct MultiviewSceneInstance final {
        RenderMeshHandle mesh;
        MaterialBindingGenerationId material;
        Math::Mat4 localToWorld{Math::Mat4::Identity()};
        Math::Aabb localBounds;
    };

    /** @brief Finite work envelope checked before walking candidate payloads. */
    struct MultiviewExtractionLimits final {
        static constexpr std::uint32_t HardMaximumViews = 32;
        static constexpr std::uint32_t HardMaximumInstances = 128;
        std::uint32_t maximumViews{2};
        std::uint32_t maximumInstances{HardMaximumInstances};
        std::uint32_t maximumCullTests{HardMaximumInstances * 2};
        std::uint32_t maximumExpandedDraws{HardMaximumInstances * 2};
        /** @brief Checks non-zero capacities against fixed ceilings. @return True for bounded limits. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief One draw references the single immutable instance table and its visible view set. */
    struct MultiviewDraw final {
        std::uint32_t instanceIndex{};
        std::uint32_t visibleViews{}; /**< Bit N means dense view index N; never an eye-role bit. */
    };

    /** @brief One expanded shader invocation mapping, shared by native and multipass adapters. */
    struct MultiviewShaderInvocation final {
        std::uint32_t instanceIndex{};
        std::uint32_t viewIndex{};
    };

    /** @brief Fixed-capacity immutable render presentation with no live-world pointers or heap allocation.
     * @details Source storage is borrowed only by Extract. Copies remain readable after scene/renderer shutdown, but
     * resource use must revalidate the exact publication and resident handles before acquiring completion-owned pins.
     * No simulation, world, image, queue, drain, or GPU-completion authority is introduced here.
     */
    class MultiviewSceneSnapshot final {
    public:
        /** @brief Extracts one canonical instance table and bounded per-view visibility atomically.
         * @param publication Candidate scene/frame/view/device publication.
         * @param active Exact current publication supplied by the existing owner.
         * @param views Complete runtime-ordered renderer views, without truncation.
         * @param instances Scene presentation values copied exactly once, independent of view count.
         * @param capabilities Effective selected-backend support for this publication.
         * @param policy Explicit optimization/fallback requirement.
         * @param limits Finite scene, culling, and native expansion envelope.
         * @return Complete immutable snapshot or typed invalid, stale, unsupported, or capacity failure.
         * @post Success allocates no heap storage, changes no owner state, and performs no native work or waits.
         */
        [[nodiscard]] static Result<MultiviewSceneSnapshot> Extract(
            const MultiviewPublication &publication, const MultiviewPublication &active, std::span<const MultiviewSceneView> views,
            std::span<const MultiviewSceneInstance> instances, const MultiviewCapabilities &capabilities, MultiviewExecutionPolicy policy,
            const MultiviewExtractionLimits &limits = {});

        /** @brief Returns captured lifetime fences. @return Exact immutable publication. */
        [[nodiscard]] const MultiviewPublication &Publication() const noexcept;
        /** @brief Returns selected execution policy. @return Admitted optimization or explicit multipass fallback. */
        [[nodiscard]] MultiviewExecutionMode Mode() const noexcept;
        /** @brief Returns complete ordered views. @return Snapshot-owned immutable span. */
        [[nodiscard]] std::span<const MultiviewSceneView> Views() const noexcept;
        /** @brief Returns the single canonical instance table. @return Snapshot-owned immutable span. */
        [[nodiscard]] std::span<const MultiviewSceneInstance> Instances() const noexcept;
        /** @brief Returns visible instance references. @return Snapshot-owned immutable draw table. */
        [[nodiscard]] std::span<const MultiviewDraw> Draws() const noexcept;
        /** @brief Returns required visible view invocations. @return Bounded count, including optimized-path view expansion. */
        [[nodiscard]] std::uint32_t ExpandedDrawCount() const noexcept;
        /** @brief Revalidates replacement, shutdown and frame fences before resource use.
         * @param active Current owner publication, invalid after shutdown. @return Success or typed invalid/stale failure. */
        [[nodiscard]] Result<void> Validate(const MultiviewPublication &active) const;
        /** @brief Maps one visible draw's packed invocation to canonical source and dense view indices.
         * @param drawIndex Index into Draws(). @param invocation Visible-view ordinal, identical in multipass/instanced modes.
         * @param active Current owner fences. @return Exact shader mapping or typed invalid/stale failure.
         * @post Success performs bounded allocation-free work. Never wraps an invalid index into a different view.
         */
        [[nodiscard]] Result<MultiviewShaderInvocation> Invocation(std::uint32_t drawIndex, std::uint32_t invocation,
                                                                   const MultiviewPublication &active) const;

    private:
        MultiviewSceneSnapshot() = default;
        MultiviewPublication publication_;
        MultiviewExecutionMode mode_{MultiviewExecutionMode::Multipass};
        std::array<MultiviewSceneView, MultiviewExtractionLimits::HardMaximumViews> views_{};
        std::array<MultiviewSceneInstance, MultiviewExtractionLimits::HardMaximumInstances> instances_{};
        std::array<MultiviewDraw, MultiviewExtractionLimits::HardMaximumInstances> draws_{};
        std::uint32_t viewCount_{};
        std::uint32_t instanceCount_{};
        std::uint32_t drawCount_{};
        std::uint32_t expandedDrawCount_{};
    };
}  // namespace Horo::Render
