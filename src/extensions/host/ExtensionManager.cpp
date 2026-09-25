#include "Horo/Extensions/ExtensionManager.h"

#include "../capabilities/asset_pipeline_points/ExternalAssetImporter.h"
#include "ExtensionAbiValidation.h"
#include "ExtensionActivationTransaction.h"
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

        constexpr std::uint32_t kMaximumModuleIdentityBytes = 256;
        constexpr ExtensionManifestLimits kManifestLimits{};
        constexpr std::size_t kMaximumHostCapabilities = kManifestLimits.maximumContributions;
        constexpr std::uintmax_t kMaximumNativeArtifactBytes = 1024ULL * 1024ULL * 1024ULL;

        [[nodiscard]] std::string_view View(const HoroExtensionStringView value) noexcept {
            return value.data != nullptr ? std::string_view{value.data, value.length} : std::string_view{};
        }

        [[nodiscard]] bool IsValidBoundedText(const HoroExtensionStringView value) noexcept {
            return (value.data != nullptr || value.length == 0) && value.length <= kMaximumModuleIdentityBytes;
        }

        /** @brief Copies a provider claim while all ABI input borrows are live. */
        HoroExtensionStatus RegisterPlatformProvider(void *hostContext, const HoroPlatformServicesProviderDescriptor *descriptor) noexcept {
            auto *session = static_cast<AssetImporterRegistrationSession *>(hostContext);
            if (session == nullptr || session->failed)
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            constexpr std::size_t MaximumPermissions = 32;
            if (descriptor == nullptr || descriptor->structSize != sizeof(HoroPlatformServicesProviderDescriptor) ||
                descriptor->abiVersion != HORO_PLATFORM_SERVICES_PROVIDER_ABI_VERSION || !IsValidBoundedText(descriptor->providerKey) ||
                descriptor->providerKey.length == 0 || descriptor->permissionCount > MaximumPermissions ||
                (descriptor->permissionCount != 0 && descriptor->permissions == nullptr) || descriptor->createCandidate == nullptr ||
                descriptor->retireCandidate == nullptr || descriptor->destroyCandidate == nullptr ||
                session->platformProviders.size() != 0) {
                session->failed = true;
                session->error = MakeError(ExtensionErrors::ContributionRejected, "Platform provider ABI descriptor is invalid.");
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            }
            const auto declared = std::ranges::find_if(session->manifest->contributions, [&](const auto &contribution) {
                return contribution.type == "platform.services.provider" && contribution.owningModule == session->extensionModule->id &&
                       contribution.id == View(descriptor->providerKey);
            });
            if (declared == session->manifest->contributions.end()) {
                session->failed = true;
                session->error = MakeError(ExtensionErrors::ContributionRejected, "Platform provider is absent from its owning manifest.");
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            }
            try {
                ExtensionPlatformProviderCandidate candidate;
                candidate.extensionId = session->manifest->id;
                candidate.moduleId = session->extensionModule->id;
                candidate.providerKey = View(descriptor->providerKey);
                candidate.providerId = descriptor->providerId;
                candidate.platformMask = descriptor->platformMask;
                candidate.profileMask = descriptor->profileMask;
                candidate.serviceMask = descriptor->serviceMask;
                candidate.interfaceMajor = descriptor->interfaceMajor;
                candidate.interfaceMinor = descriptor->interfaceMinor;
                candidate.contractMajor = descriptor->contractMajor;
                candidate.contractMinor = descriptor->contractMinor;
                candidate.contractPatch = descriptor->contractPatch;
                for (std::uint32_t index = 0; index < descriptor->permissionCount; ++index) {
                    if (!IsValidBoundedText(descriptor->permissions[index]) || descriptor->permissions[index].length == 0)
                        throw std::invalid_argument("Invalid provider permission");
                    candidate.permissions.emplace_back(View(descriptor->permissions[index]));
                }
                candidate.factoryContext = descriptor->factoryContext;
                candidate.createCandidate = descriptor->createCandidate;
                candidate.retireCandidate = descriptor->retireCandidate;
                candidate.destroyCandidate = descriptor->destroyCandidate;
                candidate.moduleCodeLease = session->lifetime;
                session->platformProviders.push_back(std::move(candidate));
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

        /** @brief Loads and validates one native module while retaining rollback ownership. */
        [[nodiscard]] Result<ActivatedModule> ActivateModule(const std::shared_ptr<Platform::DynamicLibrary> &library,
                                                             const ExtensionManifest &manifest,
                                                             const ExtensionModuleManifest &manifestModule,
                                                             const bool allowPlatformProvider) {
            const auto loadFunc = reinterpret_cast<HoroExtensionLoadFunc>(library->GetSymbol("horo_extension_load"));  // NOSONAR(cpp:S3630)
            if (loadFunc == nullptr)
                return Result<ActivatedModule>::Failure(
                    MakeError(ExtensionErrors::MissingEntryPoint, "Symbol horo_extension_load not found"));

            auto lifetime = std::make_shared<ExtensionModuleLifetime>();
            lifetime->library = library;
            lifetime->moduleId = manifestModule.id;
            lifetime->unload =
                reinterpret_cast<HoroExtensionUnloadFunc>(library->GetSymbol("horo_extension_unload"));  // NOSONAR(cpp:S3630)
            AssetImporterRegistrationSession registration{
                .manifest = &manifest,
                .extensionModule = &manifestModule,
                .lifetime = lifetime,
            };
            constexpr std::string_view engineVersion = "0.1.0";
            HoroExtensionHostApi hostApi{
                .structSize = sizeof(HoroExtensionHostApi),
                .abiVersion = HORO_EXTENSION_ABI_VERSION,
                .engineVersion = {engineVersion.data(), static_cast<std::uint32_t>(engineVersion.size())},
                .hostContext = &registration,
                .registerAssetImporter = RegisterExternalAssetImporter,
                .abiMinorVersion = HORO_EXTENSION_ABI_MINOR_VERSION,
                .registerPlatformServicesProvider = allowPlatformProvider ? RegisterPlatformProvider : nullptr,
            };
            if (const auto query =
                    reinterpret_cast<HoroExtensionQueryFunc>(library->GetSymbol("horo_extension_query"));  // NOSONAR(cpp:S3630)
                NegotiateModuleAbi(query, hostApi) != HORO_EXTENSION_SUCCESS)
                return Result<ActivatedModule>::Failure(
                    MakeError(ExtensionErrors::LoadFailed, "Native module ABI requirements are incompatible with the host."));
            HoroExtensionModuleApi moduleApi{.structSize = sizeof(HoroExtensionModuleApi)};
            if (const HoroExtensionStatus status = InvokeExtensionLoad(loadFunc, hostApi, moduleApi, manifest.id);
                status != HORO_EXTENSION_SUCCESS || registration.failed) {
                lifetime->moduleApi = moduleApi;
                lifetime->loaded = true;
                ExtensionActivationTransaction rollback;
                rollback.Stage(lifetime, std::move(registration.contributions));
                Error error = registration.failed ? std::move(registration.error)
                                                  : MakeError(ExtensionErrors::LoadFailed, "Extension load function returned an error.");
                return Result<ActivatedModule>::Failure(rollback.Rollback(std::move(error)));
            }
            if (!NormalizeModuleApi(moduleApi) || !MatchesDeclaredModule(moduleApi, manifestModule.id, manifestModule.version)) {
                lifetime->moduleApi = moduleApi;
                lifetime->loaded = true;
                ExtensionActivationTransaction rollback;
                rollback.Stage(lifetime, std::move(registration.contributions));
                return Result<ActivatedModule>::Failure(
                    rollback.Rollback(MakeError(ExtensionErrors::InvalidManifest, "Loaded module table or identity/version is invalid.")));
            }
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

        /** @brief Publishes one fully activated extension into manager-owned lifetime storage. */
        [[nodiscard]] std::string CommitLoadedExtension(ExtensionManifest manifest, ExtensionModulePlan plan,
                                                        ExtensionActivationTransaction &transaction,
                                                        ExtensionPlatformProviderPublication platformProvider,
                                                        TransparentStringMap<std::unique_ptr<LoadedExtension>> &loadedExtensions) {
            auto loadedExtension = std::make_unique<LoadedExtension>();
            loadedExtension->manifest = std::move(manifest);
            loadedExtension->lifetimes = transaction.ReleaseLifetimes();
            loadedExtension->moduleIds = std::move(plan.moduleIds);
            loadedExtension->platformProvider = std::move(platformProvider);
            const std::string extensionId = loadedExtension->manifest.id;
            loadedExtensions.try_emplace(extensionId, std::move(loadedExtension));
            return extensionId;
        }

    }  // namespace

    /** @copydoc ExtensionManager::ExtensionManager */
    ExtensionManager::ExtensionManager(Assets::AssetImporterCatalog *importerCatalog, const ExtensionHostProfile hostProfile,
                                       std::vector<std::string> hostCapabilities,
                                       std::shared_ptr<const Security::NativeArtifactGate> artifactGate, NativeLibraryLoader libraryLoader,
                                       ExtensionPlatformProviderCommit platformProviderCommit)
        : m_importerCatalog(importerCatalog), m_hostProfile(hostProfile), m_hostCapabilities(std::move(hostCapabilities)),
          m_artifactGate(std::move(artifactGate)), m_libraryLoader(std::move(libraryLoader)),
          m_platformProviderCommit(std::move(platformProviderCommit)) {
        CanonicalizeHostCapabilities(m_hostCapabilities);
        if (!m_libraryLoader)
            m_libraryLoader = [](const std::string &path) {
                return Platform::LoadDynamicLibrary(path);
            };
    }

    ExtensionManager::~ExtensionManager() {
        UnloadAll();
    }

    ExtensionManager::ExtensionManager(ExtensionManager &&) noexcept = default;
    ExtensionManager &ExtensionManager::operator=(ExtensionManager &&) noexcept = default;

    Result<std::string> ExtensionManager::LoadExtension(const std::string &extensionDir) {
        auto manifestResult = ReadAndValidateManifest(fs::path{extensionDir}, m_loadedExtensions);
        if (manifestResult.HasError())
            return Result<std::string>::Failure(manifestResult.ErrorValue());

        ExtensionManifest manifest = std::move(manifestResult).Value();
        std::vector<std::string_view> capabilityViews;
        capabilityViews.reserve(m_hostCapabilities.size());
        std::ranges::transform(m_hostCapabilities, std::back_inserter(capabilityViews), [](const std::string &capability) {
            return std::string_view{capability};
        });
        const ExtensionHostEnvironment host = CurrentHostEnvironment(m_hostProfile, capabilityViews);
        auto planResult = ResolveExtensionModules(manifest, host);
        if (planResult.HasError())
            return Result<std::string>::Failure(planResult.ErrorValue());
        ExtensionModulePlan plan = std::move(planResult).Value();

        const auto platformDeclarations = std::ranges::count_if(manifest.contributions, [](const auto &contribution) {
            return contribution.type == "platform.services.provider";
        });
        if (platformDeclarations > 1 || (platformDeclarations != 0 && manifest.contributions.size() != 1))
            return Result<std::string>::Failure(
                MakeError(ExtensionErrors::ContributionRejected,
                          "Platform provider packages must publish exactly one provider-only contribution."));
        if (platformDeclarations != 0 && (m_platformProviderCommit == nullptr ||
                                          !std::ranges::binary_search(m_hostCapabilities, std::string{"platform.services.provider"})))
            return Result<std::string>::Failure(
                MakeError(ExtensionErrors::CapabilityUnavailable, "Host composition has not admitted platform provider contributions."));
        if (platformDeclarations != 0) {
            const auto &claim = manifest.contributions.front();
            const auto module = std::ranges::find(manifest.modules, claim.owningModule, &ExtensionModuleManifest::id);
            if (module == manifest.modules.end() || !module->imports.empty() ||
                std::ranges::find(module->requiredCapabilities, "platform.services.provider") == module->requiredCapabilities.end())
                return Result<std::string>::Failure(
                    MakeError(ExtensionErrors::ContributionRejected,
                              "Provider module must require its host capability and cannot import services in ABI profile 1."));
        }

        ExtensionActivationTransaction transaction;
        std::vector<ExtensionPlatformProviderCandidate> platformCandidates;
        for (std::size_t moduleIndex = 0; moduleIndex < plan.moduleIds.size(); ++moduleIndex) {
            const std::string &moduleId = plan.moduleIds[moduleIndex];
            const auto manifestModule = std::ranges::find(manifest.modules, moduleId, &ExtensionModuleManifest::id);
            if (manifestModule == manifest.modules.end())
                return Result<std::string>::Failure(transaction.Rollback(
                    MakeError(ExtensionErrors::ModuleResolutionFailed, "Resolved module is absent from the package manifest.")));

            auto libraryPathResult = ResolveModuleLibraryPath(manifest, plan.selectedEntries[moduleIndex]);
            if (libraryPathResult.HasError())
                return Result<std::string>::Failure(transaction.Rollback(libraryPathResult.ErrorValue()));
            if (auto evidence = VerifyCurrentArtifact(m_artifactGate, libraryPathResult.Value()); evidence.HasError())
                return Result<std::string>::Failure(transaction.Rollback(evidence.ErrorValue()));
            auto loadResult = m_libraryLoader(libraryPathResult.Value().string());
            if (loadResult.HasError())
                return Result<std::string>::Failure(transaction.Rollback(loadResult.ErrorValue()));
            std::shared_ptr<Platform::DynamicLibrary> library{std::move(loadResult).Value()};
            const bool providerModule = platformDeclarations != 0 && manifest.contributions.front().owningModule == moduleId;
            auto activatedResult = ActivateModule(library, manifest, *manifestModule, providerModule);
            if (activatedResult.HasError())
                return Result<std::string>::Failure(transaction.Rollback(activatedResult.ErrorValue()));

            ActivatedModule activated = std::move(activatedResult).Value();
            platformCandidates.insert(platformCandidates.end(), std::make_move_iterator(activated.platformProviders.begin()),
                                      std::make_move_iterator(activated.platformProviders.end()));
            transaction.Stage(std::move(activated.lifetime), std::move(activated.contributions));
        }
        if (platformCandidates.size() != static_cast<std::size_t>(platformDeclarations))
            return Result<std::string>::Failure(transaction.Rollback(
                MakeError(ExtensionErrors::ContributionRejected, "Provider module did not register its exact declared contribution.")));
        ExtensionPlatformProviderPublication platformPublication;
        if (!platformCandidates.empty()) {
            auto commitProvider = [&]() -> Result<ExtensionPlatformProviderPublication> {
                try {
                    return m_platformProviderCommit(std::move(platformCandidates.front()));
                } catch (...) {  // Keep host-composition callback failure inside the manager's result contract.
                    return Result<ExtensionPlatformProviderPublication>::Failure(
                        MakeError(ExtensionErrors::ContributionRejected, "Platform provider host commit threw an exception."));
                }
            };
            auto committed = commitProvider();
            if (committed.HasError())
                return Result<std::string>::Failure(transaction.Rollback(committed.ErrorValue()));
            platformPublication = std::move(committed).Value();
            if (platformPublication == nullptr)
                return Result<std::string>::Failure(transaction.Rollback(
                    MakeError(ExtensionErrors::ContributionRejected, "Provider commit returned no revocation owner.")));
        }
        if (auto committed = CommitContributions(transaction.Contributions(), m_importerCatalog); committed.HasError())
            return Result<std::string>::Failure(transaction.Rollback(committed.ErrorValue()));

        const std::string extensionId =
            CommitLoadedExtension(std::move(manifest), std::move(plan), transaction, std::move(platformPublication), m_loadedExtensions);
        LOG_INFO("extensions", "Successfully loaded extension: %s", extensionId.c_str());
        return Result<std::string>::Success(std::move(extensionId));
    }

    void ExtensionManager::UnloadExtension(const std::string &extensionId) {
        if (const auto it = m_loadedExtensions.find(extensionId); it != m_loadedExtensions.end()) {
            m_loadedExtensions.erase(it);
            LOG_INFO("extensions", "Released extension manager lease: %s", extensionId.c_str());
        }
    }

    void ExtensionManager::UnloadAll() {
        m_loadedExtensions.clear();
    }

    std::vector<std::string> ExtensionManager::GetLoadedExtensionIds() const {
        std::vector<std::string> ids;
        ids.reserve(m_loadedExtensions.size());
        for (const auto &key : m_loadedExtensions | std::views::keys)
            ids.push_back(key);
        std::ranges::sort(ids);
        return ids;
    }
}  // namespace Horo::Extensions
