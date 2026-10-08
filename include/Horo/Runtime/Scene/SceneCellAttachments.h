#pragma once

/** @file SceneCellAttachments.h
 * @brief Production attachment admission through aggregate Scene publication and retirement.
 */
#include "Horo/Assets/AssetPayloadCache.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "Horo/WorldStreaming/CellAttachmentManifest.h"
#include "Horo/WorldStreaming/WorldStreamingRuntimeComposition.h"

#include <functional>

namespace Horo::Runtime {
    /** @brief Feature-owned decoder/preparer; success means fully staged native resources, never byte readiness alone.
     * @details Receives the exact reference and owned verified bytes. Preserve all requested semantics or return a typed error.
     * No live mutation during preparation. Returned candidate owns native resources and retirement, including cleanup on failure.
     * Publication must be no-fail and shutdown must revoke access without freeing in-flight resources prematurely.
     */
    using SceneCellAttachmentFactory =
        std::function<Result<std::unique_ptr<SceneActivationCandidate>>(const WorldStreaming::CellAttachmentReference &,
                                                                        Assets::AssetPayloadLease, const RuntimeSceneDefinition &,
                                                                        RuntimeSceneView)>;

    /** @brief Exact explicitly composed feature provider and captured immutable schema support. */
    struct SceneCellAttachmentProvider final {
        WorldStreaming::StreamingCellProvider provider{};
        WorldStreaming::StreamingRuntimeServiceId identity;
        WorldStreaming::StreamingRuntimeServiceRevision revision;
        std::uint32_t version{};
        SceneCellAttachmentFactory prepare;
    };

    /** @brief Exact separately loaded feature artifact; verified against manifest before any factory invocation. */
    struct SceneCellAttachmentBytes final {
        Assets::AssetId asset;
        Assets::AssetPayloadLease lease;
    };

    /** @brief Current Scene publication and streaming attempt supplied by the authority, never inferred from the provider. */
    struct SceneCellAttachmentContext final {
        SceneDefinitionId scene;
        SceneDefinitionRevision sceneRevision;
        WorldStreaming::StreamingCellOperationHandle operation;
        WorldStreaming::CellAttachmentRevision manifestRevision;
        std::size_t maximumProviders{};
        std::size_t maximumArtifacts{};
    };

    /** @brief Observable optional capability availability for an exact semantic reference. */
    struct SceneCellAttachmentStatus final {
        WorldStreaming::CellAttachmentReference reference;
        bool available{};
        std::optional<Error> cause; /**< Exact preserved provider/admission error when unavailable. */
    };

    namespace Detail {
        struct SceneCellAttachmentState;
    }

    /** @brief Host-composed production participant joining feature-owned artifacts to the existing atomic Scene transaction.
     * @details Owner-thread calls only. It owns immutable byte leases and factory bindings; each prepared Scene candidate owns
     * native resources and their retirement. Required failure preserves active Scene. Optional absence/failure is explicit and
     * does not publish a capability. Candidate validation rechecks every required native candidate before the common Scene commit.
     * Services captured by factories must outlive all prepared/active candidates; adapters never discover backends or services.
     */
    class SceneCellAttachmentParticipant final : public SceneActivationParticipant {
        /** @brief Factory-only capability preserving complete admission before owner construction. */
        class ConstructionKey final {
            friend class SceneCellAttachmentParticipant;
            ConstructionKey() = default;
        };

    public:
        /** @brief Adopt complete factory-validated state through std::make_unique.
         * @param key Private capability issued only by Create.
         * @param state Complete non-null admission owner.
         * @pre Only Create may issue the capability after validating all required bindings.
         */
        explicit SceneCellAttachmentParticipant(ConstructionKey key, std::shared_ptr<Detail::SceneCellAttachmentState> state) noexcept;
        /** @brief Closes pending publication; prepared candidates retain their resource ownership. */
        ~SceneCellAttachmentParticipant() override;
        SceneCellAttachmentParticipant(const SceneCellAttachmentParticipant &) = delete;
        SceneCellAttachmentParticipant &operator=(const SceneCellAttachmentParticipant &) = delete;
        /** @brief Validates complete reference/binding/byte evidence before constructing an admission owner.
         * @param context Exact Scene, attempt, manifest revision and positive storage ceilings.
         * @param manifest Owned immutable reference membership.
         * @param providers Complete explicitly selected feature implementations; unsupported optional references remain unavailable.
         * @param artifacts Immutable verified Assets leases; required missing/corrupt artifacts reject before any provider work.
         * @return Unique participant or typed invalid, stale, capacity or unsupported failure. */
        [[nodiscard]] static Result<std::unique_ptr<SceneCellAttachmentParticipant>> Create(
            const SceneCellAttachmentContext &context, WorldStreaming::CellAttachmentManifest manifest,
            std::vector<SceneCellAttachmentProvider> providers, std::vector<SceneCellAttachmentBytes> artifacts);
        /** @brief Replaces complete evidence with a strictly greater immutable manifest revision.
         * @param context New exact Scene/attempt evidence; same partition and epoch, strictly newer manifest revision.
         * @param manifest Complete validated replacement membership.
         * @param providers Complete new explicitly composed providers. @param artifacts Complete immutable byte leases.
         * @return Success or typed rejection without changing the old owner. Pending old candidates become stale on success. */
        [[nodiscard]] Result<void> Replace(const SceneCellAttachmentContext &context, WorldStreaming::CellAttachmentManifest manifest,
                                           std::vector<SceneCellAttachmentProvider> providers,
                                           std::vector<SceneCellAttachmentBytes> artifacts);
        /** @copydoc SceneActivationParticipant::Prepare */
        [[nodiscard]] Result<std::unique_ptr<SceneActivationCandidate>> Prepare(const RuntimeSceneDefinition &definition,
                                                                                RuntimeSceneView scene) override;
        /** @brief Closes admission and pending publication for cancellation; prepared/native candidates retain their cleanup ownership. */
        void RequestCancellation() const noexcept;
        /** @brief Permanently closes admission and pending publication; idempotent, with no wait or fabricated retirement. */
        void Shutdown() const noexcept;
        /** @brief Returns optional capability status of the currently published candidate. @return Owner-thread immutable borrow,
         * valid until next publication, replacement or shutdown. */
        [[nodiscard]] std::span<const SceneCellAttachmentStatus> ActiveStatus() const noexcept;

    private:
        std::shared_ptr<Detail::SceneCellAttachmentState> state_;
    };
}  // namespace Horo::Runtime
