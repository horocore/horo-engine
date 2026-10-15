#include "Horo/Editor/PackageModalSession.h"

#include <limits>

namespace Horo::Editor {
    namespace {
        /** @brief Presentation diagnostics preserve application causes by never replacing forwarded failures. */
        Error ViewError(const char *code) {
            return Error{ErrorCode{code}, ErrorDomainId{"horo.editor.packages"}};
        }
    }  // namespace

    /** @copydoc PackageModalSession::PackageModalSession */
    PackageModalSession::PackageModalSession(std::shared_ptr<Application::PackageApplication> application)
        : application_(std::move(application)) {}

    /** @copydoc PackageModalSession::Validate */
    Result<void> PackageModalSession::Validate(const PackageModalGeneration generation) const {
        if (!application_)
            return Result<void>::Failure(ViewError("editor.packages.unavailable"));
        if (!open_ || generation.value == 0 || generation != generation_)
            return Result<void>::Failure(ViewError("editor.packages.stale_view"));
        return Result<void>::Success();
    }

    /** @copydoc PackageModalSession::Open */
    Result<PackageModalGeneration> PackageModalSession::Open() {
        if (!application_)
            return Result<PackageModalGeneration>::Failure(ViewError("editor.packages.unavailable"));
        if (open_ || generation_.value == std::numeric_limits<std::uint64_t>::max())
            return Result<PackageModalGeneration>::Failure(ViewError("editor.packages.view_busy"));
        ++generation_.value;
        open_ = true;
        const auto refreshed = Refresh(generation_);
        if (refreshed.HasError()) {
            open_ = false;
            return Result<PackageModalGeneration>::Failure(refreshed.ErrorValue());
        }
        return Result<PackageModalGeneration>::Success(generation_);
    }

    /** @copydoc PackageModalSession::Refresh */
    Result<void> PackageModalSession::Refresh(const PackageModalGeneration generation) {
        if (const auto valid = Validate(generation); valid.HasError())
            return valid;
        auto current = application_->Current();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        operation_ = std::move(current).Value();
        const auto evidence = application_->Plan();
        plan_ = evidence.HasValue() ? evidence.Value() : nullptr;
        return Result<void>::Success();
    }

    /** @copydoc PackageModalSession::Submit */
    Result<Application::PackageOperationId> PackageModalSession::Submit(const PackageModalGeneration generation,
                                                                        const Application::PackageOperationKind kind) {
        if (const auto valid = Validate(generation); valid.HasError())
            return Result<Application::PackageOperationId>::Failure(valid.ErrorValue());
        switch (kind) {
            case Application::PackageOperationKind::Restore:
                return application_->Restore();
            case Application::PackageOperationKind::Install:
                return application_->Install();
            case Application::PackageOperationKind::Disable:
                return application_->Disable();
            case Application::PackageOperationKind::Activate:
                return Result<Application::PackageOperationId>::Failure(ViewError("editor.packages.selection_required"));
        }
        return Result<Application::PackageOperationId>::Failure(ViewError("editor.packages.action_invalid"));
    }

    /** @copydoc PackageModalSession::Activate */
    Result<Application::PackageOperationId> PackageModalSession::Activate(
        const PackageModalGeneration generation, const std::span<const Packages::EnabledPackageExtension> enabled) {
        if (const auto valid = Validate(generation); valid.HasError())
            return Result<Application::PackageOperationId>::Failure(valid.ErrorValue());
        return application_->Activate(enabled);
    }

    /** @copydoc PackageModalSession::Cancel */
    Result<void> PackageModalSession::Cancel(const PackageModalGeneration generation) {
        if (const auto valid = Validate(generation); valid.HasError())
            return valid;
        if (!operation_)
            return Result<void>::Failure(ViewError("editor.packages.operation_unavailable"));
        return application_->RequestCancel(operation_->id);
    }

    /** @copydoc PackageModalSession::Close */
    Result<void> PackageModalSession::Close(const PackageModalGeneration generation) {
        if (const auto valid = Validate(generation); valid.HasError())
            return valid;
        open_ = false;
        operation_.reset();
        plan_.reset();
        return Result<void>::Success();
    }

    /** @copydoc PackageModalSession::Operation */
    const std::optional<Application::PackageOperationSnapshot> &PackageModalSession::Operation() const noexcept {
        return operation_;
    }

    /** @copydoc PackageModalSession::Plan */
    std::shared_ptr<const Application::PackagePlanSnapshot> PackageModalSession::Plan() const noexcept {
        return plan_;
    }
}  // namespace Horo::Editor
