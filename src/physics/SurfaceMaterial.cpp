#include "Horo/Physics/SurfaceMaterial.h"

#include "Horo/Foundation/String.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <new>

namespace Horo::Physics {
    namespace SurfaceErrors {
        const ErrorCodeDescriptor Invalid{.domain = ErrorDomainId{"horo.physics.surface"},
                                          .code = ErrorCode{"physics.surface.invalid"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "The project surface catalogue or binding is malformed.",
                                          .remediationHint =
                                              "Supply bounded canonical semantic identities and exact captured catalogue membership.",
                                          .userActionable = true};
        const ErrorCodeDescriptor Limit{.domain = ErrorDomainId{"horo.physics.surface"},
                                        .code = ErrorCode{"physics.surface.limit"},
                                        .defaultSeverity = ErrorSeverity::Error,
                                        .summary = "The project surface capture exceeds its bounded capacity.",
                                        .remediationHint = "Reduce catalogue entries or text to the declared limits.",
                                        .userActionable = true};
        const ErrorCodeDescriptor Cancelled{.domain = ErrorDomainId{"horo.physics.surface"},
                                            .code = ErrorCode{"physics.surface.cancelled"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "The project surface capture was cancelled.",
                                            .remediationHint = "Retry with a current operation.",
                                            .userActionable = true};
        const ErrorCodeDescriptor Stale{.domain = ErrorDomainId{"horo.physics.surface"},
                                        .code = ErrorCode{"physics.surface.stale"},
                                        .defaultSeverity = ErrorSeverity::Error,
                                        .summary = "Surface evidence does not match the admitted world mapping.",
                                        .remediationHint = "Use the exact catalogue and mapping generation captured by the world.",
                                        .userActionable = true};
    }  // namespace SurfaceErrors

    namespace {
        /** @brief Detect C0/C1 controls after canonical UTF-8 validation. */
        bool ControlAt(std::string_view text, std::size_t index) noexcept {
            const auto byte = static_cast<unsigned char>(text[index]);
            if (byte < 32 || byte == 127)
                return true;
            if (byte != 0xc2 || index + 1 == text.size())
                return false;
            return static_cast<unsigned char>(text[index + 1]) <= 0x9f;
        }

        bool HasControls(std::string_view text) noexcept {
            for (std::size_t index = 0; index < text.size(); ++index)
                if (ControlAt(text, index))
                    return true;
            return false;
        }

        /** @brief Bound display/project text without duplicating Foundation UTF-8 validation. */
        bool Text(std::string_view text) noexcept {
            if (text.empty() || text.size() > MaximumSurfaceDisplayNameBytes)
                return false;
            return IsValidUtf8ScalarSequence(text) && !HasControls(text);
        }

        /** @brief Match the Project identity authority without applying display-label restrictions to opaque bytes. */
        bool ProjectIdentity(std::string_view value) noexcept {
            return value.size() <= 256 && IsValidUtf8ScalarSequence(value) && !Horo::Text::IsBlank(value);
        }

        void Word(Sha256Builder &builder, std::uint64_t word) {
            std::array<std::byte, 8> bytes{};
            for (auto &byte : bytes) {
                byte = static_cast<std::byte>(word & 255);
                word >>= 8;
            }
            static_cast<void>(builder.Update(bytes));
        }

        void HashText(Sha256Builder &builder, std::string_view text) {
            Word(builder, text.size());
            static_cast<void>(builder.Update(std::as_bytes(std::span{text.data(), text.size()})));
        }

        Sha256Digest Digest(const SurfaceCatalogDescriptor &value) {
            Sha256Builder builder;
            HashText(builder, "horo.project.surfaces.v1");
            HashText(builder, value.projectId);
            Word(builder, value.revision);
            Word(builder, value.surfaces.size());
            for (const auto &entry : value.surfaces) {
                static_cast<void>(builder.Update(std::as_bytes(std::span{entry.id.Bytes()})));
                HashText(builder, entry.displayName);
            }
            return builder.Finalize();
        }

        Result<void> Validate(SurfaceCatalogDescriptor &value, const CancellationToken &cancellation) {
            if (!ProjectIdentity(value.projectId) || value.revision == 0)
                return Result<void>::Failure(MakeError(SurfaceErrors::Invalid));
            if (value.surfaces.size() > MaximumSurfaceCatalogEntries)
                return Result<void>::Failure(MakeError(SurfaceErrors::Limit));
            for (const auto &entry : value.surfaces) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(SurfaceErrors::Cancelled));
                if (!entry.id.IsValid() || !Text(entry.displayName))
                    return Result<void>::Failure(MakeError(SurfaceErrors::Invalid));
            }
            std::ranges::sort(value.surfaces, {}, &SurfaceCatalogEntry::id);
            if (std::ranges::adjacent_find(value.surfaces, {}, &SurfaceCatalogEntry::id) != value.surfaces.end())
                return Result<void>::Failure(MakeError(SurfaceErrors::Invalid));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc SurfaceMaterialId::Parse */
    Result<SurfaceMaterialId> SurfaceMaterialId::Parse(std::string_view text) {
        auto parsed = Assets::AssetId::Parse(text);
        if (parsed.HasError())
            return Result<SurfaceMaterialId>::Failure(MakeError(SurfaceErrors::Invalid));
        return Result<SurfaceMaterialId>::Success(SurfaceMaterialId{parsed.Value()});
    }

    /** @copydoc SurfaceCatalogSnapshot::Create */
    Result<SurfaceCatalogSnapshot> SurfaceCatalogSnapshot::Create(SurfaceCatalogDescriptor descriptor,
                                                                  const CancellationToken &cancellation) {
        using Return = Result<SurfaceCatalogSnapshot>;
        try {
            if (cancellation.IsCancellationRequested())
                return Return::Failure(MakeError(SurfaceErrors::Cancelled));
            if (auto valid = Validate(descriptor, cancellation); valid.HasError())
                return Return::Failure(std::move(valid).ErrorValue());
            SurfaceCatalogSnapshot result;
            result.fingerprint_ = Digest(descriptor);
            result.value_ = std::move(descriptor);
            return Return::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(SurfaceErrors::Limit));
        }
    }

