#include "EditorOpenXRBootstrap.h"

#include "Horo/Foundation/JobSystem.h"
#include "Horo/Security/SecurityErrors.h"
#include "Horo/XR/XRProjectSettings.h"
#include "HostSignatureDocument.h"
#include "OpenXRInstalledBindings.h"
#include "OpenXRInstalledHostCatalog.h"
#include "PackageHostPolicy.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>

namespace Horo::Editor {
    namespace {
        using namespace Application::Internal;

        /** @brief Explicit invocation inputs; neither project settings nor a signed catalog supplies its trust file. */
        struct HostArguments final {
            std::array<std::optional<std::filesystem::path>, 3> paths;
            bool requested{};
        };

        /** @brief Keeps invalid explicit host requests closed, rather than silently continuing as ordinary desktop. */
        Error BootstrapError(const char *message) {
            return MakeError(XR::XRErrors::OperationUnavailable, message);
        }

        /** @brief Admits an existing canonical directory without following a leaf redirect. */
        Result<void> AdmitCacheDirectory(const NativeDurableFileSystem &files, const std::filesystem::path &path) {
            const auto entry = files.InspectEntry(path);
            if (entry.HasError())
                return Result<void>::Failure(entry.ErrorValue());
            if (!entry.Value() || entry.Value()->kind != FileEntryKind::Directory)
                return Result<void>::Failure(BootstrapError("XR cache storage must be an existing directory, never a file or redirect."));
            std::error_code error;
            if (std::filesystem::canonical(path, error) != path || error)
                return Result<void>::Failure(BootstrapError("XR cache storage must retain its exact canonical host-owned location."));
            return Result<void>::Success();
        }

        /** @brief Prepares only the verified bootstrap's two owned children before strict package admission. */
        Result<void> PrepareNativeCacheParents(NativeDurableFileSystem &files, const std::filesystem::path &cache) {
            if (auto admitted = AdmitCacheDirectory(files, cache); admitted.HasError())
                return admitted;
            for (const char *name : {"native", "xr-native"}) {
                const auto child = cache / name;
                const auto entry = files.InspectEntry(child);
                if (entry.HasError())
                    return Result<void>::Failure(entry.ErrorValue());
                if (!entry.Value()) {
                    const auto created = files.CreateDirectoryPath(child, DirectoryCreationMode::Single);
                    if (created.HasError())
                        return Result<void>::Failure(created.ErrorValue());
                }
                if (auto admitted = AdmitCacheDirectory(files, child); admitted.HasError())
                    return admitted;
            }
            return AdmitCacheDirectory(files, cache);
        }

        /** @brief Parses the actual process's three independent host inputs once, rejecting duplicate/partial requests. */
        Result<HostArguments> ParseHostArguments(const std::span<char *> arguments) {
            HostArguments parsed;
            constexpr std::array<std::string_view, 3> flags{"--xr-host-roots", "--xr-host-catalog", "--xr-host-catalog-signature"};
            std::size_t index = 1;
            while (index < arguments.size()) {
                if (arguments[index] == nullptr)
                    return Result<HostArguments>::Failure(BootstrapError("Explicit XR host input contains a null argument."));
                const std::string_view argument{arguments[index++]};
                if (!argument.starts_with("--xr-host-"))
                    continue;
                parsed.requested = true;
                const auto found = std::ranges::find(flags, argument);
                if (found == flags.end())
                    return Result<HostArguments>::Failure(BootstrapError("Explicit XR host input is unknown or missing its value."));
                if (index == arguments.size() || arguments[index] == nullptr)
                    return Result<HostArguments>::Failure(BootstrapError("Explicit XR host input is unknown or missing its value."));
                const auto slot = static_cast<std::size_t>(found - flags.begin());
                const std::filesystem::path path{arguments[index++]};
                if (parsed.paths[slot] || path.empty() || !path.is_absolute() || path != path.lexically_normal())
                    return Result<HostArguments>::Failure(
                        BootstrapError("XR host paths must be unique explicit normalized absolute paths."));
                parsed.paths[slot] = path;
            }
            if (parsed.requested && std::ranges::any_of(parsed.paths, [](const auto &path) {
                return !path;
            }))
                return Result<HostArguments>::Failure(
                    BootstrapError("XR host roots, signed catalog and catalog signature must be supplied together."));
            return Result<HostArguments>::Success(std::move(parsed));
        }

