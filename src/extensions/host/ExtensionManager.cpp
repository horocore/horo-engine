#include "Horo/Extensions/ExtensionManager.h"

#include "../capabilities/asset_pipeline_points/ExternalAssetImporter.h"
#include "EditorActivitySession.h"
#include "ExtensionAbiValidation.h"
#include "ExtensionActivationTransaction.h"
#include "ExtensionPlatformProviderCopy.h"
#include "Horo/Assets/AssetImporter.h"
#include "Horo/Extensions/ExtensionAbi.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ExtensionModuleResolution.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "Horo/Security/SecurityErrors.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace Horo::Extensions {
    namespace {
        namespace fs = std::filesystem;

        using Detail::IsValidBoundedText;
        using Detail::View;
        constexpr ExtensionManifestLimits kManifestLimits{};
        constexpr std::size_t kMaximumHostCapabilities = kManifestLimits.maximumContributions;
        constexpr std::uintmax_t kMaximumNativeArtifactBytes = 1024ULL * 1024ULL * 1024ULL;

        /** @brief Copies a provider claim while all ABI input borrows are live. */
        HoroExtensionStatus RegisterPlatformProvider(
            void *hostContext,  // NOSONAR(cpp:S5008) C11 callback ABI requires an opaque host context.
            const HoroPlatformServicesProviderDescriptor *descriptor) noexcept {
            auto *session = static_cast<AssetImporterRegistrationSession *>(hostContext);
            if (session == nullptr || session->failed)
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            if (!Detail::IsValidPlatformProviderDescriptor(descriptor) || !session->platformProviders.empty()) {
                session->failed = true;
                session->error = MakeError(ExtensionErrors::ContributionRejected, "Platform provider ABI descriptor is invalid.");
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            }
            if (!Detail::IsDeclaredPlatformProvider(*session, *descriptor)) {
                session->failed = true;
                session->error = MakeError(ExtensionErrors::ContributionRejected, "Platform provider is absent from its owning manifest.");
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            }
            try {
                session->platformProviders.push_back(Detail::CopyPlatformProviderCandidate(*session, *descriptor));
                return HORO_EXTENSION_SUCCESS;
            } catch (...) {
                session->failed = true;
                session->error = MakeError(ExtensionErrors::ContributionRejected, "Platform provider descriptor copy failed.");
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            }
        }

        [[nodiscard]] fs::path NativeLibraryEntryPath(const ExtensionManifest &manifest, const std::string_view selectedEntry) {
#if defined(_WIN32)
            constexpr std::string_view extension = ".dll";
#elif defined(__APPLE__)
            constexpr std::string_view extension = ".dylib";
#else
            constexpr std::string_view extension = ".so";
#endif
            fs::path entry = selectedEntry.empty() ? fs::path{manifest.id} : fs::path{selectedEntry};
            if (!entry.has_extension())
                entry += extension;
            return entry;
        }

        [[nodiscard]] bool IsContainedLibraryPath(const fs::path &packageRoot, const fs::path &libraryPath) {
            std::error_code ec;
            const fs::path root = fs::weakly_canonical(packageRoot, ec);
            if (ec)
                return false;
            const fs::path library = fs::weakly_canonical(libraryPath, ec);
            if (ec)
                return false;
            auto rootIt = root.begin();
            auto libraryIt = library.begin();
            for (; rootIt != root.end(); ++rootIt, ++libraryIt) {
                if (libraryIt == library.end() || *rootIt != *libraryIt)
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool HasSafeModuleEntry(const fs::path &packageRoot, const fs::path &entry) {
            if (entry.is_absolute())
                return false;
            fs::path current = packageRoot;
            std::error_code ec;
            for (const auto &component : entry) {
                if (component == "..")
                    return false;
                current /= component;
                if (fs::is_symlink(fs::symlink_status(current, ec)) || ec)
                    return false;
            }
            return true;
        }

        [[nodiscard]] Result<fs::path> ResolveModuleLibraryPath(const ExtensionManifest &manifest, const std::string_view selectedEntry) {
            const fs::path moduleEntry = NativeLibraryEntryPath(manifest, selectedEntry);
            const fs::path libraryPath = fs::path{manifest.rootPath} / moduleEntry;
            if (!HasSafeModuleEntry(manifest.rootPath, moduleEntry) || !IsContainedLibraryPath(manifest.rootPath, libraryPath))
                return Result<fs::path>::Failure(
                    MakeError(ExtensionErrors::InvalidManifest, "Native module entry must resolve inside its absolute package root."));
            return Result<fs::path>::Success(libraryPath);
        }

        [[nodiscard]] constexpr ExtensionHostPlatform CurrentHostPlatform() noexcept {
#if defined(_WIN32)
            return ExtensionHostPlatform::Windows;
#elif defined(__APPLE__)
            return ExtensionHostPlatform::MacOS;
#else
            return ExtensionHostPlatform::Linux;
#endif
        }

        [[nodiscard]] constexpr ExtensionHostArchitecture CurrentHostArchitecture() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
            return ExtensionHostArchitecture::Arm64;
#else
            return ExtensionHostArchitecture::X86_64;
#endif
        }

        [[nodiscard]] constexpr ExtensionBuildProfile CurrentBuildProfile() noexcept {
#if defined(NDEBUG)
            return ExtensionBuildProfile::Release;
#else
            return ExtensionBuildProfile::Debug;
#endif
        }

        [[nodiscard]] ExtensionHostEnvironment CurrentHostEnvironment(const ExtensionHostProfile profile,
                                                                      const std::span<const std::string_view> capabilities) noexcept {
            return {
                .profile = profile,
                .platform = CurrentHostPlatform(),
                .architecture = CurrentHostArchitecture(),
                .buildProfile = CurrentBuildProfile(),
                .engineVersion = "0.1.0",
                .abiMajor = HORO_EXTENSION_ABI_VERSION,
                .abiMinor = HORO_EXTENSION_ABI_MINOR_VERSION,
                .capabilities = capabilities,
            };
        }

        void CanonicalizeHostCapabilities(std::vector<std::string> &capabilities) {
            std::erase_if(capabilities, [](const std::string_view capability) {
                return capability.empty() || capability.size() > kManifestLimits.maximumIdentifierBytes;
            });
            std::ranges::sort(capabilities);
            capabilities.erase(std::ranges::unique(capabilities).begin(), capabilities.end());
            if (capabilities.size() > kMaximumHostCapabilities)
                capabilities.resize(kMaximumHostCapabilities);
        }

        /** @brief Reads one bounded manifest file from an absolute package root. */
        [[nodiscard]] Result<std::string> ReadManifestContent(const fs::path &requestedRoot) {
            if (!requestedRoot.is_absolute())
                return Result<std::string>::Failure(
                    MakeError(ExtensionErrors::InvalidManifest, "Extension package path must be absolute."));

            const fs::path manifestPath = requestedRoot / "extension.json";
            std::error_code fileError;
            if (const std::uintmax_t manifestBytes = fs::file_size(manifestPath, fileError);
                fileError || manifestBytes > kManifestLimits.maximumDocumentBytes) {
                return Result<std::string>::Failure(
                    MakeError(ExtensionErrors::InvalidManifest, "Extension manifest is unavailable or exceeds the bounded size."));
            }
            std::ifstream fileStream(manifestPath, std::ios::binary);
            if (!fileStream.is_open())
                return Result<std::string>::Failure(MakeError(ExtensionErrors::InvalidManifest, "Could not open extension.json"));

            std::stringstream buffer;
            buffer << fileStream.rdbuf();
            return Result<std::string>::Success(std::move(buffer).str());
        }

        /** @brief Parses a manifest and checks constraints imposed by the native loader. */
        template <typename LoadedMap>
        [[nodiscard]] Result<ExtensionManifest> ValidateLoadableManifest(const std::string &content, const fs::path &requestedRoot,
                                                                         const LoadedMap &loaded) {
            auto parseResult = ParseExtensionManifest(content, kManifestLimits);
            if (parseResult.HasError())
                return Result<ExtensionManifest>::Failure(parseResult.ErrorValue());

            ExtensionManifest manifest = std::move(parseResult).Value();
            manifest.rootPath = fs::weakly_canonical(requestedRoot).string();
            if (loaded.contains(manifest.id))
                return Result<ExtensionManifest>::Failure(
                    MakeError(ExtensionErrors::LoadFailed, "An extension with this package ID is already loaded."));
            return Result<ExtensionManifest>::Success(std::move(manifest));
        }

        template <typename LoadedMap>
        [[nodiscard]] Result<ExtensionManifest> ReadAndValidateManifest(const fs::path &requestedRoot, const LoadedMap &loaded) {
            auto content = ReadManifestContent(requestedRoot);
            if (content.HasError())
                return Result<ExtensionManifest>::Failure(content.ErrorValue());
            return ValidateLoadableManifest(content.Value(), requestedRoot, loaded);
        }

        /** @brief Invokes an extension entry point while containing exceptions at the ABI boundary. */
        template <typename LoadFunction>
        [[nodiscard]] HoroExtensionStatus InvokeExtensionLoad(const LoadFunction loadFunc, const HoroExtensionHostApi &hostApi,
                                                              HoroExtensionModuleApi &moduleApi, const std::string &extensionId) {
            try {
                return loadFunc(&hostApi, &moduleApi);
            } catch (const std::runtime_error &exception) {  // NOSONAR(cpp:S1181)
                LOG_ERROR("extensions", "Extension %s threw runtime error during load: %s", extensionId.c_str(), exception.what());
            } catch (const std::logic_error &exception) {  // NOSONAR(cpp:S1181)
                LOG_ERROR("extensions", "Extension %s threw logic error during load: %s", extensionId.c_str(), exception.what());
            } catch (const std::bad_alloc &exception) {  // NOSONAR(cpp:S1181)
                LOG_ERROR("extensions", "Extension %s threw bad alloc during load: %s", extensionId.c_str(), exception.what());
            } catch (const std::exception &exception) {  // NOSONAR(cpp:S1181)
                LOG_ERROR("extensions", "Extension %s threw during load: %s", extensionId.c_str(), exception.what());
            } catch (...) {  // NOSONAR(cpp:S1181)
                LOG_ERROR("extensions", "Extension %s threw unknown exception during load.", extensionId.c_str());
            }
            return HORO_EXTENSION_ERROR_INIT_FAILED;
        }

        /** @brief Checks the loaded module identity against the validated manifest declaration. */
        [[nodiscard]] bool MatchesDeclaredModule(const HoroExtensionModuleApi &moduleApi, const std::string_view declaredId,
                                                 const std::string_view declaredVersion) noexcept {
            return IsValidBoundedText(moduleApi.moduleId) && IsValidBoundedText(moduleApi.moduleVersion) &&
                   (View(moduleApi.moduleId).empty() || View(moduleApi.moduleId) == declaredId) &&
                   (View(moduleApi.moduleVersion).empty() || View(moduleApi.moduleVersion) == declaredVersion);
        }

        /** @brief Owns a validated native module and its staged importer contributions. */
        struct ActivatedModule {
            std::shared_ptr<ExtensionModuleLifetime> lifetime;
            std::vector<Assets::AssetImporterContribution> contributions;
            std::vector<ExtensionPlatformProviderCandidate> platformProviders;
        };

        /** @brief Rolls back rejected native activation through the same reverse-order ownership transaction. */
        [[nodiscard]] Result<ActivatedModule> RejectActivatedModule(const std::shared_ptr<ExtensionModuleLifetime> &lifetime,
                                                                    const HoroExtensionModuleApi &moduleApi,
                                                                    AssetImporterRegistrationSession &registration, Error error) {
            lifetime->moduleApi = moduleApi;
            lifetime->loaded = true;
            ExtensionActivationTransaction rollback;
            rollback.Stage(lifetime, std::move(registration.contributions));
            return Result<ActivatedModule>::Failure(rollback.Rollback(std::move(error)));
        }

        /** @brief Constructs the explicit ABI callback table without invoking or publishing any contribution. */
        [[nodiscard]] HoroExtensionHostApi MakeHostApi(AssetImporterRegistrationSession &registration, const bool allowPlatformProvider) {
            constexpr std::string_view engineVersion = "0.1.0";
            return {.structSize = sizeof(HoroExtensionHostApi),
                    .abiVersion = HORO_EXTENSION_ABI_VERSION,
                    .engineVersion = {engineVersion.data(), static_cast<std::uint32_t>(engineVersion.size())},
                    .hostContext = &registration,
                    .registerAssetImporter = RegisterExternalAssetImporter,
                    .abiMinorVersion = HORO_EXTENSION_ABI_MINOR_VERSION,
                    .registerPlatformServicesProvider = allowPlatformProvider ? &RegisterPlatformProvider : nullptr,
                    .registerEditorActivity = registration.editorHost ? &RegisterExternalEditorActivity : nullptr};
        }

        /** @brief Validates the returned module table and exact activity/drawer declarations before activation publication. */
        [[nodiscard]] Result<void> ValidateLoadedModule(HoroExtensionModuleApi &moduleApi, const ExtensionManifest &manifest,
                                                        const ExtensionModuleManifest &manifestModule,
                                                        const std::size_t registeredActivities) {
            if (!NormalizeModuleApi(moduleApi) || !MatchesDeclaredModule(moduleApi, manifestModule.id, manifestModule.version))
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::InvalidManifest, "Loaded module table or identity/version is invalid."));
            const auto declaredActivities = std::ranges::count_if(manifest.contributions, [&manifestModule](const auto &claim) {
                return claim.owningModule == manifestModule.id && claim.type == "editor.activity_item";
            });
            if (const auto declaredPanels = std::ranges::count_if(manifest.contributions,
                                                                  [&manifestModule](const auto &claim) {
                return claim.owningModule == manifestModule.id && claim.type == "editor.panel";
            });
                static_cast<std::size_t>(declaredActivities) != registeredActivities || declaredPanels != declaredActivities)
                return Result<void>::Failure(MakeError(ExtensionErrors::ContributionRejected,
                                                       "Native module did not register every declared activity/drawer pair."));
            return Result<void>::Success();
        }

        /** @brief Loads and validates one native module while retaining rollback ownership. */
        [[nodiscard]] Result<ActivatedModule> ActivateModule(const std::shared_ptr<Platform::DynamicLibrary> &library,
                                                             const ExtensionManifest &manifest,
                                                             const ExtensionModuleManifest &manifestModule,
                                                             const bool allowPlatformProvider,
                                                             const std::shared_ptr<ExtensionRetirement> &retirement,
                                                             const std::vector<std::shared_ptr<ExtensionModuleLifetime>> &dependencies,
                                                             const std::shared_ptr<EditorActivityHost> &editorHost) {
            const auto loadFunc = reinterpret_cast<HoroExtensionLoadFunc>(library->GetSymbol("horo_extension_load"));  // NOSONAR(cpp:S3630)
            if (loadFunc == nullptr)
                return Result<ActivatedModule>::Failure(
                    MakeError(ExtensionErrors::MissingEntryPoint, "Symbol horo_extension_load not found"));

            auto lifetime = std::make_shared<ExtensionModuleLifetime>();
            lifetime->code->library = library;
            lifetime->code->dependencies = dependencies;
            lifetime->moduleId = manifestModule.id;
            lifetime->retirement = retirement;
            if (!retirement->BindModuleCode(manifestModule.id, lifetime))
                return Result<ActivatedModule>::Failure(MakeError(ExtensionErrors::LoadFailed, "Module retirement identity is invalid."));
            lifetime->unload =
                reinterpret_cast<HoroExtensionUnloadFunc>(library->GetSymbol("horo_extension_unload"));  // NOSONAR(cpp:S3630)
            AssetImporterRegistrationSession registration{
                .manifest = &manifest,
                .extensionModule = &manifestModule,
                .lifetime = lifetime,
                .retirement = retirement,
                .editorHost = editorHost,
            };
            HoroExtensionHostApi hostApi = MakeHostApi(registration, allowPlatformProvider);
            if (const auto query =
                    reinterpret_cast<HoroExtensionQueryFunc>(library->GetSymbol("horo_extension_query"));  // NOSONAR(cpp:S3630)
                NegotiateModuleAbi(query, hostApi) != HORO_EXTENSION_SUCCESS)
                return Result<ActivatedModule>::Failure(
                    MakeError(ExtensionErrors::LoadFailed, "Native module ABI requirements are incompatible with the host."));
            HoroExtensionModuleApi moduleApi{.structSize = sizeof(HoroExtensionModuleApi)};
            if (const HoroExtensionStatus status = InvokeExtensionLoad(loadFunc, hostApi, moduleApi, manifest.id);
                status != HORO_EXTENSION_SUCCESS || registration.failed) {
                Error error = registration.failed ? std::move(registration.error)
                                                  : MakeError(ExtensionErrors::LoadFailed, "Extension load function returned an error.");
                return RejectActivatedModule(lifetime, moduleApi, registration, std::move(error));
            }
            if (auto validated = ValidateLoadedModule(moduleApi, manifest, manifestModule, lifetime->editorActivities.size());
                validated.HasError())
                return RejectActivatedModule(lifetime, moduleApi, registration, std::move(validated).ErrorValue());
            lifetime->moduleApi = moduleApi;
            lifetime->loaded = true;
            return Result<ActivatedModule>::Success({.lifetime = std::move(lifetime),
                                                     .contributions = std::move(registration.contributions),
                                                     .platformProviders = std::move(registration.platformProviders)});
        }

        /** @brief Atomically commits staged importer contributions to the host catalog. */
        [[nodiscard]] Result<void> CommitContributions(std::vector<Assets::AssetImporterContribution> &contributions,
                                                       Assets::AssetImporterCatalog *importerCatalog) {
            if (contributions.empty())
                return Result<void>::Success();
            if (importerCatalog == nullptr)
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::ContributionRejected, "The host did not provide an asset importer catalog."));
            if (auto validation = importerCatalog->ValidateBatch(contributions); validation.HasError())
                return Result<void>::Failure(WrapError(ExtensionErrors::ContributionRejected, validation.ErrorValue(),
                                                       "The complete extension contribution batch conflicted with the host catalog."));
            if (auto registered = importerCatalog->RegisterBatch(std::move(contributions)); registered.HasError()) {
                return Result<void>::Failure(WrapError(ExtensionErrors::ContributionRejected, registered.ErrorValue(),
                                                       "The complete extension contribution batch could not be committed."));
            }
            return Result<void>::Success();
        }

        /**
         * @brief Produces evidence for the selected library and confirms the bytes are still current immediately before loading.
         * @param gate Mandatory host-composed artifact gate.
         * @param libraryPath Exact selected native library path.
         * @return Verified evidence or a typed fail-closed security error.
         */
        [[nodiscard]] Result<Security::VerifiedArtifactEvidence> VerifyCurrentArtifact(
            const std::shared_ptr<const Security::NativeArtifactGate> &gate, const fs::path &libraryPath) {
            if (!gate)
                return Result<Security::VerifiedArtifactEvidence>::Failure(
                    MakeError(SecurityErrors::MissingEvidence, "Native extension activation has no security gate."));
            auto evidence = gate->Verify(libraryPath);
            if (evidence.HasError())
                return evidence;
            std::error_code artifactSizeError;
            const std::uintmax_t artifactSize = fs::file_size(libraryPath, artifactSizeError);
            if (artifactSizeError || artifactSize > kMaximumNativeArtifactBytes)
                return Result<Security::VerifiedArtifactEvidence>::Failure(MakeError(SecurityErrors::StaleEvidence));
            std::ifstream verifiedFile{libraryPath, std::ios::binary};
            std::vector<char> verifiedBytes(static_cast<std::size_t>(artifactSize));
            verifiedFile.read(verifiedBytes.data(), static_cast<std::streamsize>(verifiedBytes.size()));
            if (!verifiedFile || static_cast<std::size_t>(verifiedFile.gcount()) != verifiedBytes.size() ||
                verifiedFile.peek() != std::char_traits<char>::eof() ||
                ComputeSha256(std::as_bytes(std::span{verifiedBytes})) != evidence.Value().ArtifactDigest())
                return Result<Security::VerifiedArtifactEvidence>::Failure(MakeError(SecurityErrors::StaleEvidence));
            return evidence;
        }

        /** @brief Explicit load-time authorities shared by dependency-ordered native activation. */
        struct NativeActivationInputs final {
            const ExtensionManifest &manifest;
            const ExtensionModulePlan &plan;
            const std::shared_ptr<ExtensionRetirement> &retirement;
            const ExtensionManager::NativeLibraryLoader &loader;
            const std::shared_ptr<const Security::NativeArtifactGate> &artifactGate;
            std::size_t platformDeclarations;
            std::shared_ptr<EditorActivityHost> editorHost;
        };

        /** @brief Stages the exact declared native contributions without publishing or relinquishing rollback ownership. */
        [[nodiscard]] Result<std::vector<ExtensionPlatformProviderCandidate>> StageDeclaredNativeModules(
            const NativeActivationInputs &inputs, std::vector<std::shared_ptr<ExtensionModuleLifetime>> &dependencies,
            ExtensionActivationTransaction &transaction) {
            std::vector<ExtensionPlatformProviderCandidate> candidates;
            for (std::size_t index = 0; index < inputs.plan.moduleIds.size(); ++index) {
                const auto &moduleId = inputs.plan.moduleIds[index];
                const auto foundModule = std::ranges::find(inputs.manifest.modules, moduleId, &ExtensionModuleManifest::id);
                if (foundModule == inputs.manifest.modules.end())
                    return Result<std::vector<ExtensionPlatformProviderCandidate>>::Failure(
                        MakeError(ExtensionErrors::ModuleResolutionFailed, "Resolved module is absent from the package manifest."));
                auto path = ResolveModuleLibraryPath(inputs.manifest, inputs.plan.selectedEntries[index]);
                if (path.HasError())
                    return Result<std::vector<ExtensionPlatformProviderCandidate>>::Failure(path.ErrorValue());
                if (auto evidence = VerifyCurrentArtifact(inputs.artifactGate, path.Value()); evidence.HasError())
                    return Result<std::vector<ExtensionPlatformProviderCandidate>>::Failure(evidence.ErrorValue());
                auto loaded = inputs.loader(path.Value().string());
                if (loaded.HasError())
                    return Result<std::vector<ExtensionPlatformProviderCandidate>>::Failure(loaded.ErrorValue());
                std::shared_ptr<Platform::DynamicLibrary> library{std::move(loaded).Value()};
                const bool provider = inputs.platformDeclarations != 0 && inputs.manifest.contributions.front().owningModule == moduleId;
                auto activated =
                    ActivateModule(library, inputs.manifest, *foundModule, provider, inputs.retirement, dependencies, inputs.editorHost);
                if (activated.HasError())
                    return Result<std::vector<ExtensionPlatformProviderCandidate>>::Failure(activated.ErrorValue());
                ActivatedModule native = std::move(activated).Value();
                dependencies.push_back(native.lifetime);
                candidates.insert(candidates.end(), std::make_move_iterator(native.platformProviders.begin()),
                                  std::make_move_iterator(native.platformProviders.end()));
                transaction.Stage(std::move(native.lifetime), std::move(native.contributions));
            }
            if (candidates.size() != inputs.platformDeclarations)
                return Result<std::vector<ExtensionPlatformProviderCandidate>>::Failure(
                    MakeError(ExtensionErrors::ContributionRejected, "Provider module did not register its exact declared contribution."));
            return Result<std::vector<ExtensionPlatformProviderCandidate>>::Success(std::move(candidates));
        }

        /** @brief Allocates manager bookkeeping before any contribution becomes externally visible. */
        [[nodiscard]] auto PrepareLoadedExtension(ExtensionManifest manifest, ExtensionModulePlan plan,
                                                  std::shared_ptr<ExtensionRetirement> retirement,
                                                  const std::span<const std::string> providerExtensions) {
            auto loadedExtension = std::make_unique<LoadedExtension>();
            loadedExtension->manifest = std::move(manifest);
            loadedExtension->retirement = std::move(retirement);
            loadedExtension->providerExtensions.assign(providerExtensions.begin(), providerExtensions.end());
            loadedExtension->moduleIds = std::move(plan.moduleIds);
            const std::string extensionId = loadedExtension->manifest.id;
            TransparentStringMap<std::unique_ptr<LoadedExtension>> candidate;
            candidate.try_emplace(extensionId, std::move(loadedExtension));
            return candidate.extract(extensionId);
        }

        /** @brief Validates the atomic provider-only ABI profile before any native module is loaded. */
        [[nodiscard]] Result<std::size_t> ValidateProviderPackage(const ExtensionManifest &manifest,
                                                                  const ExtensionPlatformProviderCommit &providerCommit,
                                                                  const std::vector<std::string> &hostCapabilities) {
            const auto declarations = std::ranges::count_if(manifest.contributions, [](const auto &contribution) {
                return contribution.type == "platform.services.provider";
            });
            if (declarations > 1 || (declarations != 0 && manifest.contributions.size() != 1))
                return Result<std::size_t>::Failure(
                    MakeError(ExtensionErrors::ContributionRejected,
                              "Platform provider packages must publish exactly one provider-only contribution."));
            if (declarations == 0)
                return Result<std::size_t>::Success(0);
            if (providerCommit == nullptr || !std::ranges::binary_search(hostCapabilities, std::string{"platform.services.provider"}))
                return Result<std::size_t>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable,
                                                              "Host composition has not admitted platform provider contributions."));
            const auto &claim = manifest.contributions.front();
            if (const auto owningModule = std::ranges::find(manifest.modules, claim.owningModule, &ExtensionModuleManifest::id);
                owningModule == manifest.modules.end() || !owningModule->imports.empty() ||
                std::ranges::find(owningModule->requiredCapabilities, "platform.services.provider") ==
                    owningModule->requiredCapabilities.end())
                return Result<std::size_t>::Failure(
                    MakeError(ExtensionErrors::ContributionRejected,
                              "Provider module must require its host capability and cannot import services in ABI profile 1."));
            return Result<std::size_t>::Success(static_cast<std::size_t>(declarations));
        }

        /** @brief Keeps arbitrary host-composition callback failures inside the manager's result contract. */
        template <typename Commit>
        [[nodiscard]] Result<ExtensionPlatformProviderPublication> CommitPlatformProvider(ExtensionPlatformProviderCandidate candidate,
                                                                                          const Commit &commit) {
            try {
                return commit(std::move(candidate));
            } catch (...) {  // NOSONAR(cpp:S2738) Host-composition callback may throw any exception type.
                return Result<ExtensionPlatformProviderPublication>::Failure(
                    MakeError(ExtensionErrors::ContributionRejected, "Platform provider host commit threw an exception."));
            }
        }

        /** @brief Publishes the validated optional platform claim and requires an explicit revocation owner. */
        [[nodiscard]] Result<ExtensionPlatformProviderPublication> PublishPlatformClaim(
            std::vector<ExtensionPlatformProviderCandidate> candidates, const ExtensionPlatformProviderCommit &commit) {
            if (candidates.empty())
                return Result<ExtensionPlatformProviderPublication>::Success(ExtensionPlatformProviderPublication{});
            auto published = CommitPlatformProvider(std::move(candidates.front()), commit);
            if (published.HasError())
                return published;
            if (!published.Value())
                return Result<ExtensionPlatformProviderPublication>::Failure(
                    MakeError(ExtensionErrors::ContributionRejected, "Provider commit returned no revocation owner."));
            return published;
        }

        /** @brief Keeps capability-view borrows within synchronous module graph resolution. */
        [[nodiscard]] Result<ExtensionModulePlan> ResolveForHost(const ExtensionManifest &manifest, const ExtensionHostProfile profile,
                                                                 const std::vector<std::string> &capabilities) {
            std::vector<std::string_view> views;
            views.reserve(capabilities.size());
            std::ranges::transform(capabilities, std::back_inserter(views), [](const auto &capability) {
                return std::string_view{capability};
            });
            return ResolveExtensionModules(manifest, CurrentHostEnvironment(profile, views));
        }

        /** @brief Publishes admitted editor and importer contributions within the single activation rollback authority. */
        [[nodiscard]] Result<void> CommitHostContributions(ExtensionActivationTransaction &transaction,
                                                           Assets::AssetImporterCatalog *catalog) {
            if (auto editor = CommitEditorActivities(transaction.Lifetimes()); editor.HasError())
                return editor;
            return CommitContributions(transaction.Contributions(), catalog);
        }
    }  // namespace

    /** @copydoc ExtensionManager::ExtensionManager */
    ExtensionManager::ExtensionManager(Assets::AssetImporterCatalog *importerCatalog, const ExtensionHostProfile hostProfile,
                                       std::vector<std::string> hostCapabilities,
                                       std::shared_ptr<const Security::NativeArtifactGate> artifactGate, NativeLibraryLoader libraryLoader,
                                       ExtensionPlatformProviderCommit platformProviderCommit,
                                       std::shared_ptr<EditorActivityHost> editorActivityHost)
        : m_importerCatalog(importerCatalog), m_hostProfile(hostProfile), m_hostCapabilities(std::move(hostCapabilities)),
          m_artifactGate(std::move(artifactGate)), m_libraryLoader(std::move(libraryLoader)),
          m_platformProviderCommit(std::move(platformProviderCommit)), m_editorActivityHost(std::move(editorActivityHost)) {
        CanonicalizeHostCapabilities(m_hostCapabilities);
        if (m_hostProfile != ExtensionHostProfile::Interactive ||
            !std::ranges::binary_search(m_hostCapabilities, std::string{HORO_EDITOR_ACTIVITY_HOST_CAPABILITY}))
            m_editorActivityHost.reset();
        if (!m_editorActivityHost)
            std::erase(m_hostCapabilities, std::string{HORO_EDITOR_ACTIVITY_HOST_CAPABILITY});
        if (!m_libraryLoader)
            m_libraryLoader = [](const std::string &path) {
                return Platform::LoadDynamicLibrary(path);
            };
    }

    Result<std::string> ExtensionManager::LoadExtension(const std::string &extensionDir,
                                                        const std::span<const std::string> providerExtensions) {
        FinalizeRetirements();
        if (!HasActivationCapacity(providerExtensions.size()))
            return Result<std::string>::Failure(MakeError(ExtensionErrors::LoadFailed, "Extension activation retention bound exceeded."));
        std::vector<std::shared_ptr<ExtensionModuleLifetime>> dependencies;
        auto manifestResult = ReadAndValidateManifest(fs::path{extensionDir}, m_loadedExtensions);
        if (manifestResult.HasError())
            return Result<std::string>::Failure(manifestResult.ErrorValue());

        ExtensionManifest manifest = std::move(manifestResult).Value();
        std::vector<std::shared_ptr<ExtensionExecutableLease>> providerLeases;
        if (auto providers = AcquireProviderDependencies(providerExtensions, manifest.id, dependencies, providerLeases);
            providers.HasError())
            return Result<std::string>::Failure(providers.ErrorValue());
        auto planResult = ResolveForHost(manifest, m_hostProfile, m_hostCapabilities);
        if (planResult.HasError())
            return Result<std::string>::Failure(planResult.ErrorValue());
        ExtensionModulePlan plan = std::move(planResult).Value();
        auto retirement = std::make_shared<ExtensionRetirement>(manifest.id, plan.moduleIds);

        auto declarationsResult = ValidateProviderPackage(manifest, m_platformProviderCommit, m_hostCapabilities);
        if (declarationsResult.HasError())
            return Result<std::string>::Failure(declarationsResult.ErrorValue());
        const auto platformDeclarations = std::move(declarationsResult).Value();

        ExtensionActivationTransaction transaction;
        auto staged = StageDeclaredNativeModules({manifest, plan, retirement, m_libraryLoader, m_artifactGate, platformDeclarations,
                                                  m_editorActivityHost},
                                                 dependencies, transaction);
        if (staged.HasError())
            return Result<std::string>::Failure(transaction.Rollback(staged.ErrorValue()));
        auto platformCandidates = std::move(staged).Value();
        std::string extensionId = manifest.id;
        std::string activationId = extensionId;
        auto record = PrepareLoadedExtension(std::move(manifest), std::move(plan), std::move(retirement), providerExtensions);
        record.mapped()->providerLeases = std::move(providerLeases);
        m_activationOrder.reserve(m_activationOrder.size() + 1);
        m_loadedExtensions.reserve(m_loadedExtensions.size() + 1);
        auto platformPublication = PublishPlatformClaim(std::move(platformCandidates), m_platformProviderCommit);
        if (platformPublication.HasError())
            return Result<std::string>::Failure(transaction.Rollback(platformPublication.ErrorValue()));
        if (auto committed = CommitHostContributions(transaction, m_importerCatalog); committed.HasError())
            return Result<std::string>::Failure(transaction.Rollback(committed.ErrorValue()));

        // Preallocated node/vector storage and noexcept transparent hashing make this ownership
        // transfer allocation-free. No published native owner can be stranded by bookkeeping allocation.
        record.mapped()->lifetimes = transaction.ReleaseLifetimes();
        record.mapped()->platformProvider = std::move(platformPublication).Value();
        m_loadedExtensions.insert(std::move(record));
        m_activationOrder.push_back(std::move(activationId));
        LOG_INFO("extensions", "Successfully loaded extension: %s", extensionId.c_str());
        return Result<std::string>::Success(std::move(extensionId));
    }

}  // namespace Horo::Extensions
