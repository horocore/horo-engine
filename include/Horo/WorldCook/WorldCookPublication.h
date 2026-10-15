#pragma once

/** @file WorldCookPublication.h
 * @brief Headless composition of Scene cooking, canonical world artifacts, package admission and Assets publication.
 */
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Runtime/Scene/IncrementalSceneCellCook.h"
#include "Horo/Runtime/Scene/SceneCellPayloadCodec.h"
#include "Horo/Runtime/Scene/WorldBuildValidation.h"
#include "Horo/WorldStreaming/StreamingCellEncoding.h"
#include "Horo/WorldStreaming/StreamingCellInspection.h"
#include "Horo/WorldStreaming/WorldDependencyArtifact.h"
#include "Horo/WorldStreaming/WorldPackageChunkAssignment.h"

namespace Horo::WorldCook {
    /** @brief Inert aggregate-owner failures; original Scene, format, package and durable I/O errors are preserved. */
    namespace Errors {
        extern const ErrorCodeDescriptor Invalid;
        extern const ErrorCodeDescriptor CapacityExceeded;
        extern const ErrorCodeDescriptor Cancelled;
        extern const ErrorCodeDescriptor Stale;
        /** @brief Returns inert declarations for explicit host registration. @return Stable descriptor storage. */
        [[nodiscard]] std::span<const ErrorCodeDescriptor> Descriptors() noexcept;
    }  // namespace Errors

    /** @brief Exact authored object projected to one actual Scene entity in one placed cell. */
    struct SpatialObjectBinding final {
        WorldStreaming::WorldDependencyEndpoint source;
        WorldStreaming::StreamingCellId cell;
        Runtime::SceneObjectId entity;
    };

    /** @brief Exact nonspatial object whose provider-owned content must remain present in the base release. */
    struct BaseObjectBinding final {
        WorldStreaming::WorldDependencyEndpoint source;
        Assets::AssetId artifact;
        Sha256Digest artifactHash; /**< Host-captured digest of the complete verified provider envelope, not a path or ID hash. */
    };

    /** @brief Explicit spatial versus base-content ownership; an absent placement cannot silently drop an object. */
    using ObjectBinding = std::variant<SpatialObjectBinding, BaseObjectBinding>;

    /**
     * @brief One host-captured immutable source publication, borrowed for the complete synchronous operation.
     * @details The caller confines source access to the owning thread and prevents mutation until return. Scene owns
     * component semantics; existing WorldStreaming plans own assignment and dependency semantics. Bindings prove that
     * every planned placement has actual Scene content, and every nonspatial object has verified base content.
     */
    struct SourceCapture final {
        const WorldStreaming::WorldPartitionDescriptor &partition;
        const WorldStreaming::WorldSpatialAssignment &assignments;
        const WorldStreaming::WorldSpanningObjectPlan &placements;
        const WorldStreaming::WorldDependencyPlan &dependencies;
        std::span<const Runtime::SceneCellCookInput> cells;
        std::span<const Runtime::SceneCellCookDependency> dependencyArtifacts;
        std::span<const ObjectBinding> objects;
        std::span<const Runtime::WorldBuildReference> requiredReferences;
        Sha256Digest settings;
        std::uint64_t sourceRevision{}; /**< Exact positive host publication checked again by beforeCommit. */
        std::uint64_t expectedCacheRevision{};
        std::string_view worldName;
        WorldStreaming::WorldIndexCoordinateSystem coordinates{};
        std::span<const WorldStreaming::WorldIndexVolume> volumes;
    };

    /** @brief Explicit registered Assets types for each generated provider-owned artifact role. */
    struct ArtifactTypes final {
        Assets::AssetTypeId index;
        Assets::AssetTypeId dependencies;
        Assets::AssetTypeId cell;
    };

    /**
     * @brief Existing Assets release intent and exact durable publication destination.
     * @details Base envelopes are already cooked by their providers. This aggregate revalidates their complete bytes,
     * target and package membership; it never reserializes native provider objects or selects concrete backends.
     */
    struct ReleaseCapture final {
        const Assets::AssetChunkPlan &packages;
        WorldStreaming::WorldPackageAssignmentBinding binding;
        Sha256Digest baseManifest;
        std::span<const std::vector<std::uint8_t>> baseArtifacts;
        Assets::AssetId indexArtifact;
        Assets::AssetId dependencyArtifact;
        ArtifactTypes types;
        std::filesystem::path targetRoot;
        Assets::AssetCookTargetId target;
    };