        /** @brief Trusted wall time is used only for signing-policy expiry, never as an XR/simulation timestamp. */
        Result<std::uint64_t> UnixSeconds(const WallClock &clock) {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(clock.UtcNow().time_since_epoch()).count();
            return seconds < 0 ? Result<std::uint64_t>::Failure(BootstrapError("Host wall time cannot validate installed XR trust."))
                               : Result<std::uint64_t>::Success(static_cast<std::uint64_t>(seconds));
        }

        /** @brief Builds roots only from independently approved active records; parsing the catalog never reaches this authority. */
        Result<std::shared_ptr<const Security::TrustedRootStore>> Roots(const Packages::PackagePublisherVerificationPolicy &policy,
                                                                        const std::uint64_t now) {
            using Outcome = Result<std::shared_ptr<const Security::TrustedRootStore>>;
            if (policy.allowUnsigned || policy.publishers.empty())
                return Outcome::Failure(BootstrapError("XR installation requires an explicit signed-only host publisher policy."));
            auto roots = std::make_shared<Security::TrustedRootStore>();
            for (const auto &entry : policy.publishers) {
                if (entry.publisherRevoked || (entry.expiresAtUnixMilliseconds != 0 && entry.expiresAtUnixMilliseconds / 1000 <= now))
                    continue;
                if (auto added = roots->Add({entry.algorithm, entry.publisher.Value(), entry.keyId, entry.publicKey}); added.HasError())
                    return Outcome::Failure(added.ErrorValue());
            }
            return Outcome::Success(std::move(roots));
        }

        /** @brief Reads and verifies the explicitly selected catalog with the independently constructed host verifier. */
        Result<OpenXRInstalledHostCatalog> ReadSignedCatalog(const NativeDurableFileSystem &files, const HostArguments &arguments,
                                                             std::shared_ptr<const Security::ArtifactVerifier> verifier,
                                                             const std::filesystem::path &cacheRoot, std::uint64_t now) {
            auto signatureBytes = files.ReadFileBounded(*arguments.paths[2], MaximumHostSignatureDocumentBytes);
            if (signatureBytes.HasError())
                return Result<OpenXRInstalledHostCatalog>::Failure(signatureBytes.ErrorValue());
            auto signature = DecodeDetachedSignatureDocument(std::as_bytes(std::span{signatureBytes.Value()}));
            if (signature.HasError())
                return Result<OpenXRInstalledHostCatalog>::Failure(signature.ErrorValue());
            auto catalogBytes = files.ReadFileBounded(*arguments.paths[1], 64U * 1024U);
            if (catalogBytes.HasError())
                return Result<OpenXRInstalledHostCatalog>::Failure(catalogBytes.ErrorValue());
            auto platform = NativePackagePlatform();
            if (platform.HasError())
                return Result<OpenXRInstalledHostCatalog>::Failure(platform.ErrorValue());
            auto catalog = OpenXRInstalledHostCatalog::Verify(std::as_bytes(std::span{catalogBytes.Value()}), signature.Value(), verifier,
                                                              platform.Value(), cacheRoot / "xr-native", now);
            return catalog;
        }