    /** @copydoc SurfaceCatalogSnapshot::Find */
    const SurfaceCatalogEntry *SurfaceCatalogSnapshot::Find(SurfaceMaterialId id) const noexcept {
        const auto found = std::ranges::lower_bound(value_.surfaces, id, {}, &SurfaceCatalogEntry::id);
        return found != value_.surfaces.end() && found->id == id ? &*found : nullptr;
    }

    /** @copydoc ValidatePhysicsSurfaceWorldBinding */
    Result<void> ValidatePhysicsSurfaceWorldBinding(const PhysicsSurfaceWorldBinding &binding) {
        if (!binding.catalog)
            return binding.mappingGeneration == 0 && !binding.defaultSurface ? Result<void>::Success()
                                                                             : Result<void>::Failure(MakeError(SurfaceErrors::Invalid));
        if (!binding.catalog->IsValid() || binding.mappingGeneration == 0)
            return Result<void>::Failure(MakeError(SurfaceErrors::Invalid));
        if (binding.defaultSurface && !binding.catalog->Contains(*binding.defaultSurface))
            return Result<void>::Failure(MakeError(SurfaceErrors::Invalid));
        return Result<void>::Success();
    }

    /** @copydoc MakePhysicsSurfaceEvidence */
    PhysicsSurfaceEvidence MakePhysicsSurfaceEvidence(const PhysicsSurfaceWorldBinding &binding, SurfaceMaterialId id) noexcept {
        return {id, binding.catalog->Revision(), binding.catalog->Fingerprint(), binding.mappingGeneration};
    }

    /** @copydoc PhysicsSurfaceWorldFingerprint */
    Sha256Digest PhysicsSurfaceWorldFingerprint(const PhysicsSurfaceWorldBinding &binding) noexcept {
        Sha256Builder builder;
        constexpr std::string_view domain = "horo.physics.surface-world.v1";
        static_cast<void>(builder.Update(std::as_bytes(std::span{domain.data(), domain.size()})));
        const Sha256Digest catalog = binding.catalog ? binding.catalog->Fingerprint() : Sha256Digest{};
        static_cast<void>(builder.Update(std::as_bytes(std::span{catalog.bytes})));
        Word(builder, binding.mappingGeneration);
        Word(builder, binding.defaultSurface.has_value());
        if (binding.defaultSurface)
            static_cast<void>(builder.Update(std::as_bytes(std::span{binding.defaultSurface->Bytes()})));
        return builder.Finalize();
    }

    /** @copydoc MatchesPhysicsSurfaceEvidence */
    bool MatchesPhysicsSurfaceEvidence(const PhysicsSurfaceWorldBinding &binding, const PhysicsSurfaceEvidence &evidence) noexcept {
        if (!binding.catalog || !binding.catalog->IsValid())
            return false;
        return binding.mappingGeneration == evidence.mappingGeneration && binding.catalog->Revision() == evidence.catalogRevision &&
               binding.catalog->Fingerprint() == evidence.catalogFingerprint && binding.catalog->Contains(evidence.surface);
    }
}  // namespace Horo::Physics
