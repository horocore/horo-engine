#include "Horo/Extensions/ExtensionInventory.h"

#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Foundation/TransparentString.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <unordered_map>

namespace Horo::Extensions {
    namespace {
        namespace fs = std::filesystem;
        using Json = nlohmann::json;

        constexpr ExtensionManifestLimits kManifestLimits{};
        constexpr std::size_t kMaximumPackageEntries = 4096;
        constexpr std::uintmax_t kMaximumPackageBytes = 1024ULL * 1024ULL * 1024ULL;

        /** @brief Tracks bounded resource consumption while staging a package tree. */
        struct PackageCopyBudget {
            std::size_t entryCount{};
            std::uintmax_t totalBytes{};
        };

        /** @brief Carries the validated relative path and filesystem type of one package entry. */
        struct ValidatedPackageEntry {
            fs::path relativePath;
            fs::file_status status;
        };

        /** @brief Parsed direct-directory manifest plus its exact encoded-byte identity. */
        struct ParsedManifestFile {
            ExtensionManifest manifest;
            std::string compositionVersion;
        };

        /** @brief Preserves process-local activation state across inventory refreshes. */
        struct RuntimeState {
            bool active{};
            ExtensionHostCompatibilityState compatibility{ExtensionHostCompatibilityState::NotEvaluated};
            ExtensionActivationOutcome outcome{ExtensionActivationOutcome::NotAttempted};
            ExtensionActivationFailure failure;
            std::string compositionVersion;
            std::uint64_t stateRevision{1};
            std::uint64_t activationGeneration{};
        };

        using RuntimeStateMap = TransparentStringMap<RuntimeState>;

        [[nodiscard]] std::string EnvironmentValue(const char *name) {
#if defined(_WIN32)
            std::size_t length = 0;
            if (getenv_s(&length, nullptr, 0, name) != 0 || length <= 1)
                return {};
            std::string value(length, '\0');
            if (getenv_s(&length, value.data(), value.size(), name) != 0 || length <= 1)
                return {};
            value.resize(length - 1);
            return value;
#else
            const char *value = std::getenv(name);
            return value != nullptr ? std::string{value} : std::string{};
#endif
        }

        [[nodiscard]] bool IsSafePackageId(const std::string_view value) {
            return !value.empty() && value != "." && value != ".." && std::ranges::all_of(value, [](const unsigned char character) {
                return std::isalnum(character) != 0 || character == '.' || character == '-' || character == '_';
            });
        }

