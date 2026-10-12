#include "UiTemplateCaptureControls.h"

#include "Horo/Packages/PackageRequest.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/UiTemplates/UiTemplateErrors.h"

#include <fstream>

namespace Horo::Application::Detail {
    namespace {
        /** @brief Classifies a protected control path without following symlinks; absence is not a permission failure. */
        Result<bool> ExistsAs(const std::filesystem::path &path, const std::filesystem::file_type expected) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error == std::errc::no_such_file_or_directory || (!error && status.type() == std::filesystem::file_type::not_found))
                return Result<bool>::Success(false);
            if (error || status.type() != expected)
                return Result<bool>::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphInvalid));
            return Result<bool>::Success(true);
        }

        /** @brief Requires actual bounded regular control files beneath the protected project root; missing remains explicit. */
        Result<std::optional<std::string>> ReadControl(const std::filesystem::path &root, const char *name, const std::size_t maximum) {
            using Output = Result<std::optional<std::string>>;
            const auto directory = root / ".horo";
            const auto directoryStatus = ExistsAs(directory, std::filesystem::file_type::directory);
            if (directoryStatus.HasError())
                return Output::Failure(directoryStatus.ErrorValue());
            if (!directoryStatus.Value())
                return Output::Success(std::nullopt);
            const auto file = directory / name;
            const auto status = ExistsAs(file, std::filesystem::file_type::regular);
            if (status.HasError())
                return Output::Failure(status.ErrorValue());
            if (!status.Value())
                return Output::Success(std::nullopt);
            std::error_code error;
            const auto size = std::filesystem::file_size(file, error);
            if (error || size > maximum)
                return Output::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphBudgetExceeded));
            std::ifstream input{file, std::ios::binary};
            std::string bytes(static_cast<std::size_t>(size), '\0');
            input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!input || input.peek() != std::char_traits<char>::eof())
                return Output::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphInvalid));
            return Output::Success(std::move(bytes));
        }

        /** @brief Matches exact package origin and every selected content commitment. */
        bool SameContent(const Packages::LockedPackage &expected, const Packages::LockedPackage &actual) {
            return actual.package == expected.package && actual.version == expected.version && actual.source == expected.source &&
                   actual.artifactDigest == expected.artifactDigest && actual.manifestDigest == expected.manifestDigest &&
                   actual.fileManifestDigest == expected.fileManifestDigest;
        }

        /** @brief Matches the lock's declared closure and format without recomputing a package policy. */
        bool SameClosure(const Packages::LockedPackage &expected, const Packages::LockedPackage &actual) {
            return actual.packageFormatVersion == expected.packageFormatVersion && actual.platforms == expected.platforms &&
                   actual.dependencies == expected.dependencies && actual.contributions == expected.contributions;
        }

        /** @brief Requires actual accepted publisher evidence under the existing owning policy. */
        bool PublisherAccepted(const Packages::LockedPackage &expected, const Packages::PackageRestorePackage &actual,
                               const bool required) {
            if (required && (!actual.publisher || actual.publisher->outcome != Packages::PackagePublisherVerificationOutcome::Accepted))
                return false;
            return !actual.publisher || (actual.publisher->installPermitted && actual.publisher->artifactDigest == expected.artifactDigest);
        }

        /** @brief Reuses exact lock and verified archive evidence from PackageRestore without evaluating another trust policy. */
        bool Matches(const Packages::LockedPackage &expected, const Packages::PackageRestorePackage &actual, const bool requirePublisher) {
            return actual.archive && SameContent(expected, actual.lock) && SameClosure(expected, actual.lock) &&
                   actual.archive->Digest() == expected.artifactDigest &&
                   actual.archive->PackageManifestDigest() == expected.manifestDigest &&
                   actual.archive->Manifest().Digest() == expected.fileManifestDigest &&
                   PublisherAccepted(expected, actual, requirePublisher);
        }

        /** @brief Distinguishes truly dependency-free intent from a missing mandatory restore lock. */
        Result<std::string> UnlockedIdentity(const Packages::ValidatedPackageRequest &intent,
                                             const std::shared_ptr<const Packages::PackageRestoreGraph> &restored) {
            if (!intent.Dependencies().empty() || (restored && !restored->packages.empty()))
                return Result<std::string>::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphInvalid));
            return Result<std::string>::Success("no-package-lock:" + intent.SerializeCanonical());
        }

        /** @brief Validates the entire selected restore closure before any package commitment is retained. */
        Result<void> ValidateLock(const Packages::ValidatedPackageRequest &intent, const Packages::ValidatedPackageLockfileV1 &lock,
                                  const Packages::PackageRestoreGraph &restored, const bool requirePublisher) {
            if (const auto valid = intent.ValidateLock(lock); valid.HasError())
                return valid;
            if (const auto valid = lock.ValidateForRestore(restored.requestHash, restored.platform); valid.HasError())
                return valid;
            if (lock.Packages().size() != restored.packages.size())
                return Result<void>::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphInvalid));
            for (std::size_t i = 0; i < restored.packages.size(); ++i)
                if (!Matches(lock.Packages()[i], restored.packages[i], requirePublisher))
                    return Result<void>::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphInvalid));
            return Result<void>::Success();
        }

        /** @brief Binds actual current intent/lock bytes to the existing restore owner's whole verified graph. */
        Result<std::string> PackageIdentity(const std::optional<std::string> &intentBytes, const std::optional<std::string> &lockBytes,
                                            const std::shared_ptr<const Packages::PackageRestoreGraph> &restored,
                                            const bool requirePublisher) {
            using Output = Result<std::string>;
            const auto intent = Packages::ValidatedPackageRequest::Parse(intentBytes.value_or(R"({"sources":{},"dependencies":{}})"));
            if (intent.HasError())
                return Output::Failure(intent.ErrorValue());
            if (!lockBytes)
                return UnlockedIdentity(intent.Value(), restored);
            if (!intentBytes || !restored)
                return Output::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphInvalid));
            const auto lock = Packages::ValidatedPackageLockfileV1::Parse(*lockBytes);
            if (lock.HasError())
                return Output::Failure(lock.ErrorValue());
            if (const auto valid = ValidateLock(intent.Value(), lock.Value(), *restored, requirePublisher); valid.HasError())
                return Output::Failure(valid.ErrorValue());
            return Output::Success(intent.Value().SerializeCanonical() + lock.Value().SerializeCanonical());
        }
    }  // namespace

    Result<UiTemplateCaptureControls> ReadTemplateControls(const std::filesystem::path &root,
                                                           std::shared_ptr<const Packages::PackageRestoreGraph> graph) {
        using Output = Result<UiTemplateCaptureControls>;
        auto intent = ReadControl(root, "packages.json", 1U * 1024U * 1024U);
        if (intent.HasError())
            return Output::Failure(intent.ErrorValue());
        auto lock = ReadControl(root, "packages.lock", 4U * 1024U * 1024U);
        if (lock.HasError())
            return Output::Failure(lock.ErrorValue());
        return Output::Success({std::move(intent).Value(), std::move(lock).Value(), std::move(graph)});
    }

    Result<std::string> ValidateTemplatePackages(const UiTemplateCaptureControls &controls, const bool requirePublisher) {
        return PackageIdentity(controls.intent, controls.lock, controls.graph, requirePublisher);
    }

    Result<void> VerifyTemplateControls(const std::filesystem::path &root, const UiTemplateCaptureControls &captured,
                                        std::shared_ptr<const Packages::PackageRestoreGraph> currentGraph) {
        const auto current = ReadTemplateControls(root, std::move(currentGraph));
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (current.Value().intent != captured.intent || current.Value().lock != captured.lock || current.Value().graph != captured.graph)
            return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::RevisionStale));
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Detail
