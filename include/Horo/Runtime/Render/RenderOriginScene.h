#pragma once

/** @file RenderOriginScene.h @brief Renderer-owned origin projection and retained camera-relative frame storage. */
#include "Horo/Runtime/Render/RenderScene.h"
#include "Horo/Runtime/Render/TemporalHistory.h"
#include "Horo/WorldStreaming/OriginRebaseTransaction.h"

#include <memory>
#include <span>
#include <utility>

namespace Horo::Render {
    /** @brief Canonical camera endpoints share the same world authority as rendered placements. */
    struct RenderOriginCamera final {
        Math::WorldCoordinate64 position;
        Math::WorldCoordinate64 target;
        Math::Vec3 up{0.0F, 1.0F, 0.0F};
        RenderProjectionDescriptor projection;
    };

    /** @brief Exact global anchor plus an affine rotation/scale resource descriptor with zero translation. */
    struct RenderOriginInstance final {
        Math::WorldCoordinate64 position;
        RenderStaticMeshInstance draw;
    };

    /** @brief Canonical light anchor; the submitted light descriptor must have zero local translation. */
    struct RenderOriginLight final {
        Math::WorldCoordinate64 position;
        RenderLight light;
    };

    /** @brief Synchronously borrowed canonical extraction inputs. Only converted records are retained. */
    struct RenderOriginSceneSource final {
        RenderOriginCamera camera;
        std::span<const RenderOriginInstance> instances;
        std::span<const RenderOriginLight> lights;
        std::uint64_t sceneDomain{}; /**< Stable producer-issued scene identity for renderer caches. */
    };

    /** @brief Explicit cold allocation policy. Retained frame packets are never overwritten. */
    struct RenderOriginSceneLimits final {
        static constexpr std::size_t MaximumFrameSlots = 8;
        static constexpr std::size_t MaximumInstances = 65'536;
        static constexpr std::size_t MaximumMaterialIdBytes = 256;
        std::size_t frameSlots{};
        std::size_t instances{};
        std::size_t lights{};
        std::size_t materialIdBytes{MaximumMaterialIdBytes};
    };

    namespace Detail {
        struct RenderOriginSceneState;
        struct RenderOriginFrameStorage;
    }  // namespace Detail

    /** @brief Immutable owned geometry/camera packet, valid for new submissions only while its exact origin is current. */
    class RenderOriginFrame final {
    public:
        /** @brief Borrows a scene for one synchronous frontend preparation/submission.
         * @param meshes Existing caller-owned resource views, pinned by the ordinary mesh producer through submission.
         * @return Camera-relative scene or typed rejection for a stale/replaced/closed origin.
         * @pre Retain this packet through any work that borrows its scene. No prior raw borrow creates fresh submission authority.
         */
        [[nodiscard]] Result<RenderSceneView> Scene(std::span<const RenderMeshResourceView> meshes) const;

        /** @brief Joins this origin to the temporal history of the same frontend-issued render view.
         * @param history Existing validated history inputs and view; the adapter owns its scene-origin generation.
         * @return Exact history compatibility or stale/foreign/invalid failure.
         */
        [[nodiscard]] Result<TemporalHistoryCompatibility> StampHistory(TemporalHistoryCompatibility history) const;

    private:
        friend class RenderOriginSceneAdapter;

        RenderOriginFrame(std::shared_ptr<const Detail::RenderOriginFrameStorage> storage,
                          std::weak_ptr<const Detail::RenderOriginSceneState> owner) noexcept
            : storage_(std::move(storage)), owner_(std::move(owner)) {}

        /** @brief Identifies the one still-current renderer view for this immutable packet. */
        [[nodiscard]] Result<RenderViewId> Current() const;
        std::shared_ptr<const Detail::RenderOriginFrameStorage> storage_;
        std::weak_ptr<const Detail::RenderOriginSceneState> owner_;
    };

    /** @brief Owner-lane renderer participant for one canonical world origin and one actual frontend view.
     * @details The canonical world owner and view outlive this adapter. A replacement origin owner requires a new frontend view.
     * The adapter projects published data for rendering only; it cannot alter scene authority or retire accepted GPU work.
     */
    class RenderOriginSceneAdapter final : public WorldStreaming::IOriginRebaseParticipant {
    public:
        /** @brief Allocates all camera/culling/GPU packet storage before frame work.
         * @param origin Canonical world origin owner. @param view Frontend-issued exact view.
         * @param requirement Host-selected participant metadata. @param limits Explicit reserved storage.
         * @return Complete adapter or typed invalid/lifecycle/storage failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<RenderOriginSceneAdapter>> Create(
            const WorldStreaming::OriginFrameOwner &origin, RenderViewId view,
            WorldStreaming::OriginRebaseParticipantRequirement requirement, RenderOriginSceneLimits limits);

        /** @brief Revokes new captures and submissions before releasing the adapter's packet ownership. */
        ~RenderOriginSceneAdapter() override;
        /** @brief Returns the exact configured renderer participant contract. */
        [[nodiscard]] WorldStreaming::OriginRebaseParticipantRequirement Requirement() const noexcept override;

        /** @brief Prepares one exact canonical origin replacement. No packet or live state changes on failed preparation. */
        [[nodiscard]] Result<std::unique_ptr<WorldStreaming::IOriginRebasePreparedParticipant>> Prepare(
            const WorldStreaming::OriginRebaseEvent &event) noexcept override;

        /** @brief Checks the mounted World producer before a host submits canonical positions.
         * @param origin Owner that supplied the placements.
         * @return True only for this adapter's current bound OriginFrameOwner.
         */
        [[nodiscard]] bool UsesOrigin(const WorldStreaming::OriginFrameOwner &origin) const noexcept;

        /** @brief Projects one complete camera/instance/light set from the same committed origin.
         * @param source Canonical immutable source and finite resource spans.
         * @return Owned frame or typed capacity, range, stale or malformed input failure.
         * @post Success allocates no storage. Failure leaves every already published frame unchanged.
         */
        [[nodiscard]] Result<RenderOriginFrame> Capture(const RenderOriginSceneSource &source);
        /** @brief Revokes new captures/submissions; existing accepted packet data remains pinned by its frame owners. */
        void Shutdown() noexcept;

    private:
        explicit RenderOriginSceneAdapter(std::shared_ptr<Detail::RenderOriginSceneState> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<Detail::RenderOriginSceneState> state_;
    };
}  // namespace Horo::Render
