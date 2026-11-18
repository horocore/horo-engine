#pragma once

/**
 * @file TemporalHistory.h
 * @brief Backend-neutral temporal resource ownership and history validity contract.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderResource.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace Horo::Render {
    /** @brief Process-local identity of one temporal-history store lifetime. */
    struct TemporalHistoryOwnerId {
        std::uint64_t value{0};

        /** @brief Reports whether this identity was issued by a store. @return True for a non-zero identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const TemporalHistoryOwnerId &) const noexcept = default;
    };

    /** @brief Generation-safe identity of one view-scoped temporal history. */
    struct TemporalHistoryHandle {
        TemporalHistoryOwnerId owner;
        std::uint32_t slot{0};
        std::uint32_t generation{0};

        /** @brief Reports whether every handle component is non-zero. @return True for valid structure. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.value > 0 && slot > 0 && generation > 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const TemporalHistoryHandle &) const noexcept = default;
    };

    /** @brief Stable application-owned identity of one rendered view. */
    struct RenderViewId {
        std::uint64_t value{0};

        /** @brief Reports whether the view identity is usable. @return True for a non-zero identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderViewId &) const noexcept = default;
    };

    /** @brief Stable frontend-selected temporal provider identity. */
    struct TemporalHistoryProviderId {
        std::uint64_t value{0};

        /** @brief Reports whether the provider identity is usable. @return True for a non-zero identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const TemporalHistoryProviderId &) const noexcept = default;
    };

    /** @brief Stable frontend-selected temporal mode identity. */
    struct TemporalHistoryModeId {
        std::uint64_t value{0};

        /** @brief Reports whether the mode identity is usable. @return True for a non-zero identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const TemporalHistoryModeId &) const noexcept = default;
    };

    /** @brief Explicit reason that prior temporal samples cannot be consumed. */
    enum class TemporalHistoryResetCause : std::uint8_t {
        FirstFrame,
        CameraCut,
        Resize,
        ProfileChange,
        ExplicitRequest,
        MissingPredecessor,
        ViewReplacement,
        SurfaceReplacement,
        DeviceReplacement,
        ProviderReplacement,
        ModeReplacement,
        SchemaReplacement,
        ProjectionChange,
        ColorPlanChange,
        SceneDiscontinuity,
        RecipeChange,
        InvalidInput,
        Suspension,
        SkippedFrame,
        ProviderRequested,
    };

    /** @brief Tests whether a reset cause is declared in the portable contract.
     * @param cause Candidate cause. @return True when the cause can be admitted.
     */
    [[nodiscard]] constexpr bool IsKnownTemporalHistoryResetCause(TemporalHistoryResetCause cause) noexcept {
        return static_cast<std::uint8_t>(cause) <= static_cast<std::uint8_t>(TemporalHistoryResetCause::ProviderRequested);
    }

    /** @brief Generation-complete compatibility identity for one temporal history. */
    struct TemporalHistoryCompatibility {
        RenderViewId view;
        TemporalHistoryProviderId provider;
        TemporalHistoryModeId mode;
        FramebufferExtent renderExtent;
        FramebufferExtent targetExtent;
        std::uint64_t providerGeneration{0};
        std::uint64_t modeGeneration{0};
        std::uint64_t surfaceGeneration{0};
        std::uint64_t rasterGeneration{0};
        std::uint64_t colorGeneration{0};
        std::uint64_t exposureGeneration{0};
        std::uint64_t inputSchemaGeneration{0};
        std::uint64_t deviceGeneration{0};
        std::uint64_t projectionGeneration{0};
        std::uint64_t jitterGeneration{0};
        std::uint64_t sceneOriginGeneration{0};
        std::uint64_t motionGeneration{0};
        std::uint64_t recipeGeneration{0};

        /** @brief Validates all compatibility dimensions. @return True when every dimension is usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            if (!view.IsValid() || !provider.IsValid() || !mode.IsValid() || !renderExtent.IsValid() || !targetExtent.IsValid())
                return false;
            const std::array generations{
                providerGeneration,    modeGeneration,        surfaceGeneration, rasterGeneration,     colorGeneration,
                exposureGeneration,    inputSchemaGeneration, deviceGeneration,  projectionGeneration, jitterGeneration,
                sceneOriginGeneration, motionGeneration,      recipeGeneration,
            };
            for (const std::uint64_t generation : generations) {
                if (generation == 0)
                    return false;
            }
            return true;
        }

        [[nodiscard]] constexpr auto operator<=>(const TemporalHistoryCompatibility &) const noexcept = default;
    };

    /** @brief Finite capacities admitted before a temporal-history store allocates storage. */
    struct TemporalHistoryLimits {
        static constexpr std::size_t HardMaxHistories = 4'096;
        static constexpr std::size_t HardMaxResourcesPerHistory = 32;

        std::size_t maxHistories{64};
        std::size_t maxResourcesPerHistory{8};

        /** @brief Validates finite non-zero limits. @return True when both limits are within hard bounds. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maxHistories > 0 && maxHistories <= HardMaxHistories && maxResourcesPerHistory > 0 &&
                   maxResourcesPerHistory <= HardMaxResourcesPerHistory;
        }
    };

    /**
     * @brief Owning resource and validity snapshot for one attempted real frame.
     *
     * Callers may inspect or move this value. Mutating its validation fields makes Publish and Abandon reject it.
     */
    struct TemporalHistoryFrame {
        TemporalHistoryHandle history;
        TemporalHistoryCompatibility compatibility;
        std::uint64_t frameId{0};
        std::uint64_t contentGeneration{0};
        std::uint64_t attempt{0};
        bool canReadPrevious{false};
        std::optional<TemporalHistoryResetCause> resetCause;
        std::vector<RenderTextureHandle> resources;
    };

    /** @brief Exact compatibility and optional invalidation supplied by a real-frame consumer. */
    struct TemporalHistoryFrameRequest final {
        TemporalHistoryHandle history;
        TemporalHistoryCompatibility compatibility;
        RenderResourceOwnerId resourceOwner; /**< Exact resident texture owner expected by the real frame. */
        std::uint64_t frameId{};
        std::uint64_t predecessorFrameId{};
        std::optional<TemporalHistoryResetCause> reset;
    };

    /**
     * @brief Owner-thread store for bounded view-scoped temporal histories.
     *
     * Resident texture handles remain generation checked by the renderer resource owner. This store owns
     * immutable handle sets, compatibility, frame publication, and reset validity; it never exposes native
     * resources or advances history for abandoned work.
     */
    class TemporalHistoryStore final {
    public:
        TemporalHistoryStore(const TemporalHistoryStore &) = delete;
        TemporalHistoryStore &operator=(const TemporalHistoryStore &) = delete;
        TemporalHistoryStore(TemporalHistoryStore &&other) noexcept;
        TemporalHistoryStore &operator=(TemporalHistoryStore &&other) noexcept;
        ~TemporalHistoryStore();

        /**
         * @brief Creates owner-thread history storage with finite admitted limits.
         * @param limits Maximum live histories and texture handles per history.
         * @return A new store, or a typed validation/allocation/identity error.
         */
        [[nodiscard]] static Result<TemporalHistoryStore> Create(const TemporalHistoryLimits &limits);

        /**
         * @brief Creates one initially invalid view history over exact resident texture generations.
         * @param compatibility Generation-complete compatibility identity.
         * @param resources Non-empty set of valid texture handles retained by value.
         * @return A generation-safe handle, or a typed validation/capacity error.
         */
        [[nodiscard]] Result<TemporalHistoryHandle> Add(const TemporalHistoryCompatibility &compatibility,
                                                        std::span<const RenderTextureHandle> resources);

        /**
         * @brief Begins one real-frame attempt without advancing published history.
         * @param history Live history handle.
         * @param frameId Non-zero monotonically increasing real-frame identity.
         * @param predecessorFrameId Exact predecessor expected by this frame, or zero when none is available.
         * @return Owning frame input with exact validation identity and reset provenance, or a typed failure.
         */
        [[nodiscard]] Result<TemporalHistoryFrame> BeginFrame(TemporalHistoryHandle history, std::uint64_t frameId,
                                                              std::uint64_t predecessorFrameId);

        /** @brief Begins an attempt only when the retained history matches the consumer's complete compatibility.
         * @param request Exact history and resource owners, frame sequence and optional current-frame invalidation.
         * @return Owning frame attempt or typed reset/sequence/lifecycle failure. Rejection leaves no attempt.
         * @details An explicit reset disables prior reads until successful publication. Abandon does not publish a new generation.
         */
        [[nodiscard]] Result<TemporalHistoryFrame> BeginFrame(const TemporalHistoryFrameRequest &request);

        /** @brief Checks a borrowed pending frame before a consumer records native work.
         * @param frame Exact snapshot returned by BeginFrame.
         * @return Success for the current owner-thread attempt or the typed stale/foreign/lifecycle error.
         */
        [[nodiscard]] Result<void> CheckPending(const TemporalHistoryFrame &frame);

        /**
         * @brief Publishes a successfully completed real frame as the next history generation.
         * @param frame Frame snapshot returned by BeginFrame.
         * @return Success, or a typed stale/foreign/lifecycle error.
         */
        [[nodiscard]] Result<void> Publish(const TemporalHistoryFrame &frame);

        /**
         * @brief Abandons a failed or cancelled frame without advancing history.
         * @param frame Frame snapshot returned by BeginFrame.
         * @return Success, or a typed stale/foreign/lifecycle error.
         */
        [[nodiscard]] Result<void> Abandon(const TemporalHistoryFrame &frame);

        /**
         * @brief Atomically replaces compatibility/resources and invalidates previous samples.
         * @param history Live history handle.
         * @param compatibility Complete replacement compatibility identity.
         * @param resources Non-empty replacement set of valid exact texture generations.
         * @param cause Explicit invalidation cause, including camera cut, resize, or profile change.
         * @return Success, or a typed validation/lifecycle error. A pending frame blocks reset.
         */
        [[nodiscard]] Result<void> Reset(TemporalHistoryHandle history, const TemporalHistoryCompatibility &compatibility,
                                         std::span<const RenderTextureHandle> resources, TemporalHistoryResetCause cause);

        /**
         * @brief Retires one history handle and releases its bounded CPU ownership records.
         * @param history Live history handle.
         * @return Success, or a typed stale/foreign/lifecycle error. A pending frame blocks retirement.
         */
        [[nodiscard]] Result<void> Retire(TemporalHistoryHandle history);

        /**
         * @brief Stops the store idempotently and invalidates all handles.
         * @return Success, or WrongThread when invoked outside the owner-thread safe point.
         */
        [[nodiscard]] Result<void> Shutdown();

        /**
         * @brief Reports the number of live histories on the owner thread.
         * @return Bounded live record count, or WrongThread outside the owner-thread safe point.
         */
        [[nodiscard]] Result<std::size_t> Size() const;

    private:
        struct Record;
        enum class PendingCompletion : std::uint8_t {
            Publish,
            Abandon,
        };

        TemporalHistoryStore(TemporalHistoryOwnerId owner, TemporalHistoryLimits limits, std::vector<Record> records) noexcept;
        [[nodiscard]] Result<Record *> Resolve(TemporalHistoryHandle history);
        [[nodiscard]] Result<Record *> ResolveIdle(TemporalHistoryHandle history);
        [[nodiscard]] Result<Record *> ResolvePending(const TemporalHistoryFrame &frame);
        /** @brief Opens one owner-thread attempt with optional exact consumer evidence. */
        [[nodiscard]] Result<TemporalHistoryFrame> Begin(TemporalHistoryHandle history, std::uint64_t frameId,
                                                         std::uint64_t predecessorFrameId, const TemporalHistoryFrameRequest *expected);
        [[nodiscard]] Result<void> CompletePending(const TemporalHistoryFrame &frame, PendingCompletion completion);
        [[nodiscard]] Result<void> ValidateFrameRequest(const Record &record, std::uint64_t frameId) const;
        [[nodiscard]] Result<void> ValidatePendingFrame(const TemporalHistoryFrame &frame, const Record &record) const;
        [[nodiscard]] Result<void> ValidateThreadAndState() const;
        void ReleaseAll() noexcept;

        TemporalHistoryOwnerId m_owner;
        TemporalHistoryLimits m_limits;
        std::thread::id m_ownerThread;
        std::vector<Record> m_records;
        bool m_stopped{false};
        std::size_t m_size{0};
    };
}  // namespace Horo::Render
