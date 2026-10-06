#include "Horo/Application/ProjectSourceDocument.h"
#include "Horo/Packages/PackageLockfile.h"
#include "Horo/Packages/PackageRequest.h"
#include "PrefabSceneCookState.h"

#include <array>
#include <fstream>
#include <limits>

namespace Horo::Application {
    namespace PrefabSceneCookErrors {
        /** @copydoc Invalid */
        const ErrorCodeDescriptor Invalid{ErrorDomainId{"horo.host.prefab_cook"},
                                          ErrorCode{"prefab.cook.host_invalid"},
                                          ErrorSeverity::Error,
                                          "Prefab cook host inputs are not admissible.",
                                          "Complete project migration and package restore, then validate source inputs.",
                                          false,
                                          true};
        /** @copydoc Stale */
        const ErrorCodeDescriptor Stale{ErrorDomainId{"horo.host.prefab_cook"},
                                        ErrorCode{"prefab.cook.host_stale"},
                                        ErrorSeverity::Error,
                                        "Prefab cook inputs changed before publication.",
                                        "Capture a new project/source snapshot and retry the explicit operation.",
                                        true,
                                        true};
        /** @copydoc Cancelled */
        const ErrorCodeDescriptor Cancelled{ErrorDomainId{"horo.host.prefab_cook"},
                                            ErrorCode{"prefab.cook.host_cancelled"},
                                            ErrorSeverity::Info,
                                            "Prefab cook was cancelled.",
                                            "Start a new cook when required.",
                                            true,
                                            false};
    }  // namespace PrefabSceneCookErrors

    namespace PrefabCookDetail {
        namespace {
            /** @brief Reads an exact bounded control document without following linked path components. */
            Result<std::optional<std::string>> ReadControl(const std::filesystem::path &root, const char *name,
                                                           const std::size_t maximumBytes, const bool required) {
                std::error_code error;
                const auto directory = root / ".horo";
                const auto path = directory / name;
                const auto directoryStatus = std::filesystem::symlink_status(directory, error);
                if (error || !std::filesystem::is_directory(directoryStatus) || std::filesystem::is_symlink(directoryStatus))
                    return Result<std::optional<std::string>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                const auto status = std::filesystem::symlink_status(path, error);
                if (!required && error == std::errc::no_such_file_or_directory)
                    return Result<std::optional<std::string>>::Success(std::nullopt);
                if (!required && !error && status.type() == std::filesystem::file_type::not_found)
                    return Result<std::optional<std::string>>::Success(std::nullopt);
                if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status))
                    return Result<std::optional<std::string>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                const auto bytes = std::filesystem::file_size(path, error);
                if (error || bytes > maximumBytes || bytes > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
                    return Result<std::optional<std::string>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                std::string contents(static_cast<std::size_t>(bytes), '\0');
                std::ifstream stream{path, std::ios::binary};
                if (!stream || !stream.read(contents.data(), static_cast<std::streamsize>(bytes)) ||
                    stream.peek() != std::char_traits<char>::eof())
                    return Result<std::optional<std::string>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                return Result<std::optional<std::string>>::Success(std::move(contents));
            }

            /** @brief Length-delimits semantic fields; native paths, timestamps and editor labels are not cooker settings. */
            void Append(std::string &identity, const std::string_view value) {
                identity += std::to_string(value.size());
                identity += ':';
                identity += value;
            }

