#include "Horo/Terrain/TerrainPayloadManifest.h"
#include "TerrainPayloadManifestInternal.h"
#include "TerrainTileCookCodec.h"

#include <algorithm>
#include <limits>

namespace Horo::Terrain {
    namespace Detail {
        /** @copydoc PayloadManifestSuccessor */
        bool PayloadManifestSuccessor(const TerrainPayloadProvenance &old, const TerrainPayloadProvenance &next) {
            return old.dataset == next.dataset && old.sourceAsset == next.sourceAsset &&
                   (old.source != next.source || old.sourceDigest == next.sourceDigest) &&
                   old.content.Value() != std::numeric_limits<std::uint64_t>::max() && next.content.Value() == old.content.Value() + 1 &&
                   next.source.Value() >= old.source.Value() && next.capability.Value() >= old.capability.Value();
        }
    }  // namespace Detail

    namespace TerrainPayloadManifestErrors {
        namespace {
            const ErrorDomainId Domain{"horo.terrain.manifest"};
        }

        const ErrorCodeDescriptor Invalid{Domain, ErrorCode{"terrain.manifest.invalid"}, ErrorSeverity::Error,
                                          "Terrain payload manifest evidence is invalid.", "Use a complete verified cooked generation."};
        const ErrorCodeDescriptor LimitExceeded{Domain, ErrorCode{"terrain.manifest.limit_exceeded"}, ErrorSeverity::Error,
                                                "Terrain manifest exceeds a finite construction limit.",
                                                "Reduce admitted manifest work or content."};
        const ErrorCodeDescriptor Cancelled{Domain, ErrorCode{"terrain.manifest.cancelled"}, ErrorSeverity::Warning,
                                            "Terrain manifest work was cancelled.", "Retry with current source evidence."};
        const ErrorCodeDescriptor Stale{Domain, ErrorCode{"terrain.manifest.stale"}, ErrorSeverity::Error,
                                        "Terrain manifest replacement is stale.", "Prepare an exact successor of the selected generation."};
        const ErrorCodeDescriptor Closed{Domain, ErrorCode{"terrain.manifest.closed"}, ErrorSeverity::Error,
                                         "Terrain manifest admission is closed.", "Use an active host publication owner."};
    }  // namespace TerrainPayloadManifestErrors

    /** @copydoc VerifyTerrainPayloadManifest */
    Result<void> VerifyTerrainPayloadManifest(const TerrainPayloadManifest &expected, const std::span<const std::uint8_t> bytes,
                                              const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(TerrainPayloadManifestErrors::Cancelled));
        constexpr std::array<std::uint8_t, 10> prefix{4, 0, 'H', 'T', 'P', 'M', 1, 0, 0, 0};
        if (expected.Tiles().empty() || !expected.Provenance().dataset.IsValid() || !expected.Provenance().content.IsValid() ||
            bytes.size() != expected.Bytes().size() || bytes.size() < prefix.size() ||
            !std::ranges::equal(bytes.first(prefix.size()), prefix))
            return Result<void>::Failure(MakeError(TerrainPayloadManifestErrors::Invalid));
        const auto digest = Detail::HashPayload(bytes, cancellation);
        if (digest.HasError())
            return Result<void>::Failure(MakeError(TerrainPayloadManifestErrors::Cancelled));
        if (digest.Value() != expected.Digest())
            return Result<void>::Failure(MakeError(TerrainPayloadManifestErrors::Invalid));
        return Result<void>::Success();
    }

    /** @copydoc ValidateTerrainPayloadManifestPublication */
    Result<void> ValidateTerrainPayloadManifestPublication(const TerrainPayloadManifest &candidate, const TerrainPayloadManifest *current,
                                                           const std::optional<Sha256Digest> expectedCurrent,
                                                           const TerrainRuntimeLifecycle lifecycle, const CancellationToken &cancellation) {
        if (lifecycle != TerrainRuntimeLifecycle::Active)
            return Result<void>::Failure(MakeError(TerrainPayloadManifestErrors::Closed));
        const auto checked = VerifyTerrainPayloadManifest(candidate, candidate.Bytes(), cancellation);
        if (checked.HasError())
            return checked;
        if (!current) {
            if (expectedCurrent)
                return Result<void>::Failure(MakeError(TerrainPayloadManifestErrors::Stale));
            return Result<void>::Success();
        }
        const auto prior = VerifyTerrainPayloadManifest(*current, current->Bytes(), cancellation);
        if (prior.HasError())
            return prior;
        const auto &old = current->Provenance();
        const auto &next = candidate.Provenance();
        if (!expectedCurrent || *expectedCurrent != current->Digest() || !Detail::PayloadManifestSuccessor(old, next))
            return Result<void>::Failure(MakeError(TerrainPayloadManifestErrors::Stale));
        return Result<void>::Success();
    }
}  // namespace Horo::Terrain