    /** @brief Finite bounds owned by the aggregate's explicit application composition. */
    struct PublicationLimits final {
        Runtime::SceneCellPayloadCodecLimits scene;
        WorldStreaming::StreamingCellEncodingLimits cell;
        WorldStreaming::WorldIndexArtifactLimits index;
        WorldStreaming::WorldDependencyArtifactLimits dependencies;
        WorldStreaming::WorldPackageChunkLimits packages;
        Runtime::WorldBuildValidationLimits semantic;
        Assets::AssetCookLimits assets;
        std::size_t maximumTotalArtifactBytes{};
        std::size_t maximumObjectBindings{};
    };

    /** @brief One independently expected cell artifact and complete borrowed bytes. */
    struct CellArtifactView final {
        Assets::AssetId id;
        std::span<const std::byte> bytes;
    };

    /** @brief Detached encoded world outputs; views never escape the synchronous inspection call. */
    struct ArtifactViews final {
        std::span<const std::byte> index;
        std::span<const std::byte> dependencies;
        std::span<const CellArtifactView> cells;
    };

    /** @brief Stable BuildOutput projection paired with the full original typed cause. */
    struct Finding final {
        BuildOutputRecord diagnostic;
        Error cause;
    };

    /** @brief Complete bounded artifact/package qualification, without I/O, store mutation or runtime activation. */
    struct ValidationReport final {
        std::uint64_t sourceRevision{};
        std::vector<Finding> findings;

        /** @brief Reports whether every captured artifact and hard package requirement was admitted. @return True without findings. */
        [[nodiscard]] bool CanPublish() const noexcept {
            return findings.empty();
        }
    };

    /** @brief Immutable retained operation reports; a committed generation may carry an explicit durability error. */
    struct PublicationReport final {
        std::shared_ptr<const ValidationReport> validation;             /**< Prepared before publication; always present on success. */
        std::shared_ptr<const Runtime::SceneCellCookReport> scene;      /**< Detached report retained without moving its storage. */
        std::shared_ptr<const Assets::AssetCookGeneration> publication; /**< Null when content findings prevent publication. */
    };

    /**
     * @brief Verifies actual sibling/cell/provider bytes and complete release membership before publication.
     * @param source Exact expected source identities, schemas, object placement and hard-reference capture.
     * @param release Complete package intent and verified base-envelope inputs.
     * @param artifacts Complete detached world bytes to inspect.
     * @param limits Mandatory aggregate and owning subsystem ceilings.
     * @param cancellation Cooperative observer throughout parsing, semantic decoding and package validation.
     * @return Owned BuildOutput findings for malformed, unsupported, missing or stale content; typed failure for invalid
     * request, cancellation or exhausted report/storage ceilings. Every finding retains its original typed cause.
     * @throws std::bad_alloc Without modifying source, Scene cache, Assets publication or diagnostic stores.
     */
    [[nodiscard]] Result<ValidationReport> ValidateWorldCookArtifacts(const SourceCapture &source, const ReleaseCapture &release,
                                                                      const ArtifactViews &artifacts, const PublicationLimits &limits,
                                                                      const CancellationToken &cancellation = {});

    /**
     * @brief Cooks, encodes, independently validates and atomically publishes one complete world generation.
     * @param cache Scene-owned owner-thread cache; no replacement occurs before durable pointer commit.
     * @param source Exact immutable host capture and both source/cache revision fences.
     * @param release Explicit complete release intent, target and destination.
     * @param limits Mandatory bounds for every owning stage.
     * @param policy Existing Assets durable filesystem/lease policy. beforeCommit is mandatory and must validate and
     * reserve sourceRevision through return; it runs before the Scene cache reservation. Host callbacks remain synchronous.
     * @param cancellation Cooperative observer; cancellation after the commit point cannot undo published output.
     * @return Owned report; content findings publish nothing. A publication value, including durabilityError, is committed
     * and its prepared Scene state has been adopted under the same retained native writer lease.
     * @details Uses only Assets PublishCookGeneration for durable authority. No separate pointer, history or installation
     * protocol is introduced. All fallible receipt preparation precedes commit; final Scene adoption is preallocated and
     * noexcept. Reports retain preallocated immutable owners; no aggregate-owned vector/string move is added after
     * commit. Assets retains its independent implementation/return contract. Host afterCommit callbacks obey the existing nonthrowing
     * Assets contract. Never delete committed output or report rollback after a committed durability failure. Shutdown remains caller
     * cancellation and synchronous return.
     * @throws std::bad_alloc Before commit without replacing prior Scene cache/output, subject to the existing Assets
     * bootstrap recovery contract. Caller afterCommit callbacks must not throw.
     */
    [[nodiscard]] Result<PublicationReport> PublishWorldCook(Runtime::IncrementalSceneCellCook &cache, const SourceCapture &source,
                                                             const ReleaseCapture &release, const PublicationLimits &limits,
                                                             const Assets::AssetCookPublicationPolicy &policy,
                                                             const CancellationToken &cancellation = {});
}  // namespace Horo::WorldCook