            /** @brief Requires real immutable archive evidence for every exact package selected by the actual lockfile. */
            Result<std::string> PackageIdentity(const std::optional<std::string> &source, const std::optional<std::string> &intentBytes,
                                                const std::shared_ptr<const Packages::PackageRestoreGraph> &restored) {
                auto intent = Packages::ValidatedPackageRequest::Parse(intentBytes.value_or(R"({"sources":{},"dependencies":{}})"));
                if (intent.HasError())
                    return Result<std::string>::Failure(intent.ErrorValue());
                if (!source) {
                    if (!intent.Value().Dependencies().empty() || (restored && !restored->packages.empty()))
                        return Result<std::string>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                    return Result<std::string>::Success("no-package-lock:" + intent.Value().SerializeCanonical());
                }
                if (!intentBytes)
                    return Result<std::string>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                auto parsed = Packages::ValidatedPackageLockfileV1::Parse(*source);
                if (parsed.HasError())
                    return Result<std::string>::Failure(parsed.ErrorValue());
                const auto &lock = parsed.Value();
                if (auto valid = intent.Value().ValidateLock(lock); valid.HasError())
                    return Result<std::string>::Failure(valid.ErrorValue());
                if (!restored || lock.Packages().size() != restored->packages.size())
                    return Result<std::string>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                if (auto valid = lock.ValidateForRestore(restored->requestHash, restored->platform); valid.HasError())
                    return Result<std::string>::Failure(valid.ErrorValue());
                for (std::size_t index = 0; index < lock.Packages().size(); ++index) {
                    const auto &expected = lock.Packages()[index];
                    const auto &actual = restored->packages[index];
                    if (!actual.archive || actual.lock.package != expected.package || actual.lock.version != expected.version ||
                        actual.lock.source != expected.source || actual.lock.artifactDigest != expected.artifactDigest ||
                        actual.lock.manifestDigest != expected.manifestDigest ||
                        actual.lock.fileManifestDigest != expected.fileManifestDigest ||
                        actual.lock.packageFormatVersion != expected.packageFormatVersion || actual.lock.platforms != expected.platforms ||
                        actual.lock.dependencies != expected.dependencies || actual.lock.contributions != expected.contributions ||
                        actual.archive->Digest() != expected.artifactDigest ||
                        actual.archive->PackageManifestDigest() != expected.manifestDigest ||
                        actual.archive->Manifest().Digest() != expected.fileManifestDigest)
                        return Result<std::string>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                }
                return Result<std::string>::Success(intent.Value().SerializeCanonical() + lock.SerializeCanonical());
            }

            /** @brief Captures every policy ceiling as a scalar; no object layout or process-local revision enters the key. */
            std::string PolicyIdentity(const PrefabSceneCookRequest &request) {
                const auto &p = request.prefabPolicy;
                const std::array values{p.maximumHierarchyDepth,
                                        p.maximumObjectCount,
                                        p.maximumSourcePayloadBytes,
                                        p.maximumExpandedPayloadBytes,
                                        p.maximumCookedPayloadBytes,
                                        p.maximumComponentsPerObject,
                                        p.maximumReferencedAssets,
                                        p.maximumDirectNestedPlacements,
                                        p.maximumVariantInheritanceDepth,
                                        p.maximumNestedPrefabDepth,
                                        p.maximumOverrideRecords,
                                        p.maximumConflictAndOrphanRecords,
                                        p.maximumPropertyPathSegments,
                                        p.maximumOverrideValueBytes,
                                        p.maximumOverrideSetBytes,
                                        p.maximumBindingSlots,
                                        p.maximumBindingUses,
                                        p.maximumInstanceBindings,
                                        p.maximumRuntimeSpawnDepth,
                                        request.sceneLimits.maximumEntities,
                                        request.sceneLimits.maximumDependencies,
                                        request.sceneLimits.maximumBytes};
                std::string result;
                for (const auto value : values)
                    Append(result, std::to_string(value));
                return result;
            }
        }  // namespace