        [[nodiscard]] bool IsPathContainedBy(const fs::path &root, const fs::path &candidate) {
            auto rootComponent = root.begin();
            auto candidateComponent = candidate.begin();
            for (; rootComponent != root.end(); ++rootComponent, ++candidateComponent) {
                if (candidateComponent == candidate.end() || *rootComponent != *candidateComponent)
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool IsSafeRelativePath(const fs::path &relative) {
            if (relative.empty() || relative.is_absolute())
                return false;
            return std::ranges::none_of(relative, [](const fs::path &component) {
                return component == "..";
            });
        }

        [[nodiscard]] std::string BuildBuiltInCompositionVersion(const std::string_view packageVersion,
                                                                 const std::vector<ExtensionModuleManifest> &modules) {
            std::string fingerprint{packageVersion};
            for (const auto &mod : modules) {
                fingerprint.append("|");
                fingerprint.append(std::to_string(mod.id.size()));
                fingerprint.append(":");
                fingerprint.append(mod.id);
                fingerprint.append("@");
                fingerprint.append(mod.version);
            }
            const auto bytes = std::as_bytes(std::span{fingerprint.data(), fingerprint.size()});
            return "sha256:" + FormatSha256(ComputeSha256(bytes));
        }

        [[nodiscard]] Result<ParsedManifestFile> ReadManifest(const fs::path &absoluteManifestPath) {
            if (!absoluteManifestPath.is_absolute())
                return Result<ParsedManifestFile>::Failure(
                    MakeError(ExtensionErrors::InvalidManifest, "Extension manifest path must be absolute."));
            std::error_code error;
            if (!fs::is_regular_file(absoluteManifestPath, error) || error ||
                fs::is_symlink(fs::symlink_status(absoluteManifestPath, error))) {
                return Result<ParsedManifestFile>::Failure(
                    MakeError(ExtensionErrors::InvalidManifest, "Extension manifest must be a regular non-symlink file."));
            }
            if (const std::uintmax_t size = fs::file_size(absoluteManifestPath, error);
                error || size > kManifestLimits.maximumDocumentBytes)
                return Result<ParsedManifestFile>::Failure(
                    MakeError(ExtensionErrors::InvalidManifest, "Extension manifest exceeds the bounded size."));
            std::ifstream input(absoluteManifestPath, std::ios::binary);
            std::ostringstream contents;
            contents << input.rdbuf();
            const std::string encoded = contents.str();
            auto parsed = ParseExtensionManifest(encoded, kManifestLimits);
            if (parsed.HasError())
                return Result<ParsedManifestFile>::Failure(parsed.ErrorValue());
            const auto bytes = std::as_bytes(std::span{encoded.data(), encoded.size()});
            return Result<ParsedManifestFile>::Success(
                {.manifest = std::move(parsed).Value(), .compositionVersion = "sha256:" + FormatSha256(ComputeSha256(bytes))});
        }

        /** @brief Validates one source entry and accounts for the package entry limit. */
        [[nodiscard]] Result<ValidatedPackageEntry> ValidatePackageEntry(const fs::directory_entry &entry, const fs::path &source,
                                                                         PackageCopyBudget &budget) {
            if (++budget.entryCount > kMaximumPackageEntries) {
                return Result<ValidatedPackageEntry>::Failure(
                    MakeError(ExtensionErrors::LoadFailed, "Extension package traversal failed or exceeded entry limits."));
            }
            std::error_code error;
            const fs::file_status status = entry.symlink_status(error);
            if (error || fs::is_symlink(status)) {
                return Result<ValidatedPackageEntry>::Failure(
                    MakeError(ExtensionErrors::LoadFailed, "Extension packages may not contain symlinks."));
            }
            const fs::path relative = fs::relative(entry.path(), source, error);
            if (error || !IsSafeRelativePath(relative)) {
                return Result<ValidatedPackageEntry>::Failure(
                    MakeError(ExtensionErrors::LoadFailed, "Extension package path escaped its source root."));
            }
            return Result<ValidatedPackageEntry>::Success({.relativePath = relative, .status = status});
        }

        /** @brief Copies one validated package entry while enforcing the aggregate byte limit. */
        [[nodiscard]] Result<void> CopyPackageEntry(const fs::directory_entry &entry, const fs::path &target, const fs::file_status &status,
                                                    PackageCopyBudget &budget) {
            std::error_code error;
            if (fs::is_directory(status)) {
                fs::create_directories(target, error);
            } else if (fs::is_regular_file(status)) {
                const std::uintmax_t size = fs::file_size(entry.path(), error);
                if (error || size > kMaximumPackageBytes - budget.totalBytes) {
                    return Result<void>::Failure(
                        MakeError(ExtensionErrors::LoadFailed, "Extension package exceeds the bounded byte size."));
                }
                budget.totalBytes += size;
                fs::create_directories(target.parent_path(), error);
                if (!error)
                    fs::copy_file(entry.path(), target, fs::copy_options::none, error);
            } else {
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::LoadFailed, "Extension packages may contain only regular files and directories."));
            }
            if (error)
                return Result<void>::Failure(MakeError(ExtensionErrors::LoadFailed, "Unable to stage extension package contents."));
            return Result<void>::Success();
        }

        /** @brief Copies a bounded, symlink-free package tree into a staging directory. */
        [[nodiscard]] Result<void> CopyPackageTree(const fs::path &source, const fs::path &destination) {
            std::error_code error;
            fs::create_directories(destination, error);
            if (error)
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::LoadFailed, "Unable to create extension installation staging directory."));