        /** @brief Retains only the active independently approved signer expiry; the catalog cannot add trust records. */
        Result<std::uint64_t> ActiveSignerExpiry(const Packages::PackagePublisherVerificationPolicy &policy,
                                                 const OpenXRInstalledHostCatalog &catalog, std::uint64_t now) {
            std::optional<std::uint64_t> rootExpiry;
            for (const auto &entry : policy.publishers) {
                const auto expiry = entry.expiresAtUnixMilliseconds / 1000;
                if (!entry.publisherRevoked && (entry.expiresAtUnixMilliseconds == 0 || expiry > now) &&
                    entry.publisher.Value() == catalog.Evidence().PublisherId() && entry.keyId == catalog.Evidence().KeyId()) {
                    rootExpiry = expiry;
                    break;
                }
            }
            if (!rootExpiry.has_value())
                return Result<std::uint64_t>::Failure(
                    BootstrapError("Verified XR catalog signer no longer matches the active approved host policy."));
            return Result<std::uint64_t>::Success(*rootExpiry);
        }
    }  // namespace

    /** @brief Retains every canonical package dependency through startup failure, cancellation and final native drain. */
    struct EditorOpenXRBootstrap::Impl final {
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 32, .ioWorkerCount = 1}};
        NativeDurableFileSystem files;
        SystemWallClock wall;
        SteadyClock clock;
        Application::Internal::UnconfiguredPackageTrust executionTrust;
        Packages::PackagePublisherVerificationService publishers;
        Application::Internal::OpenXRInstalledHostCatalog catalog;
        std::shared_ptr<Application::Internal::PackageProjectHost> packages;
        std::optional<EditorOpenXRHostServices> services;
        std::uint64_t approvedAt{};
        std::uint64_t rootExpiresAt{};
        bool closing{};

        Impl(Packages::PackagePublisherVerificationService publisherService, Application::Internal::OpenXRInstalledHostCatalog selected,
             const std::uint64_t approval, const std::uint64_t rootExpiry)
            : publishers(std::move(publisherService)), catalog(std::move(selected)), approvedAt(approval), rootExpiresAt(rootExpiry) {}

        ~Impl() {
            // Existing editor worker shutdown ordering: retain package services while cancellation joins their callbacks.
            closing = true;
            try {
                if (packages)
                    static_cast<void>(packages->Close());
            } catch (...) {
                std::fputs("XR package close raised; retaining owners while joining package jobs.\n", stderr);
            }
            jobs.StopAccepting();
            jobs.Shutdown(ShutdownPolicy::Cancel);
            services.reset();
            try {
                if (packages && packages->Close().HasError())
                    std::terminate();
            } catch (...) {
                std::terminate();
            }
        }

        /** @brief Allocation-free successful host policy fence; expiry/rollback revokes without manufacturing runtime state. */
        Result<void> Current() const {
            if (closing)
                return Result<void>::Failure(BootstrapError("Installed XR host is closing."));
            const auto now = UnixSeconds(wall);
            if (now.HasError())
                return Result<void>::Failure(now.ErrorValue());
            if (now.Value() < approvedAt || now.Value() >= catalog.ExpiresAt() || (rootExpiresAt != 0 && now.Value() >= rootExpiresAt))
                return Result<void>::Failure(MakeError(SecurityErrors::CredentialExpired));
            return Result<void>::Success();
        }

        /** @brief Advances only acquisition/start for the same local startup transaction; all job authority remains canonical. */
        Result<void> BeginInstall(const std::filesystem::path &project, std::shared_ptr<Application::PackageApplication> &application,
                                  std::optional<Application::PackageOperationId> &operation) const {
            if (!application) {
                auto acquired = packages->Acquire(project);
                if (acquired.HasValue())
                    application = std::move(acquired).Value();
                else if (acquired.ErrorValue().code.Value() != "packages.host.preparing")
                    return Result<void>::Failure(acquired.ErrorValue());
            }
            if (application && !operation) {
                auto started = application->Install();
                if (started.HasError())
                    return Result<void>::Failure(started.ErrorValue());
                operation = started.Value();
            }
            return Result<void>::Success();
        }

        /** @brief Waits only during explicit pre-window startup for the actual host's metadata/install jobs. */
        Result<std::shared_ptr<Application::PackageApplication>> Install(const std::filesystem::path &project) const {
            const auto deadline = clock.MonotonicNow() + Duration::FromMilliseconds(10'000);
            std::shared_ptr<Application::PackageApplication> application;
            std::optional<Application::PackageOperationId> operation;
            while (clock.MonotonicNow() < deadline) {
                if (auto current = Current(); current.HasError())
                    return Result<std::shared_ptr<Application::PackageApplication>>::Failure(current.ErrorValue());
                if (auto started = BeginInstall(project, application, operation); started.HasError())
                    return Result<std::shared_ptr<Application::PackageApplication>>::Failure(started.ErrorValue());
                if (auto pumped = packages->Pump(Packages::PackageActivationBoundary::Startup); pumped.HasError())
                    return Result<std::shared_ptr<Application::PackageApplication>>::Failure(pumped.ErrorValue());
                if (operation) {
                    const auto state = application->Query(*operation);
                    if (state.HasError())
                        return Result<std::shared_ptr<Application::PackageApplication>>::Failure(state.ErrorValue());
                    if (state.Value().outcome == Application::PackageOperationOutcome::Succeeded)
                        return Result<std::shared_ptr<Application::PackageApplication>>::Success(std::move(application));
                    if (state.Value().outcome != Application::PackageOperationOutcome::Running)
                        return Result<std::shared_ptr<Application::PackageApplication>>::Failure(
                            state.Value().diagnostic.value_or(BootstrapError("Offline XR package installation did not complete.")));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            return Result<std::shared_ptr<Application::PackageApplication>>::Failure(
                BootstrapError("Offline XR package startup exceeded its ten-second bound."));
        }

        /** @brief Composes ONE canonical package authority, then copies only verified installed physical declarations. */
        Result<void> Initialize(const std::filesystem::path &project, const std::filesystem::path &cache,
                                const Runtime::SceneObjectId trackingRoot) {
            if (auto prepared = PrepareNativeCacheParents(files, cache); prepared.HasError())
                return prepared;
            auto platform = Application::Internal::NativePackagePlatform();
            if (platform.HasError())
                return Result<void>::Failure(platform.ErrorValue());
            auto created =
                Application::Internal::PackageProjectHost::Create({jobs, files, clock, wall, executionTrust},
                                                                  {.projectRoot = project,
                                                                   .cacheRoot = cache,
                                                                   .platform = std::move(platform).Value(),
                                                                   .lifecycle = {.temporaryRoot = cache / "native",
                                                                                 .profile = Extensions::ExtensionHostProfile::Interactive,
                                                                                 .artifactGate = catalog.Policy().nativeGate},
                                                                   .mode = Packages::PackageRestoreMode::Offline,
                                                                   .requirePublisherVerification = true},
                                                                  Duration::FromMilliseconds(1'000), nullptr, &publishers);
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            packages = std::move(created).Value();
            if (auto installed = Install(project); installed.HasError())
                return Result<void>::Failure(installed.ErrorValue());
            auto selected =
                Application::Internal::SelectInstalledOpenXRProduct(*packages, project, packages->Generation(), &catalog.Policy(), {});
            if (selected.HasError())
                return Result<void>::Failure(selected.ErrorValue());
            auto bindings = Application::Internal::ReadInstalledOpenXRBindings(selected.Value().component, catalog.Bindings());
            if (bindings.HasError())
                return Result<void>::Failure(bindings.ErrorValue());
            services.emplace(EditorOpenXRHostServices{packages, catalog.Policy(), std::move(bindings).Value(), {}, trackingRoot, [this] {
                return Current();
            }, &jobs});
            auto &configuration = services->configuration.invocation;
            const auto put = [&](const char *key, SettingValue value) {
                configuration.try_emplace(SettingKey{key}, ConfigurationInputValue{std::move(value), std::nullopt});
            };
            put("xr.enabled", true);
            put("xr.schema_version", XR::XRProjectSettingsSchemaVersion);
            put("xr.capability_profile", static_cast<std::int64_t>(catalog.Profile()));
            put("xr.limits.views", static_cast<std::int64_t>(catalog.Limits().maximumViews));
            put("xr.limits.spaces", static_cast<std::int64_t>(catalog.Limits().maximumSpaces));
            put("xr.limits.actions", static_cast<std::int64_t>(catalog.Limits().maximumActions));
            put("xr.limits.devices", static_cast<std::int64_t>(catalog.Limits().maximumDevices));
            return Current();
        }
    };

    /** @copydoc EditorOpenXRBootstrap::Create */
    Result<std::unique_ptr<EditorOpenXRBootstrap>> EditorOpenXRBootstrap::Create(const std::span<char *> arguments,
                                                                                 const std::filesystem::path &projectRoot,
                                                                                 const std::filesystem::path &cacheRoot,
                                                                                 const Runtime::SceneObjectId trackingRoot) {
        using namespace Application::Internal;
        using Outcome = Result<std::unique_ptr<EditorOpenXRBootstrap>>;
        const auto parsed = ParseHostArguments(arguments);
        if (parsed.HasError())
            return Outcome::Failure(parsed.ErrorValue());
        if (!parsed.Value().requested)
            return Outcome::Success(nullptr);
        if (!trackingRoot.IsValid() || projectRoot.empty() || !projectRoot.is_absolute() || projectRoot != projectRoot.lexically_normal() ||
            cacheRoot.empty() || !cacheRoot.is_absolute() || cacheRoot != cacheRoot.lexically_normal())
            return Outcome::Failure(
                BootstrapError("Explicit XR installation requires the canonical project/cache and authored tracking-root selection."));
        NativeDurableFileSystem files;
        SystemWallClock wall;
        auto now = UnixSeconds(wall);
        if (now.HasError())
            return Outcome::Failure(now.ErrorValue());
        auto rootBytes = files.ReadFileBounded(*parsed.Value().paths[0], MaximumHostSignatureDocumentBytes);
        if (rootBytes.HasError())
            return Outcome::Failure(rootBytes.ErrorValue());
        auto policy =
            DecodePublisherTrustDocument(std::as_bytes(std::span{rootBytes.Value()}), Packages::PackagePublisherVerificationPolicy{});
        if (policy.HasError())
            return Outcome::Failure(policy.ErrorValue());
        auto roots = Roots(policy.Value(), now.Value());
        if (roots.HasError())
            return Outcome::Failure(roots.ErrorValue());
        auto provider = Security::CreateMbedTlsSignatureProvider();
        auto verifier = std::make_shared<Security::ArtifactVerifier>(provider, std::move(roots).Value());
        auto catalog = ReadSignedCatalog(files, parsed.Value(), verifier, cacheRoot, now.Value());
        if (catalog.HasError())
            return Outcome::Failure(catalog.ErrorValue());
        const auto rootExpiry = ActiveSignerExpiry(policy.Value(), catalog.Value(), now.Value());
        if (rootExpiry.HasError())
            return Outcome::Failure(rootExpiry.ErrorValue());
        auto publishers = Packages::PackagePublisherVerificationService::Create(std::move(policy).Value(), std::move(provider));
        if (publishers.HasError())
            return Outcome::Failure(publishers.ErrorValue());
        auto impl = std::make_unique<Impl>(std::move(publishers).Value(), std::move(catalog).Value(), now.Value(), rootExpiry.Value());
        if (auto initialized = impl->Initialize(projectRoot, cacheRoot, trackingRoot); initialized.HasError())
            return Outcome::Failure(initialized.ErrorValue());
        return Outcome::Success(std::unique_ptr<EditorOpenXRBootstrap>{new EditorOpenXRBootstrap{std::move(impl)}});
    }

    /** @copydoc EditorOpenXRBootstrap::EditorOpenXRBootstrap */
    EditorOpenXRBootstrap::EditorOpenXRBootstrap(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    /** @copydoc EditorOpenXRBootstrap::~EditorOpenXRBootstrap */
    EditorOpenXRBootstrap::~EditorOpenXRBootstrap() = default;

    /** @copydoc EditorOpenXRBootstrap::Services */
    const EditorOpenXRHostServices &EditorOpenXRBootstrap::Services() const noexcept {
        return *impl_->services;
    }
}  // namespace Horo::Editor
