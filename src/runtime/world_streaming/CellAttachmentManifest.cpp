#include "Horo/WorldStreaming/CellAttachmentManifest.h"

#include <algorithm>
#include <tuple>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        /** @brief Orders exact semantic addresses without content revisions affecting duplicate detection. */
        auto Address(const CellAttachmentReference &reference) noexcept {
            return std::tie(reference.provider, reference.asset, reference.subresource);
        }

        /** @brief Validates one reference against the owning TOC policy and exact schema. */
        Result<void> ValidateReference(const CellAttachmentReference &reference, const StreamingCellCandidate &candidate) {
            if (!IsCellAttachmentProvider(reference.provider))
                return Result<void>::Failure(MakeError(CellAttachmentErrors::Unsupported));
            if (!reference.asset.IsValid() || !reference.subresource.IsValid() || !reference.revision.IsValid() || reference.version == 0 ||
                reference.bytes == 0 || reference.requirement >= StreamingCellPayloadRequirement::Count)
                return Result<void>::Failure(MakeError(CellAttachmentErrors::Invalid));
            const auto rows = candidate.Payloads();
            const auto row = std::ranges::find(rows, reference.provider, &StreamingCellPayloadHeader::provider);
            if (row == rows.end())
                return Result<void>::Failure(MakeError(CellAttachmentErrors::Invalid));
            if (row->version != reference.version)
                return Result<void>::Failure(MakeError(CellAttachmentErrors::Stale));
            if (row->requirement == StreamingCellPayloadRequirement::Required && reference.requirement != row->requirement)
                return Result<void>::Failure(MakeError(CellAttachmentErrors::Invalid));
            return Result<void>::Success();
        }

        /** @brief Rejects unsupported critical rows and missing complete known feature membership. */
        Result<void> ValidateMembership(const StreamingCellCandidate &candidate, std::span<const CellAttachmentReference> references) {
            for (const auto &row : candidate.Payloads()) {
                if (row.provider == StreamingCellProvider::CoreEcs)
                    continue;
                if (!IsCellAttachmentProvider(row.provider)) {
                    if (row.requirement == StreamingCellPayloadRequirement::Required)
                        return Result<void>::Failure(MakeError(CellAttachmentErrors::Unsupported));
                    continue;
                }
                if (std::ranges::none_of(references, [&](const auto &reference) {
                    return reference.provider == row.provider;
                }))
                    return Result<void>::Failure(MakeError(CellAttachmentErrors::Invalid));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc IsCellAttachmentProvider */
    bool IsCellAttachmentProvider(const StreamingCellProvider provider) noexcept {
        switch (provider) {
            case StreamingCellProvider::Terrain:
            case StreamingCellProvider::Foliage:
            case StreamingCellProvider::NavigationMesh:
            case StreamingCellProvider::PhysicsMesh:
            case StreamingCellProvider::Audio:
                return true;
            default:
                return false;
        }
    }

    /** @copydoc CellAttachmentManifest::Create */
    Result<CellAttachmentManifest> CellAttachmentManifest::Create(const StreamingCellCandidate &candidate,
                                                                  const CellAttachmentRevision revision,
                                                                  const std::span<const CellAttachmentReference> references,
                                                                  const CellAttachmentManifestLimits limits) {
        const auto fail = [](const ErrorCodeDescriptor &code) {
            return Result<CellAttachmentManifest>::Failure(MakeError(code));
        };
        if (!candidate.Operation().IsValid() || !revision.IsValid() || limits.maximumReferences == 0 || limits.maximumArtifactBytes == 0)
            return fail(CellAttachmentErrors::Invalid);
        if (references.size() > limits.maximumReferences)
            return fail(CellAttachmentErrors::CapacityExceeded);
        std::uint64_t total{};
        for (const auto &reference : references) {
            if (const auto valid = ValidateReference(reference, candidate); valid.HasError())
                return Result<CellAttachmentManifest>::Failure(valid.ErrorValue());
            if (reference.bytes > limits.maximumArtifactBytes - total)
                return fail(CellAttachmentErrors::CapacityExceeded);
            total += reference.bytes;
        }
        if (const auto valid = ValidateMembership(candidate, references); valid.HasError())
            return Result<CellAttachmentManifest>::Failure(valid.ErrorValue());
        std::vector<CellAttachmentReference> canonical{references.begin(), references.end()};
        std::ranges::sort(canonical, [](const auto &left, const auto &right) {
            return Address(left) < Address(right);
        });
        if (std::ranges::adjacent_find(canonical, [](const auto &left, const auto &right) {
            return Address(left) == Address(right);
        }) != canonical.end())
            return fail(CellAttachmentErrors::Invalid);
        return Result<CellAttachmentManifest>::Success(
            CellAttachmentManifest{candidate.Operation(), candidate.ManifestEntry().artifactHash, revision, std::move(canonical)});
    }

    /** @copydoc CellAttachmentManifest::CellAttachmentManifest */
    CellAttachmentManifest::CellAttachmentManifest(StreamingCellOperationHandle operation, Sha256Digest digest,
                                                   CellAttachmentRevision revision,
                                                   std::vector<CellAttachmentReference> references) noexcept
        : operation_(operation), digest_(digest), revision_(revision), references_(std::move(references)) {}

    /** @copydoc CellAttachmentManifest::Operation */
    const StreamingCellOperationHandle &CellAttachmentManifest::Operation() const noexcept {
        return operation_;
    }

    /** @copydoc CellAttachmentManifest::CellDigest */
    const Sha256Digest &CellAttachmentManifest::CellDigest() const noexcept {
        return digest_;
    }

    /** @copydoc CellAttachmentManifest::Revision */
    CellAttachmentRevision CellAttachmentManifest::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc CellAttachmentManifest::References */
    std::span<const CellAttachmentReference> CellAttachmentManifest::References() const noexcept {
        return references_;
    }
}  // namespace Horo::WorldStreaming