        /** @copydoc CaptureHost */
        Result<HostCapture> CaptureHost(const PrefabSceneCookRequest &request, const ProjectCompatibilityInspector &compatibility,
                                        const Release::ReleaseExecutionPlan *releasePlan) {
            const auto classified = compatibility.Inspect(request.assets.sourceRoot);
            using enum ProjectCompatibilityStatus;
            if ((classified.status != Current && classified.status != CompatibleReleaseLine) || classified.markerUpdateRequired ||
                !classified.metadata)
                return Result<HostCapture>::Failure(classified.diagnostic.value_or(MakeError(PrefabSceneCookErrors::Invalid)));
            auto rawProject = ReadControl(request.assets.sourceRoot, "project.json", 1U * 1024U * 1024U, true);
            if (rawProject.HasError())
                return Result<HostCapture>::Failure(rawProject.ErrorValue());
            auto document = DecodeProjectSourceDocument(*rawProject.Value());
            if (document.HasError())
                return Result<HostCapture>::Failure(document.ErrorValue());
            const auto &metadata = document.Value().metadata;
            if (metadata.horoVersion != classified.metadata->horoVersion ||
                metadata.persistentContract != classified.metadata->persistentContract)
                return Result<HostCapture>::Failure(MakeError(PrefabSceneCookErrors::Stale));
            auto rawPackages = ReadControl(request.assets.sourceRoot, "packages.lock", 4U * 1024U * 1024U, false);
            if (rawPackages.HasError())
                return Result<HostCapture>::Failure(rawPackages.ErrorValue());
            auto rawIntent = ReadControl(request.assets.sourceRoot, "packages.json", 1U * 1024U * 1024U, false);
            if (rawIntent.HasError())
                return Result<HostCapture>::Failure(rawIntent.ErrorValue());
            auto packages = PackageIdentity(rawPackages.Value(), rawIntent.Value(), request.restoredPackages);
            if (packages.HasError())
                return Result<HostCapture>::Failure(packages.ErrorValue());
            std::string semantic{"horo.prefab-scene.host.v1"};
            Append(semantic, FormatHoroVersion(metadata.horoVersion.value));
            Append(semantic, FormatPersistentContractHash(metadata.persistentContract));
            Append(semantic, packages.Value());
            Append(semantic, PolicyIdentity(request));
            Append(semantic, request.schemas ? FormatSha256(request.schemas->Digest()) : "no-project-schema-context");
            if (releasePlan) {
                const auto actualLockDigest = rawPackages.Value() ? ComputeSha256(std::as_bytes(std::span{*rawPackages.Value()}))
                                                                  : ComputeSha256(std::span<const std::byte>{});
                if (actualLockDigest != releasePlan->Identities().dependencyLock)
                    return Result<HostCapture>::Failure(MakeError(PrefabSceneCookErrors::Stale));
                Append(semantic, FormatSha256(releasePlan->Identities().profile));
                Append(semantic, FormatSha256(releasePlan->Identities().policy));
                Append(semantic, FormatSha256(releasePlan->Identities().toolchain));
            }
            return Result<HostCapture>::Success(
                {metadata, ComputeSha256(std::as_bytes(std::span{semantic})), ComputeSha256(std::as_bytes(std::span{*rawProject.Value()})),
                 rawPackages.Value() ? std::optional{ComputeSha256(std::as_bytes(std::span{*rawPackages.Value()}))} : std::nullopt,
                 rawIntent.Value() ? std::optional{ComputeSha256(std::as_bytes(std::span{*rawIntent.Value()}))} : std::nullopt});
        }

        /** @copydoc VerifyHost */
        Result<void> VerifyHost(const PrefabSceneCookRequest &request, const HostCapture &capture,
                                const ProjectCompatibilityInspector &compatibility, const Release::ReleaseExecutionPlan *releasePlan) {
            auto current = CaptureHost(request, compatibility, releasePlan);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (current.Value().projectBytes != capture.projectBytes || current.Value().packageBytes != capture.packageBytes ||
                current.Value().packageIntentBytes != capture.packageIntentBytes ||
                current.Value().semanticDigest != capture.semanticDigest)
                return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Stale));
            return Result<void>::Success();
        }
    }  // namespace PrefabCookDetail
}  // namespace Horo::Application