            PackageCopyBudget budget;
            fs::recursive_directory_iterator it{source, fs::directory_options::skip_permission_denied, error};
            const fs::recursive_directory_iterator end;
            while (!error && it != end) {
                auto validated = ValidatePackageEntry(*it, source, budget);
                if (validated.HasError())
                    return Result<void>::Failure(validated.ErrorValue());
                const ValidatedPackageEntry &packageEntry = validated.Value();
                if (auto copied = CopyPackageEntry(*it, destination / packageEntry.relativePath, packageEntry.status, budget);
                    copied.HasError())
                    return copied;
                it.increment(error);
            }
            if (error)
                return Result<void>::Failure(MakeError(ExtensionErrors::LoadFailed, "Unable to stage extension package contents."));
            return Result<void>::Success();
        }

        /** @brief Moves current runtime-only state out of inventory entries before rediscovery. */
        [[nodiscard]] RuntimeStateMap CaptureRuntimeStates(std::vector<ExtensionInventoryEntry> &entries) {
            RuntimeStateMap runtimeStates;
            runtimeStates.reserve(entries.size());
            for (auto &entry : entries) {
                runtimeStates.try_emplace(entry.packageId, RuntimeState{.active = entry.runtimeActive,
                                                                        .compatibility = entry.hostCompatibility,
                                                                        .outcome = entry.activationOutcome,
                                                                        .failure = std::move(entry.activationFailure),
                                                                        .compositionVersion = std::move(entry.runtimeCompositionVersion),
                                                                        .stateRevision = entry.stateRevision,
                                                                        .activationGeneration = entry.activationGeneration});
            }
            return runtimeStates;
        }

        /** @brief Restores runtime-only state onto newly discovered entries with matching identities. */
        void RestoreRuntimeStates(std::vector<ExtensionInventoryEntry> &entries, RuntimeStateMap &runtimeStates) {
            for (auto &entry : entries) {
                if (auto runtime = runtimeStates.find(entry.packageId); runtime != runtimeStates.end()) {
                    entry.runtimeActive = runtime->second.active;
                    entry.hostCompatibility = runtime->second.compatibility;
                    entry.activationOutcome = runtime->second.outcome;
                    entry.activationFailure = std::move(runtime->second.failure);
                    entry.loadError = entry.activationFailure.message;
                    entry.runtimeCompositionVersion = std::move(runtime->second.compositionVersion);
                    entry.stateRevision = runtime->second.stateRevision;
                    entry.activationGeneration = runtime->second.activationGeneration;
                    if (entry.runtimeCompositionVersion != entry.compositionVersion &&
                        entry.stateRevision != std::numeric_limits<std::uint64_t>::max())
                        ++entry.stateRevision;
                }
            }
        }

        /** @brief Reports whether a directory entry is eligible for manifest discovery. */
        [[nodiscard]] bool IsDiscoverableDirectory(const fs::directory_entry &directory, std::error_code &error) {
            const fs::file_status status = directory.symlink_status(error);
            return !error && !fs::is_symlink(status) && fs::is_directory(status) &&
                   !directory.path().filename().string().starts_with(".install-");
        }

        /** @brief Builds one installed-package projection, or skips an invalid or duplicate package. */
        [[nodiscard]] std::optional<ExtensionInventoryEntry> ReadInstalledEntry(
            const fs::directory_entry &directory, const std::vector<ExtensionInventoryEntry> &existing, const TransparentStringSet &enabled,
            const TransparentStringMap<std::string> &trustedCompositions) {
            auto parsed = ReadManifest(fs::absolute(directory.path() / "extension.json"));
            if (parsed.HasError())
                return std::nullopt;
            ParsedManifestFile manifestFile = std::move(parsed).Value();
            ExtensionManifest manifest = std::move(manifestFile.manifest);
            if (const bool duplicate = std::ranges::any_of(existing,
                                                           [&manifest](const ExtensionInventoryEntry &entry) {
                return entry.packageId == manifest.id;
            });
                !IsSafePackageId(manifest.id) || directory.path().filename() != fs::path{manifest.id} || duplicate)
                return std::nullopt;
            const std::string compositionVersion = std::move(manifestFile.compositionVersion);
            const auto trust = trustedCompositions.find(manifest.id);
            const bool hasCurrentTrust = trust != trustedCompositions.end() && trust->second == compositionVersion;
            return ExtensionInventoryEntry{
                .packageId = manifest.id,
                .displayName = manifest.displayName.empty() ? manifest.id : manifest.displayName,
                .description = manifest.description,
                .version = manifest.version,
                .author = manifest.author,
                .origin = ExtensionOrigin::UserInstalled,
                .absoluteRootPath = fs::absolute(directory.path()).lexically_normal(),
                .absoluteManifestPath = fs::absolute(directory.path() / "extension.json").lexically_normal(),
                .modules = std::move(manifest.modules),
                .contributions = std::move(manifest.contributions),
                .enabled = enabled.contains(manifest.id),
                .locallyTrusted = hasCurrentTrust,
                .compositionVersion = compositionVersion,
                .trustedCompositionVersion = hasCurrentTrust ? compositionVersion : std::string{},
            };
        }

        /** @brief Adds valid installed packages from the managed root to the inventory projection. */
        [[nodiscard]] Result<void> DiscoverInstalledEntries(const fs::path &installRoot, std::vector<ExtensionInventoryEntry> &entries,
                                                            const TransparentStringSet &enabled,
                                                            const TransparentStringMap<std::string> &trustedCompositions) {
            std::error_code error;
            fs::create_directories(installRoot, error);
            if (error)
                return Result<void>::Failure(MakeError(ExtensionErrors::LoadFailed, "Unable to create the user extension directory."));
            for (const auto &directory : fs::directory_iterator(installRoot, error)) {
                if (error)
                    return Result<void>::Failure(MakeError(ExtensionErrors::LoadFailed, "Unable to enumerate installed extensions."));
                if (!IsDiscoverableDirectory(directory, error))
                    continue;
                if (auto entry = ReadInstalledEntry(directory, entries, enabled, trustedCompositions); entry.has_value())
                    entries.push_back(std::move(*entry));
            }
            return Result<void>::Success();
        }

        /** @brief Resolves and validates an installation source outside the managed root. */
        [[nodiscard]] Result<fs::path> ValidateInstallSource(const fs::path &absoluteSourceDirectory, const fs::path &installRoot) {
            if (!absoluteSourceDirectory.is_absolute())
                return Result<fs::path>::Failure(
                    MakeError(ExtensionErrors::InvalidManifest, "Extension source directory must be absolute."));
            std::error_code error;
            const fs::path source = fs::weakly_canonical(absoluteSourceDirectory, error);
            if (error || !fs::is_directory(source, error) || fs::is_symlink(fs::symlink_status(source, error))) {
                return Result<fs::path>::Failure(
                    MakeError(ExtensionErrors::InvalidManifest, "Extension source must be a regular non-symlink directory."));
            }
            if (const fs::path canonicalInstallRoot = fs::weakly_canonical(installRoot, error);
                !error && IsPathContainedBy(canonicalInstallRoot, source)) {
                return Result<fs::path>::Failure(
                    MakeError(ExtensionErrors::InvalidManifest, "Extension source must be outside the managed installation directory."));
            }
            return Result<fs::path>::Success(source);
        }

        /** @brief Removes an unpublished staging tree without masking the primary operation result. */
        void RemoveStagingTree(const fs::path &staging) noexcept {
            std::error_code ignored;
            fs::remove_all(staging, ignored);
        }

        /** @brief Copies and atomically publishes a validated package directory. */
        [[nodiscard]] Result<void> PublishPackage(const fs::path &source, const fs::path &staging, const fs::path &destination) {
            if (auto copied = CopyPackageTree(source, staging); copied.HasError()) {
                RemoveStagingTree(staging);
                return copied;
            }
            std::error_code error;
            fs::rename(staging, destination, error);
            if (error) {
                RemoveStagingTree(staging);
                return Result<void>::Failure(MakeError(ExtensionErrors::LoadFailed, "Unable to publish the staged extension package."));
            }
            return Result<void>::Success();
        }

        /** @brief Reads and validates the identity needed to install a package. */
        [[nodiscard]] Result<ExtensionManifest> ReadInstallManifest(const fs::path &source) {
            auto parsed = ReadManifest(source / "extension.json");
            if (parsed.HasError())
                return Result<ExtensionManifest>::Failure(parsed.ErrorValue());
            ParsedManifestFile manifestFile = std::move(parsed).Value();
            if (!IsSafePackageId(manifestFile.manifest.id))
                return Result<ExtensionManifest>::Failure(
                    MakeError(ExtensionErrors::InvalidManifest, "Extension package ID is unsafe for installation."));
            return Result<ExtensionManifest>::Success(std::move(manifestFile.manifest));
        }

        /** @brief Creates the managed root and resolves an unused package destination. */
        [[nodiscard]] Result<fs::path> PrepareInstallDestination(const fs::path &installRoot, const std::string_view packageId) {
            std::error_code error;
            fs::create_directories(installRoot, error);
            if (error)
                return Result<fs::path>::Failure(MakeError(ExtensionErrors::LoadFailed, "Unable to create the user extension directory."));
            const fs::path destination = installRoot / packageId;
            if (fs::exists(destination, error) || error)
                return Result<fs::path>::Failure(
                    MakeError(ExtensionErrors::LoadFailed, "An extension with this package ID is already installed."));
            return Result<fs::path>::Success(destination);
        }

        /** @brief Inserts string values from one persisted JSON array into a state set. */
        void LoadStringSet(const Json &state, const std::string_view field, TransparentStringSet &values) {
            for (const auto &id : state.value(field, Json::array())) {
                if (id.is_string())
                    values.insert(id.get<std::string>());
            }
        }

        /** @brief Loads bounded exact-composition trust records; legacy ID-only trust is intentionally not migrated. */
        void LoadTrustedCompositions(const Json &state, TransparentStringMap<std::string> &values) {
            const auto trusted = state.find("trustedCompositions");
            if (trusted == state.end() || !trusted->is_object())
                return;
            for (auto it = trusted->begin(); it != trusted->end(); ++it) {
                if (IsSafePackageId(it.key()) && it.value().is_string()) {
                    std::string composition = it.value().get<std::string>();
                    if (!composition.empty() && composition.size() <= 512)
                        values.try_emplace(it.key(), std::move(composition));
                }
            }
        }

        /** @brief Replaces legacy entry fields from the canonical typed projection. */
        void PublishProjection(ExtensionInventoryEntry &entry, ExtensionActivationProjection projection) {
            entry.enabled = projection.enablement == ExtensionEnablementState::Enabled;
            entry.locallyTrusted = projection.HasCurrentTrust();
            entry.hostCompatibility = projection.compatibility;
            entry.runtimeActive = projection.runtime == ExtensionRuntimeActivityState::Active;
            entry.activationOutcome = projection.outcome;
            entry.activationFailure = std::move(projection.failure);
            entry.loadError = entry.activationFailure.message;
            entry.compositionVersion = std::move(projection.installedComposition);
            entry.trustedCompositionVersion = std::move(projection.trustedComposition);
            entry.runtimeCompositionVersion = std::move(projection.runtimeComposition);
            entry.stateRevision = projection.stateRevision;
            entry.activationGeneration = projection.activationGeneration;
        }

        /** @brief Finds one mutable inventory entry without duplicating public-operation admission. */
        [[nodiscard]] ExtensionInventoryEntry *FindEntry(std::vector<ExtensionInventoryEntry> &entries,
                                                         const std::string_view packageId) noexcept {
            const auto found = std::ranges::find(entries, packageId, &ExtensionInventoryEntry::packageId);
            return found == entries.end() ? nullptr : std::to_address(found);
        }

        /** @brief Applies common exact-entry transition admission for inventory mutations. */
        [[nodiscard]] Result<ExtensionLifecycleTransition> PrepareEntryTransition(const ExtensionInventoryEntry &entry,
                                                                                  const ExtensionLifecycleAction action,
                                                                                  const ExtensionLifecycleOwner owner,
                                                                                  std::string composition = {}) {
            return TransitionExtensionActivation(entry.ActivationState(), {.action = action,
                                                                           .owner = owner,
                                                                           .expectedRevision = entry.stateRevision,
                                                                           .composition = std::move(composition)});
        }
    }  // namespace

    /** @copydoc ExtensionInventoryEntry::ActivationState */
    ExtensionActivationProjection ExtensionInventoryEntry::ActivationState() const {
        return {
            .installation = ExtensionInstallationState::Installed,
            .trust = locallyTrusted ? ExtensionTrustState::Trusted : ExtensionTrustState::Untrusted,
            .enablement = enabled ? ExtensionEnablementState::Enabled : ExtensionEnablementState::Disabled,
            .compatibility = hostCompatibility,
            .runtime = runtimeActive ? ExtensionRuntimeActivityState::Active : ExtensionRuntimeActivityState::Inactive,
            .outcome = activationOutcome,
            .failure = activationFailure,
            .installedComposition = compositionVersion,
            .trustedComposition = trustedCompositionVersion,
            .runtimeComposition = runtimeCompositionVersion,
            .stateRevision = stateRevision,
            .activationGeneration = activationGeneration,
        };
    }

    /** @copydoc ExtensionInventoryEntry::RestartRequired */
    bool ExtensionInventoryEntry::RestartRequired() const {
        return ActivationState().RestartReason() != ExtensionRestartReason::None;
    }

    /** @copydoc ExtensionInventory::ExtensionInventory */
    ExtensionInventory::ExtensionInventory(fs::path absoluteInstallRoot)
        : installRoot_(absoluteInstallRoot.empty() ? DefaultInstallRoot() : std::move(absoluteInstallRoot)) {
        if (!installRoot_.is_absolute())
            installRoot_ = fs::absolute(installRoot_);
        installRoot_ = installRoot_.lexically_normal();
        statePath_ = installRoot_ / "_state.json";
    }

    /** @copydoc ExtensionInventory::DefaultInstallRoot */
    fs::path ExtensionInventory::DefaultInstallRoot() {
#if defined(_WIN32)
        fs::path home{EnvironmentValue("USERPROFILE")};
#else
        fs::path home{EnvironmentValue("HOME")};
#endif
        if (home.empty())
            home = fs::temp_directory_path() / "horo-user";  // NOSONAR(cpp:S5443) Fallback root when HOME is unset in tests.
        return fs::absolute(home / ".horo" / "extensions").lexically_normal();
    }

    /** @copydoc ExtensionInventory::Refresh */
    Result<void> ExtensionInventory::Refresh() {
        RuntimeStateMap runtimeStates = CaptureRuntimeStates(entries_);
        entries_.clear();
        if (auto loaded = LoadState(); loaded.HasError())
            return loaded;
        AddBuiltInPackages();
        if (auto discovered = DiscoverInstalledEntries(installRoot_, entries_, enabled_, trustedCompositions_); discovered.HasError())
            return discovered;
        RestoreRuntimeStates(entries_, runtimeStates);
        std::ranges::sort(entries_, [](const ExtensionInventoryEntry &left, const ExtensionInventoryEntry &right) {
            if (left.origin != right.origin)
                return left.origin == ExtensionOrigin::BuiltIn;
            return left.packageId < right.packageId;
        });
        return Result<void>::Success();
    }

    /** @copydoc ExtensionInventory::Entries */
    const std::vector<ExtensionInventoryEntry> &ExtensionInventory::Entries() const noexcept {
        return entries_;
    }

    /** @copydoc ExtensionInventory::InstallRoot */
    const fs::path &ExtensionInventory::InstallRoot() const noexcept {
        return installRoot_;
    }

    /** @copydoc ExtensionInventory::InstallFromDirectory */
    Result<std::string> ExtensionInventory::InstallFromDirectory(const fs::path &absoluteSourceDirectory) {
        auto validatedSource = ValidateInstallSource(absoluteSourceDirectory, installRoot_);
        if (validatedSource.HasError())
            return Result<std::string>::Failure(validatedSource.ErrorValue());
        const fs::path source = std::move(validatedSource).Value();
        auto parsed = ReadInstallManifest(source);
        if (parsed.HasError())
            return Result<std::string>::Failure(parsed.ErrorValue());
        const ExtensionManifest &manifest = parsed.Value();
        auto preparedDestination = PrepareInstallDestination(installRoot_, manifest.id);
        if (preparedDestination.HasError())
            return Result<std::string>::Failure(preparedDestination.ErrorValue());
        const fs::path destination = std::move(preparedDestination).Value();
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path staging = installRoot_ / std::format(".install-{}-{}", manifest.id, nonce);
        if (auto published = PublishPackage(source, staging, destination); published.HasError())
            return Result<std::string>::Failure(published.ErrorValue());
        enabled_.erase(manifest.id);
        trustedCompositions_.erase(manifest.id);
        if (auto saved = SaveState(); saved.HasError()) {
            std::error_code ignored;
            fs::remove_all(destination, ignored);
            return Result<std::string>::Failure(saved.ErrorValue());
        }
        if (auto refreshed = Refresh(); refreshed.HasError())
            return Result<std::string>::Failure(refreshed.ErrorValue());
        return Result<std::string>::Success(manifest.id);
    }

    /** @copydoc ExtensionInventory::SetEnabled */
    Result<void> ExtensionInventory::SetEnabled(const std::string_view packageId, const bool enabled) {
        ExtensionInventoryEntry *entry = FindEntry(entries_, packageId);
        if (entry == nullptr)
            return Result<void>::Failure(MakeError(ExtensionErrors::InvalidManifest, "Unknown extension package ID."));
        auto transition =
            PrepareEntryTransition(*entry,
                                   enabled ? ExtensionLifecycleAction::EnableForProject : ExtensionLifecycleAction::DisableForProject,
                                   ExtensionLifecycleOwner::PackageLifecycleService);
        if (transition.HasError())
            return Result<void>::Failure(transition.ErrorValue());
        const bool wasEnabled = enabled_.contains(entry->packageId);
        if (enabled)
            enabled_.insert(entry->packageId);
        else
            enabled_.erase(entry->packageId);
        if (auto saved = SaveState(); saved.HasError()) {
            if (wasEnabled)
                enabled_.insert(entry->packageId);
            else
                enabled_.erase(entry->packageId);
            return saved;
        }
        PublishProjection(*entry, std::move(transition).Value().next);
        return Result<void>::Success();
    }

    /** @copydoc ExtensionInventory::SetTrusted */
    Result<void> ExtensionInventory::SetTrusted(const std::string_view packageId, const bool trusted) {
        const ExtensionLifecycleAction action = trusted ? ExtensionLifecycleAction::GrantTrust : ExtensionLifecycleAction::RevokeTrust;
        ExtensionInventoryEntry *entry = FindEntry(entries_, packageId);
        if (entry == nullptr)
            return Result<void>::Failure(MakeError(ExtensionErrors::InvalidManifest, "Unknown extension package ID."));
        auto transition = PrepareEntryTransition(*entry, action, ExtensionLifecycleOwner::TrustService,
                                                 trusted ? entry->compositionVersion : std::string{});
        if (transition.HasError())
            return Result<void>::Failure(transition.ErrorValue());
        const auto previous = trustedCompositions_.find(entry->packageId);
        const std::optional<std::string> previousComposition =
            previous == trustedCompositions_.end() ? std::nullopt : std::optional<std::string>{previous->second};
        if (trusted)
            trustedCompositions_.insert_or_assign(entry->packageId, entry->compositionVersion);
        else
            trustedCompositions_.erase(entry->packageId);
        if (auto saved = SaveState(); saved.HasError()) {
            if (previousComposition.has_value())
                trustedCompositions_.insert_or_assign(entry->packageId, *previousComposition);
            else
                trustedCompositions_.erase(entry->packageId);
            return saved;
        }
        PublishProjection(*entry, std::move(transition).Value().next);
        return Result<void>::Success();
    }

    /** @copydoc ExtensionInventory::MarkRuntimeActive */
    Result<void> ExtensionInventory::MarkRuntimeActive(const std::string_view packageId) {
        ExtensionInventoryEntry *entry = FindEntry(entries_, packageId);
        if (entry == nullptr)
            return Result<void>::Failure(MakeError(ExtensionErrors::InvalidManifest, "Unknown extension package ID."));
        auto state = entry->ActivationState();
        if (state.compatibility == ExtensionHostCompatibilityState::NotEvaluated) {
            auto compatible = TransitionExtensionActivation(state, {.action = ExtensionLifecycleAction::MarkCompatible,
                                                                    .owner = ExtensionLifecycleOwner::PackageLifecycleService,
                                                                    .expectedRevision = state.stateRevision});
            if (compatible.HasError())
                return Result<void>::Failure(compatible.ErrorValue());
            state = std::move(compatible).Value().next;
        }
        auto loaded = TransitionExtensionActivation(state, {.action = ExtensionLifecycleAction::MarkLoaded,
                                                            .owner = ExtensionLifecycleOwner::ExtensionHost,
                                                            .expectedRevision = state.stateRevision,
                                                            .composition = state.installedComposition});
        if (loaded.HasError())
            return Result<void>::Failure(loaded.ErrorValue());
        state = std::move(loaded).Value().next;
        if (nextActivationGeneration_ == 0)
            return Result<void>::Failure(MakeError(ExtensionErrors::LifecycleCapacityExceeded));
        auto active = TransitionExtensionActivation(state, {.action = ExtensionLifecycleAction::MarkActive,
                                                            .owner = ExtensionLifecycleOwner::ExtensionHost,
                                                            .expectedRevision = state.stateRevision,
                                                            .activationGeneration = nextActivationGeneration_,
                                                            .composition = state.installedComposition});
        if (active.HasError())
            return Result<void>::Failure(active.ErrorValue());
        ++nextActivationGeneration_;
        PublishProjection(*entry, std::move(active).Value().next);
        return Result<void>::Success();
    }

    /** @copydoc ExtensionInventory::RecordActivationFailure */
    Result<void> ExtensionInventory::RecordActivationFailure(const std::string_view packageId,
                                                             const ExtensionActivationFailureReason reason, std::string message) {
        ExtensionInventoryEntry *entry = FindEntry(entries_, packageId);
        if (entry == nullptr)
            return Result<void>::Failure(MakeError(ExtensionErrors::InvalidManifest, "Unknown extension package ID."));
        auto failed =
            TransitionExtensionActivation(entry->ActivationState(), {.action = ExtensionLifecycleAction::RecordActivationFailure,
                                                                     .owner = ExtensionLifecycleOwner::PackageLifecycleService,
                                                                     .expectedRevision = entry->stateRevision,
                                                                     .failure = {.reason = reason, .message = std::move(message)}});
        if (failed.HasError())
            return Result<void>::Failure(failed.ErrorValue());
        PublishProjection(*entry, std::move(failed).Value().next);
        return Result<void>::Success();
    }

    /** @copydoc ExtensionInventory::EnabledUserPackageRoots */
    std::vector<fs::path> ExtensionInventory::EnabledUserPackageRoots() const {
        std::vector<fs::path> roots;
        for (const auto &entry : entries_) {
            if (entry.origin == ExtensionOrigin::UserInstalled &&
                entry.ActivationState().DesiredActivation() == ExtensionDesiredActivation::Active && entry.absoluteRootPath.is_absolute())
                roots.push_back(entry.absoluteRootPath);
        }
        return roots;
    }

    /** @copydoc ExtensionInventory::IsEnabled */
    bool ExtensionInventory::IsEnabled(const std::string_view packageId) const noexcept {
        return enabled_.contains(std::string{packageId});
    }

    Result<void> ExtensionInventory::LoadState() {
        enabled_.clear();
        trustedCompositions_.clear();
        if (std::error_code error; !fs::exists(statePath_, error) || error) {
            enabled_.emplace("horo.builtin.assets");
            return Result<void>::Success();
        }
        std::ifstream input(statePath_, std::ios::binary);
        try {
            const Json state = Json::parse(input);
            LoadStringSet(state, "enabled", enabled_);
            LoadTrustedCompositions(state, trustedCompositions_);
        } catch (const Json::exception &) {
            return Result<void>::Failure(MakeError(ExtensionErrors::InvalidManifest, "Extension activation state is malformed."));
        }
        return Result<void>::Success();
    }

    Result<void> ExtensionInventory::SaveState() const {
        std::error_code error;
        fs::create_directories(installRoot_, error);
        if (error)
            return Result<void>::Failure(MakeError(ExtensionErrors::LoadFailed, "Unable to create extension state directory."));
        std::vector<std::string> enabled{enabled_.begin(), enabled_.end()};
        std::ranges::sort(enabled);
        Json trustedCompositions = Json::object();
        std::vector<std::string> trustedIds;
        trustedIds.reserve(trustedCompositions_.size());
        for (const auto &[id, composition] : trustedCompositions_)
            trustedIds.push_back(id);
        std::ranges::sort(trustedIds);
        for (const auto &id : trustedIds)
            trustedCompositions[id] = trustedCompositions_.at(id);
        const std::string serialized =
            Json{
                {"schemaVersion", 2},
                {"enabled", enabled},
                {"trustedCompositions", trustedCompositions},
            }
                .dump(2) +
            "\n";
        const fs::path prepared = statePath_.string() + ".tmp";
        NativeDurableFileSystem files;
        const auto bytes = std::as_bytes(std::span{serialized.data(), serialized.size()});
        if (auto written = files.WriteDurable(prepared, bytes); written.HasError())
            return written;
        return files.AtomicReplace(prepared, statePath_);
    }

    void ExtensionInventory::AddBuiltInPackages() {
        ExtensionInventoryEntry builtIn{
            .packageId = "horo.builtin.assets",
            .displayName = "Horo Asset Importers",
            .description = "Built-in OBJ and FBX mesh import, metadata, reimport, and preview providers.",
            .version = "1.0.0",
            .author = "Horo Engine",
            .origin = ExtensionOrigin::BuiltIn,
            .modules =
                {
                    {.id = "horo.builtin.assets.importer.obj", .version = "1.0.0", .kind = "asset_importer"},
                    {.id = "horo.builtin.assets.importer.fbx", .version = "1.0.0", .kind = "asset_importer"},
                    {.id = "horo.builtin.assets.importer.terrain", .version = "1.0.0", .kind = "asset_importer"},
                },
            .contributions =
                {
                    {.type = "asset.importer", .id = "horo.asset-importer.obj-mesh", .owningModule = "horo.builtin.assets.importer.obj"},
                    {.type = "asset.importer", .id = "horo.asset-importer.fbx-mesh", .owningModule = "horo.builtin.assets.importer.fbx"},
                    {.type = "asset.importer", .id = "horo.terrain.raw-f32", .owningModule = "horo.builtin.assets.importer.terrain"},
                },
            .enabled = enabled_.contains("horo.builtin.assets"),
            .locallyTrusted = true,
        };
        builtIn.compositionVersion = BuildBuiltInCompositionVersion(builtIn.version, builtIn.modules);
        builtIn.trustedCompositionVersion = builtIn.compositionVersion;
        entries_.push_back(std::move(builtIn));
    }
}  // namespace Horo::Extensions
