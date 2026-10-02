#include "Horo/Navigation/NavigationLinkValidation.h"

#include <algorithm>
#include <array>
#include <bit>
#include <utility>

namespace Horo::Navigation {
    namespace {
        /** @brief Fixed-width big-endian hashing excludes runtime handles and operation generation from the cook key. */
        void HashU64(Sha256Builder &hash, const std::uint64_t value) noexcept {
            std::array<std::byte, 8> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index)
                bytes[index] = static_cast<std::byte>((value >> ((7 - index) * 8)) & 0xFFU);
            static_cast<void>(hash.Update(bytes));
        }

        /** @brief Canonical float hashing treats signed zero as one semantic value. */
        void HashPoint(Sha256Builder &hash, const Math::Vec3 point) noexcept {
            for (const float value : {point.x, point.y, point.z})
                HashU64(hash, std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value));
        }

        /** @brief Adds portable link semantics and stable source identities to the cook dependency key. */
        void HashLink(Sha256Builder &hash, const NavigationValidatedLink &link) noexcept {
            HashU64(hash, link.authoredLink.Value());
            HashU64(hash, link.startAnchor.Value());
            HashU64(hash, link.endAnchor.Value());
            HashU64(hash, link.start.surface.Value());
            HashU64(hash, link.end.surface.Value());
            HashPoint(hash, link.start.position);
            HashPoint(hash, link.end.position);
            HashU64(hash, link.start.provenance.polygonIndex);
            HashU64(hash, link.end.provenance.polygonIndex);
            HashU64(hash, static_cast<std::uint8_t>(link.kind));
            HashU64(hash, static_cast<std::uint8_t>(link.direction));
            HashU64(hash, link.area.Value());
            HashU64(hash, std::bit_cast<std::uint32_t>(link.radiusMeters));
        }
    }  // namespace

    NavigationLinkValidationSnapshot::NavigationLinkValidationSnapshot(const NavigationBakeInputSnapshot &input,
                                                                       const NavigationLinkProjectionContext &context)
        : revisions_(input.Revisions()), fingerprint_(input.Fingerprint()), context_(context) {}

    /** @copydoc NavigationLinkValidationSnapshot::NavigationLinkValidationSnapshot */
    NavigationLinkValidationSnapshot::NavigationLinkValidationSnapshot(NavigationLinkValidationSnapshot &&other) noexcept
        : valid_(std::exchange(other.valid_, false)), revisions_(other.revisions_), fingerprint_(other.fingerprint_),
          context_(other.context_), authored_(std::move(other.authored_)), suggestions_(std::move(other.suggestions_)),
          diagnostics_(std::move(other.diagnostics_)) {}

    /** @copydoc NavigationLinkValidationSnapshot::Authored */
    std::span<const NavigationValidatedLink> NavigationLinkValidationSnapshot::Authored() const noexcept {
        return authored_;
    }

    /** @copydoc NavigationLinkValidationSnapshot::Suggestions */
    std::span<const NavigationValidatedLink> NavigationLinkValidationSnapshot::Suggestions() const noexcept {
        return suggestions_;
    }

    /** @copydoc NavigationLinkValidationSnapshot::Diagnostics */
    std::span<const NavigationLinkDiagnostic> NavigationLinkValidationSnapshot::Diagnostics() const noexcept {
        return diagnostics_;
    }

    /** @copydoc NavigationLinkValidationSnapshot::PrepareCookedLinks */
    Result<NavigationCookedLinkSet> NavigationLinkValidationSnapshot::PrepareCookedLinks(
        const NavigationGeneratedLinkCookPolicy policy, const NavigationBakeInputSnapshot &input,
        const NavigationBakeInputRevisions &currentRevisions, const std::span<const NavigationSourceObservation> currentSources,
        const NavigationLinkProjectionContext &currentContext, const NavigationBakePublicationState state,
        const CancellationToken &cancellation) const {
        if (cancellation.IsCancellationRequested())
            return Result<NavigationCookedLinkSet>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
        if (!valid_ || policy < NavigationGeneratedLinkCookPolicy::AuthoredOnly || policy >= NavigationGeneratedLinkCookPolicy::Count)
            return Result<NavigationCookedLinkSet>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        if (input.Revisions() != revisions_ || input.Fingerprint() != fingerprint_ || currentContext.profile != context_.profile ||
            currentContext.world != context_.world || currentContext.topology != context_.topology)
            return Result<NavigationCookedLinkSet>::Failure(MakeError(NavigationErrors::BakeInputStale));
        if (const auto fresh = input.ValidatePublication(revisions_.requestGeneration, currentRevisions, currentSources, state);
            fresh.HasError())
            return Result<NavigationCookedLinkSet>::Failure(fresh.ErrorValue());
        if (std::ranges::any_of(diagnostics_, [](const auto &row) {
            return !row.startAnchor.IsValid();
        }))
            return Result<NavigationCookedLinkSet>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        NavigationCookedLinkSet output;
        const bool includeSuggestions = policy == NavigationGeneratedLinkCookPolicy::IncludeValidatedSuggestions;
        output.links.reserve(authored_.size() + (includeSuggestions ? suggestions_.size() : 0));
        Sha256Builder hash;
        HashU64(hash, 0x4e41564c494e4b01ULL);
        static_cast<void>(hash.Update(std::as_bytes(std::span{fingerprint_.bytes})));
        HashU64(hash, context_.profile.Value());
        HashU64(hash, static_cast<std::uint8_t>(policy));
        HashU64(hash, authored_.size() + (includeSuggestions ? suggestions_.size() : 0));
        const auto append = [&](const std::span<const NavigationValidatedLink> rows) {
            for (const auto &row : rows) {
                output.links.push_back({.start = row.start.position,
                                        .end = row.end.position,
                                        .radiusMeters = row.radiusMeters,
                                        .startPolygon = row.start.provenance.polygonIndex,
                                        .endPolygon = row.end.provenance.polygonIndex,
                                        .area = row.area,
                                        .bidirectional = row.direction == NavigationLinkDirection::Bidirectional});
                HashLink(hash, row);
            }
        };
        append(authored_);
        if (includeSuggestions)
            append(suggestions_);
        if (cancellation.IsCancellationRequested())
            return Result<NavigationCookedLinkSet>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
        output.fingerprint = hash.Finalize();
        return Result<NavigationCookedLinkSet>::Success(std::move(output));
    }
}  // namespace Horo::Navigation
