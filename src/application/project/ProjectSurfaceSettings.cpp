#include "Horo/Application/ProjectSurfaceSettings.h"

#include <algorithm>
#include <new>

namespace Horo::Application {
    namespace ProjectSurfaceErrors {
        const ErrorCodeDescriptor Invalid{.domain = ErrorDomainId{"horo.project.surface"},
                                          .code = ErrorCode{"surface.document.invalid"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "The project surface catalog is invalid.",
                                          .remediationHint = "Repair .horo/surfaces.json using the project's exact persistent contract.",
                                          .userActionable = true};
        const ErrorCodeDescriptor Version{.domain = ErrorDomainId{"horo.project.surface"},
                                          .code = ErrorCode{"surface.document.version"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "The surface catalog format requires migration.",
                                          .remediationHint =
                                              "Open the project with a compatible Horo release and its registered migration chain.",
                                          .userActionable = true};
        const ErrorCodeDescriptor Unavailable{.domain = ErrorDomainId{"horo.project.surface"},
                                              .code = ErrorCode{"surface.source.unavailable"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The exact project surface catalog is unavailable.",
                                              .remediationHint =
                                                  "Adopt the surface catalog through project migration; no semantic default is inferred.",
                                              .userActionable = true};
        const ErrorCodeDescriptor Capacity{.domain = ErrorDomainId{"horo.project.surface"},
                                           .code = ErrorCode{"surface.document.capacity"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "The surface catalog exceeds its admitted capacity.",
                                           .remediationHint = "Use at most 256 surfaces, 128-byte names and a 256 KiB source.",
                                           .userActionable = true};
    }  // namespace ProjectSurfaceErrors

    /** @copydoc ProjectSurfaceSettings::Create */
    Result<ProjectSurfaceSettings> ProjectSurfaceSettings::Create(ProjectSurfaceSettingsInput input) {
        try {
            if (input.schemaVersion != 1)
                return Result<ProjectSurfaceSettings>::Failure(MakeError(ProjectSurfaceErrors::Version));
            auto catalog = Physics::SurfaceCatalogSnapshot::Create({std::move(input.projectId), input.revision, std::move(input.surfaces)});
            if (catalog.HasError())
                return Result<ProjectSurfaceSettings>::Failure(catalog.ErrorValue());
            if (input.defaultSurfaceId && !catalog.Value().Contains(*input.defaultSurfaceId))
                return Result<ProjectSurfaceSettings>::Failure(
                    MakeError(ProjectSurfaceErrors::Invalid, "The explicit default is absent from the catalog."));
            input.projectId = catalog.Value().ProjectId();
            const auto entries = catalog.Value().Entries();
            input.surfaces.assign(entries.begin(), entries.end());
            ProjectSurfaceSettings settings{std::move(input),
                                            std::make_shared<const Physics::SurfaceCatalogSnapshot>(std::move(catalog).Value())};
            auto encoded = EncodeProjectSurfaceSettings(settings);
            if (encoded.HasError())
                return Result<ProjectSurfaceSettings>::Failure(encoded.ErrorValue());
            settings.fingerprint_ = ComputeSha256(std::as_bytes(std::span{encoded.Value().data(), encoded.Value().size()}));
            return Result<ProjectSurfaceSettings>::Success(std::move(settings));
        } catch (const std::bad_alloc &) {
            return Result<ProjectSurfaceSettings>::Failure(MakeError(ProjectSurfaceErrors::Capacity));
        }
    }

    ProjectSurfaceSettings::ProjectSurfaceSettings(ProjectSurfaceSettingsInput input,
                                                   std::shared_ptr<const Physics::SurfaceCatalogSnapshot> catalog)
        : input_(std::move(input)), catalog_(std::move(catalog)) {}

    /** @copydoc ProjectSurfaceSettings::Input */
    const ProjectSurfaceSettingsInput &ProjectSurfaceSettings::Input() const noexcept {
        return input_;
    }

    /** @copydoc ProjectSurfaceSettings::Catalog */
    const std::shared_ptr<const Physics::SurfaceCatalogSnapshot> &ProjectSurfaceSettings::Catalog() const noexcept {
        return catalog_;
    }

    /** @copydoc ProjectSurfaceSettings::Capture */
    Result<Physics::PhysicsSurfaceWorldBinding> ProjectSurfaceSettings::Capture(const std::uint64_t mappingGeneration) const {
        Physics::PhysicsSurfaceWorldBinding binding{catalog_, mappingGeneration, input_.defaultSurfaceId};
        auto valid = Physics::ValidatePhysicsSurfaceWorldBinding(binding);
        if (valid.HasError())
            return Result<Physics::PhysicsSurfaceWorldBinding>::Failure(valid.ErrorValue());
        return Result<Physics::PhysicsSurfaceWorldBinding>::Success(std::move(binding));
    }

    /** @copydoc ProjectSurfaceSettings::Fingerprint */
    const Sha256Digest &ProjectSurfaceSettings::Fingerprint() const noexcept {
        return fingerprint_;
    }
}  // namespace Horo::Application
