#pragma once

/** @file CellAttachmentManifest.h
 * @brief Exact feature-owned cooked references and mandatory cell readiness policy.
 */
#include "Horo/WorldStreaming/StreamingCellCandidate.h"

#include <optional>

namespace Horo::WorldStreaming {
    namespace Detail {
        /** @brief Separates immutable attachment revisions from runtime attempts. */
        struct CellAttachmentRevisionTag;
        /** @brief Separates semantic feature addresses from dense array slots. */
        struct CellAttachmentSubresourceTag;
    }  // namespace Detail

    /** @brief Immutable cooked feature publication revision, never a runtime generation. */
    using CellAttachmentRevision =
        Foundation::Detail::NonZeroId64<Detail::CellAttachmentRevisionTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Feature-owned persistent subresource address, never an array offset. */
    using CellAttachmentSubresource =
        Foundation::Detail::NonZeroId64<Detail::CellAttachmentSubresourceTag, WorldStreamingErrors::IdentityInvalid>;

    namespace CellAttachmentErrors {
        extern const ErrorCodeDescriptor Invalid;          /**< Malformed reference, duplicate membership or policy downgrade. */
        extern const ErrorCodeDescriptor Stale;            /**< Exact content, provider revision or attempt was replaced. */
        extern const ErrorCodeDescriptor CapacityExceeded; /**< Explicit attachment or byte ceiling exceeded. */
        extern const ErrorCodeDescriptor Unsupported;      /**< Unknown feature or missing required implementation. */
        extern const ErrorCodeDescriptor NotReady;         /**< A required prepared resource is unavailable. */
        extern const ErrorCodeDescriptor Closed;           /**< Cancellation or shutdown closed publication. */
    }  // namespace CellAttachmentErrors

    /** @brief Reference only; feature subsystems retain payload schemas, decoding and native ownership. */
    struct CellAttachmentReference final {
        StreamingCellProvider provider{};      /**< One of Terrain, Foliage, NavigationMesh, PhysicsMesh or Audio. */
        Assets::AssetId asset;                 /**< Exact cooked artifact, independent of paths. */
        CellAttachmentSubresource subresource; /**< Exact feature-owned semantic address. */
        CellAttachmentRevision revision;       /**< Exact immutable content revision. */
        std::uint32_t version{};               /**< Exact non-zero feature schema version matching the cell TOC. */
        StreamingCellPayloadRequirement requirement{StreamingCellPayloadRequirement::Required};
        Sha256Digest digest;   /**< Digest of complete artifact bytes; zero is a representable digest. */
        std::uint64_t bytes{}; /**< Exact positive artifact size, checked before provider preparation. */
        [[nodiscard]] constexpr auto operator<=>(const CellAttachmentReference &) const noexcept = default;
    };

    /** @brief Mandatory cook/load-time limits; native reservation accounting stays with the feature provider. */
    struct CellAttachmentManifestLimits final {
        std::size_t maximumReferences{};
        std::uint64_t maximumArtifactBytes{};
    };

    /** @brief Owned immutable feature reference membership for one exact cell artifact. */
    class CellAttachmentManifest final {
    public:
        /** @brief Validates complete membership against the cell TOC without provider calls or partial output.
         * @param candidate Exact validated cell artifact and operation fence.
         * @param revision Nonzero immutable manifest publication revision.
         * @param references Complete reference set, order independent; every supported TOC feature has at least one reference.
         * @param limits Positive complete membership and aggregate byte ceilings.
         * @return Owned manifest or typed invalid, stale, unsupported or capacity error.
         * @details Required wire rows cannot become optional; optional rows may be promoted to required.
         * Unknown required TOC providers fail closed. Optional unknown providers remain unavailable.
         * Copies all retained input. Does not introduce a new wire schema, topology or runtime residency authority.
         */
        [[nodiscard]] static Result<CellAttachmentManifest> Create(const StreamingCellCandidate &candidate, CellAttachmentRevision revision,
                                                                   std::span<const CellAttachmentReference> references,
                                                                   CellAttachmentManifestLimits limits);
        /** @brief Returns exact cell attempt captured by preparation. @return Immutable full operation/fence. */
        [[nodiscard]] const StreamingCellOperationHandle &Operation() const noexcept;
        /** @brief Returns exact cell artifact digest. @return Immutable content identity. */
        [[nodiscard]] const Sha256Digest &CellDigest() const noexcept;
        /** @brief Returns publication revision. @return Nonzero immutable revision. */
        [[nodiscard]] CellAttachmentRevision Revision() const noexcept;
        /** @brief Returns canonical provider/asset/subresource membership. @return Borrow valid through this value's lifetime. */
        [[nodiscard]] std::span<const CellAttachmentReference> References() const noexcept;

    private:
        CellAttachmentManifest(const StreamingCellOperationHandle &operation, const Sha256Digest &digest, CellAttachmentRevision revision,
                               std::vector<CellAttachmentReference> references) noexcept;
        StreamingCellOperationHandle operation_;
        Sha256Digest digest_;
        CellAttachmentRevision revision_;
        std::vector<CellAttachmentReference> references_;
    };

    /** @brief Checks the explicitly supported feature vocabulary. @param provider Wire provider identity.
     * @return True for the five feature attachment domains; CoreEcs remains Scene-owned. */
    [[nodiscard]] bool IsCellAttachmentProvider(StreamingCellProvider provider) noexcept;
}  // namespace Horo::WorldStreaming
