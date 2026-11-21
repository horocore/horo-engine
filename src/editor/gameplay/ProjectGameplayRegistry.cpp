#include "editor/gameplay/ProjectGameplayRegistry.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/BlackboardUsageInspection.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/LuaDecisionNode.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <ranges>
#include <system_error>
#include <utility>

namespace Horo::Editor {
    namespace {
        constexpr std::size_t MaximumDiscoveredScripts = 1024;
        constexpr std::uintmax_t MaximumGameplayManifestBytes = 64U * 1024U;
        constexpr std::size_t MaximumDecisionScriptBytes = 256U * 1024U;

        /** @brief Derives a native Lua sidecar without locale-dependent text conversion. */
        std::filesystem::path LuaMetadataPath(const std::filesystem::path &source) {
            auto metadata = source;
            metadata += ".meta";
            return metadata;
        }

        /** @brief The sidecar's explicit stable identities for a project Decision contribution. */
        struct DecisionScriptIdentity final {
            AI::DecisionNodeTypeId type;
            AI::DecisionProviderId provider;
            std::uint32_t version{};
        };

        /** @brief Parses a fixed-width identity without lossy JSON number conversions. */
        template <typename Id> Result<Id> ParseDecisionScriptId(const nlohmann::json &value) {
            if (!value.is_string())
                return Result<Id>::Failure(MakeError(AI::AIErrors::DecisionAssetSchemaInvalid));
            const auto &text = value.get_ref<const std::string &>();
            std::uint64_t number{};
            const auto end = text.data() + text.size();
            const auto parsed = std::from_chars(text.data(), end, number);
            if (text.starts_with('0') || parsed.ec != std::errc{} || parsed.ptr != end)
                return Result<Id>::Failure(MakeError(AI::AIErrors::DecisionAssetSchemaInvalid));
            return Id::Create(number);
        }

        /** @brief Routes only an explicit Decision sidecar into the new script authority. */
        Result<std::optional<DecisionScriptIdentity>> ReadDecisionScriptIdentity(const std::filesystem::path &root,
                                                                                 const std::filesystem::path &source) {
            using Identity = Result<std::optional<DecisionScriptIdentity>>;
            const auto capture = Assets::CaptureAssetSource(root, LuaMetadataPath(source), MaximumGameplayManifestBytes);
            if (capture.HasError())
                return Identity::Failure(capture.ErrorValue());
            if (!capture.Value().identity.exists)
                return Identity::Success(std::nullopt);
            const auto &bytes = capture.Value().bytes;
            const std::string_view text{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
            const auto metadata = nlohmann::json::parse(text, nullptr, false);
            if (!metadata.is_object() || !metadata.contains("decisionNode"))
                return Identity::Success(std::nullopt);
            const auto &fields = metadata["decisionNode"];
            if (metadata.size() != 3 || !metadata.contains("schemaVersion") || !metadata["schemaVersion"].is_number_unsigned() ||
                metadata["schemaVersion"] != 1 || !metadata.contains("runtime") || metadata["runtime"] != "lua" || !fields.is_object() ||
                fields.size() != 3 || !fields.contains("typeId") || !fields.contains("providerId") ||
                !fields.contains("descriptorVersion") || !fields["descriptorVersion"].is_number_unsigned())
                return Identity::Failure(MakeError(AI::AIErrors::DecisionAssetSchemaInvalid, "Invalid project Decision script sidecar."));
            const auto version = fields["descriptorVersion"].get<std::uint64_t>();
            const auto type = ParseDecisionScriptId<AI::DecisionNodeTypeId>(fields["typeId"]);
            const auto provider = ParseDecisionScriptId<AI::DecisionProviderId>(fields["providerId"]);
            if (type.HasError() || provider.HasError() || version == 0 || version > std::numeric_limits<std::uint32_t>::max())
                return Identity::Failure(MakeError(AI::AIErrors::DecisionAssetSchemaInvalid, "Invalid project Decision script identity."));
            return Identity::Success(DecisionScriptIdentity{type.Value(), provider.Value(), static_cast<std::uint32_t>(version)});
        }

        /** @brief Compiles bounded source bytes from the Project's physically contained script asset. */
        Result<std::shared_ptr<Gameplay::LuaDecisionNodeProgram>> LoadDecisionScript(const std::filesystem::path &root,
                                                                                     const std::filesystem::path &source,
                                                                                     const DecisionScriptIdentity identity) {
            using Program = Result<std::shared_ptr<Gameplay::LuaDecisionNodeProgram>>;
            const auto capture = Assets::CaptureAssetSource(root, source, MaximumDecisionScriptBytes);
            if (capture.HasError())
                return Program::Failure(capture.ErrorValue());
            if (!capture.Value().identity.exists || capture.Value().bytes.empty())
                return Program::Failure(MakeError(AI::AIErrors::DecisionAssetSchemaInvalid, "Decision script source is missing."));
            const auto &bytes = capture.Value().bytes;
            const auto path = source.lexically_relative(root).generic_u8string();
            auto compiled =
                Gameplay::LuaDecisionNodeProgram::Compile(std::string{reinterpret_cast<const char *>(bytes.data()), bytes.size()},
                                                          identity.type, identity.provider, identity.version,
                                                          {std::string{path.begin(), path.end()}, 0, 0});
            if (compiled.HasError())
                return Program::Failure(compiled.ErrorValue());
            return Program::Success(std::shared_ptr<Gameplay::LuaDecisionNodeProgram>{std::move(compiled).Value()});
        }

        struct NativeArtifactManifest {
            std::string moduleId;
            std::string buildFingerprint;
            std::uint64_t descriptorRevision{};
            std::filesystem::path artifactPath;
        };

        [[nodiscard]] Error ManifestError(const std::string &message) {
            return MakeError(Gameplay::GameplayErrors::InvalidBehaviorComponent,
                             "Gameplay module artifact manifest is invalid: " + message);
        }

        [[nodiscard]] Result<NativeArtifactManifest> ReadNativeArtifactManifest(const std::filesystem::path &path) {
            std::error_code error;
            if (const std::uintmax_t size = std::filesystem::file_size(path, error);
                error || size == 0 || size > MaximumGameplayManifestBytes)
                return Result<NativeArtifactManifest>::Failure(ManifestError("file size is unavailable or outside the supported limit."));
            std::ifstream input(path, std::ios::binary);
            const nlohmann::json document = nlohmann::json::parse(input, nullptr, false);
            if (document.is_discarded() || !document.is_object() || document.value("schemaVersion", 0) != 1 ||
                !document.contains("moduleId") || !document["moduleId"].is_string() || !document.contains("buildFingerprint") ||
                !document["buildFingerprint"].is_string() || !document.contains("descriptorRevision") ||
                !document["descriptorRevision"].is_number_unsigned() || !document.contains("artifactPath") ||
                !document["artifactPath"].is_string())
                return Result<NativeArtifactManifest>::Failure(ManifestError("required typed fields are missing."));

            const auto &artifactText = document["artifactPath"].get_ref<const std::string &>();
            NativeArtifactManifest manifest{document["moduleId"].get<std::string>(), document["buildFingerprint"].get<std::string>(),
                                            document["descriptorRevision"].get<std::uint64_t>(),
                                            std::filesystem::path{std::u8string{artifactText.begin(), artifactText.end()}}};
            if (manifest.moduleId.empty() || manifest.buildFingerprint.empty() || manifest.descriptorRevision == 0 ||
                !manifest.artifactPath.is_absolute() || !std::filesystem::is_regular_file(manifest.artifactPath, error) || error)
                return Result<NativeArtifactManifest>::Failure(ManifestError("metadata or artifact path is invalid."));
            return Result<NativeArtifactManifest>::Success(std::move(manifest));
        }

        [[nodiscard]] bool HasNativeGameplaySources(const std::filesystem::path &projectRoot) {
            const std::array roots{projectRoot / "source" / "gameplay", projectRoot / "src" / "gameplay"};
            for (const std::filesystem::path &root : roots) {
                std::error_code error;
                if (!std::filesystem::is_directory(root, error))
                    continue;
                for (const std::filesystem::directory_entry &entry :
                     std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied,
                                                                   error)) {
                    if (entry.is_regular_file(error) &&
                        (entry.path().extension() == ".cpp" || entry.path().extension() == ".cc" || entry.path().extension() == ".cxx") &&
                        // GameModule.cpp is the generated bootstrap created by the editor;
                        // it is not evidence that the project contains a native behavior.
                        entry.path().filename() != "GameModule.cpp")
                        return true;
                }
            }
            return false;
        }

    }  // namespace

    /** @copydoc NativeGameplayRollbackArtifact::NativeGameplayRollbackArtifact(std::filesystem::path, std::string, std::uint64_t) */
    NativeGameplayRollbackArtifact::NativeGameplayRollbackArtifact(std::filesystem::path artifactPath, std::string owningModuleId,
                                                                   const std::uint64_t revision)
        : path(std::move(artifactPath)), moduleId(std::move(owningModuleId)), descriptorRevision(revision) {}

    /** @copydoc NativeGameplayRollbackArtifact::~NativeGameplayRollbackArtifact */
    NativeGameplayRollbackArtifact::~NativeGameplayRollbackArtifact() {
        if (path.empty())
            return;
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    /** @copydoc NativeGameplayRollbackArtifact::NativeGameplayRollbackArtifact(NativeGameplayRollbackArtifact&&) */
    NativeGameplayRollbackArtifact::NativeGameplayRollbackArtifact(NativeGameplayRollbackArtifact &&other) noexcept
        : path(std::exchange(other.path, {})), moduleId(std::move(other.moduleId)), descriptorRevision(other.descriptorRevision) {
        other.descriptorRevision = 0;
    }

    /** @copydoc NativeGameplayRollbackArtifact::operator= */
    NativeGameplayRollbackArtifact &NativeGameplayRollbackArtifact::operator=(NativeGameplayRollbackArtifact &&other) noexcept {
        if (this == &other)
            return *this;
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
        path = std::exchange(other.path, {});
        moduleId = std::move(other.moduleId);
        descriptorRevision = std::exchange(other.descriptorRevision, 0);
        return *this;
    }

    ProjectGameplayRegistry::ProjectGameplayRegistry(ConstructionToken, std::shared_ptr<Gameplay::GameModuleRetirementQueue> retirements)
        : moduleRetirements_(retirements ? std::move(retirements) : std::make_shared<Gameplay::GameModuleRetirementQueue>()) {
        static_cast<void>(missingAssetTypes_.Freeze());
        static_cast<void>(missingComponents_.Freeze());
    }

    ProjectGameplayRegistry::~ProjectGameplayRegistry() {
        if (nativeModule_ && retirementAdmission_)
            retirementAdmission_->Retain(std::move(nativeModule_));
        retirementAdmission_.reset();
        (void)moduleRetirements_->Drain();
    }

    ProjectGameplayRegistry::LuaSourceStat ProjectGameplayRegistry::ReadLuaSourceStat(const std::filesystem::path &source,
                                                                                      std::error_code &error) {
        const std::filesystem::path metadata = LuaMetadataPath(source);
        ProjectGameplayRegistry::LuaSourceStat stat;
        stat.sourceWriteTime = std::filesystem::last_write_time(source, error);
        if (error)
            return {};
        stat.metadataWriteTime = std::filesystem::last_write_time(metadata, error);
        if (error)
            return {};
        stat.sourceSize = std::filesystem::file_size(source, error);
        if (error)
            return {};
        stat.metadataSize = std::filesystem::file_size(metadata, error);
        return error ? ProjectGameplayRegistry::LuaSourceStat{} : stat;
    }

    /** @copydoc ProjectGameplayRegistry::Discover */
    std::unique_ptr<ProjectGameplayRegistry> ProjectGameplayRegistry::Discover(
        const std::filesystem::path &projectRoot, std::shared_ptr<Gameplay::GameModuleRetirementQueue> retirements) {
        auto result = std::make_unique<ProjectGameplayRegistry>(ConstructionToken{}, std::move(retirements));
        result->DiscoverNativeModule(projectRoot);
        result->DiscoverLuaPrograms(projectRoot);
        if (const auto catalog = result->RefreshDecisionAuthoringCatalog({}); catalog.HasError())
            result->diagnostics_.emplace_back(ProjectLayout::ScriptsRoot(projectRoot), catalog.ErrorValue());
        result->CaptureReplicationMetadata(projectRoot);
        return result;
    }

    /** @copydoc ProjectGameplayRegistry::DiscoverRollback */
    std::unique_ptr<ProjectGameplayRegistry> ProjectGameplayRegistry::DiscoverRollback(
        const std::filesystem::path &projectRoot, const NativeGameplayRollbackArtifact &artifact,
        const ProjectLuaGenerationSnapshot &luaGeneration, std::shared_ptr<Gameplay::GameModuleRetirementQueue> retirements) {
        auto result = std::make_unique<ProjectGameplayRegistry>(ConstructionToken{}, std::move(retirements));
        result->nativeManifestPath_ = projectRoot / ".horo" / "local" / "gameplay_module.json";
        if (std::error_code manifestError; std::filesystem::is_regular_file(result->nativeManifestPath_, manifestError)) {
            const auto writeTime = std::filesystem::last_write_time(result->nativeManifestPath_, manifestError);
            if (!manifestError)
                result->nativeManifestWriteTime_ = writeTime;
        }
        result->LoadNativeModule(projectRoot, artifact.path, artifact.moduleId, artifact.descriptorRevision);
        result->InstallLuaGeneration(luaGeneration);
        if (const auto catalog = result->RefreshDecisionAuthoringCatalog({}); catalog.HasError())
            result->diagnostics_.emplace_back(ProjectLayout::ScriptsRoot(projectRoot), catalog.ErrorValue());
        result->CaptureReplicationMetadata(projectRoot);
        return result;
    }

    /** @copydoc ProjectGameplayRegistry::DiscoverNativeGeneration */
    std::unique_ptr<ProjectGameplayRegistry> ProjectGameplayRegistry::DiscoverNativeGeneration(
        const std::filesystem::path &projectRoot, const ProjectLuaGenerationSnapshot &luaGeneration,
        std::shared_ptr<Gameplay::GameModuleRetirementQueue> retirements) {
        auto result = std::make_unique<ProjectGameplayRegistry>(ConstructionToken{}, std::move(retirements));
        result->DiscoverNativeModule(projectRoot);
        result->InstallLuaGeneration(luaGeneration);
        if (const auto catalog = result->RefreshDecisionAuthoringCatalog({}); catalog.HasError())
            result->diagnostics_.emplace_back(ProjectLayout::ScriptsRoot(projectRoot), catalog.ErrorValue());
        result->CaptureReplicationMetadata(projectRoot);
        return result;
    }

    void ProjectGameplayRegistry::DiscoverNativeModule(const std::filesystem::path &projectRoot) {
        const std::filesystem::path nativeManifestPath = projectRoot / ".horo" / "local" / "gameplay_module.json";
        nativeManifestPath_ = nativeManifestPath;
        if (!PrepareNativeManifestPath(projectRoot, nativeManifestPath))
            return;

        Result<NativeArtifactManifest> manifest = ReadNativeArtifactManifest(nativeManifestPath);
        if (manifest.HasError()) {
            diagnostics_.emplace_back(nativeManifestPath, manifest.ErrorValue());
            return;
        }
        if (manifest.Value().buildFingerprint != Gameplay::CurrentGameplayBuildFingerprint()) {
            diagnostics_.emplace_back(nativeManifestPath, ManifestError("the artifact was built for a different Horo SDK generation."));
            return;
        }
        LoadNativeModule(projectRoot, manifest.Value().artifactPath, manifest.Value().moduleId, manifest.Value().descriptorRevision);
    }

    bool ProjectGameplayRegistry::PrepareNativeManifestPath(const std::filesystem::path &projectRoot,
                                                            const std::filesystem::path &manifestPath) {
        std::error_code nativeFilesystemError;
        const bool nativeManifestExists = std::filesystem::exists(manifestPath, nativeFilesystemError);
        if (nativeFilesystemError) {
            diagnostics_.emplace_back(manifestPath, ManifestError(nativeFilesystemError.message()));
            return false;
        }
        if (!nativeManifestExists) {
            if (HasNativeGameplaySources(projectRoot))
                diagnostics_.emplace_back(manifestPath,
                                          ManifestError(
                                              "native gameplay sources exist, but no successful module build has been published."));
            return false;
        }

        nativeManifestWriteTime_ = std::filesystem::last_write_time(manifestPath, nativeFilesystemError);
        if (nativeFilesystemError) {
            diagnostics_.emplace_back(manifestPath, ManifestError(nativeFilesystemError.message()));
            return false;
        }
        const bool regularManifest = std::filesystem::is_regular_file(manifestPath, nativeFilesystemError);
        if (nativeFilesystemError) {
            diagnostics_.emplace_back(manifestPath, ManifestError(nativeFilesystemError.message()));
            return false;
        }
        if (!regularManifest) {
            diagnostics_.emplace_back(manifestPath, ManifestError("manifest path is not a regular file."));
            return false;
        }
        return true;
    }

    void ProjectGameplayRegistry::LoadNativeModule(const std::filesystem::path &projectRoot, const std::filesystem::path &artifactPath,
                                                   const std::string_view moduleId, const std::uint64_t descriptorRevision) {
        auto admission = moduleRetirements_->Reserve();
        if (admission.HasError()) {
            diagnostics_.emplace_back(artifactPath, admission.ErrorValue());
            return;
        }
        retirementAdmission_.emplace(std::move(admission).Value());
        Gameplay::GameModuleHost host;
        auto loaded = host.LoadShadowCopy(artifactPath, projectRoot / ".horo" / "local" / "gameplay_module_shadow",
                                          {.moduleId = moduleId,
                                           .buildFingerprint = Gameplay::CurrentGameplayBuildFingerprint(),
                                           .descriptorRevision = descriptorRevision});
        if (loaded.HasError()) {
            diagnostics_.emplace_back(artifactPath, loaded.ErrorValue());
            return;
        }
        auto candidate = std::move(loaded).Value();
        nativeModule_ = std::move(candidate.owner);
        if (candidate.diagnostic) {
            diagnostics_.emplace_back(artifactPath, *candidate.diagnostic);
            retirementAdmission_->Retain(std::move(nativeModule_));
            retirementAdmission_.reset();
            return;
        }
        nativeModuleId_ = std::string{moduleId};
        nativeDescriptorRevision_ = descriptorRevision;
        if (Result<void> contributed = nativeModule_->ContributeBehaviorsTo(registry_); contributed.HasError()) {
            diagnostics_.emplace_back(artifactPath, contributed.ErrorValue());
            return;
        }
        nativeDecisionAvailable_ = true;
    }

    void ProjectGameplayRegistry::DiscoverLuaPrograms(const std::filesystem::path &projectRoot) {
        const std::filesystem::path scriptsRoot = ProjectLayout::ScriptsRoot(projectRoot);
        std::error_code filesystemError;
        if (!std::filesystem::is_directory(scriptsRoot, filesystemError)) {
            if (const Result<void> frozen = registry_.Freeze(); frozen.HasError())
                diagnostics_.emplace_back(scriptsRoot, frozen.ErrorValue());
            return;
        }

        std::vector<std::filesystem::path> sources;
        for (const std::filesystem::directory_entry &entry :
             std::filesystem::recursive_directory_iterator(scriptsRoot, std::filesystem::directory_options::skip_permission_denied,
                                                           filesystemError)) {
            if (!entry.is_regular_file(filesystemError) || entry.path().extension() != ".horo_script")
                continue;
            if (sources.size() == MaximumDiscoveredScripts) {
                diagnostics_.emplace_back(scriptsRoot, MakeError(Gameplay::GameplayErrors::InvalidBehaviorComponent,
                                                                 "Project script discovery exceeds the supported asset count."));
                break;
            }
            sources.emplace_back(entry.path());
        }
        if (filesystemError)
            diagnostics_.emplace_back(scriptsRoot,
                                      MakeError(Gameplay::GameplayErrors::InvalidBehaviorComponent, filesystemError.message()));

        std::ranges::sort(sources);
        for (const std::filesystem::path &source : sources) {
            filesystemError.clear();
            const auto observed = ReadLuaSourceStat(source, filesystemError);
            const auto decision = ReadDecisionScriptIdentity(projectRoot, source);
            if (decision.HasError()) {
                diagnostics_.emplace_back(source, decision.ErrorValue());
                continue;
            }
            if (decision.Value()) {
                if (luaDecisionPrograms_.size() == AI::DecisionNodeGeneration::MaximumNodes) {
                    diagnostics_.emplace_back(source, MakeError(AI::AIErrors::DecisionAssetStorageUnavailable,
                                                                "Project Decision script inventory exceeds its supported limit."));
                    continue;
                }
                auto loaded = LoadDecisionScript(projectRoot, source, *decision.Value());
                if (loaded.HasError()) {
                    diagnostics_.emplace_back(source, loaded.ErrorValue());
                    continue;
                }
                if (filesystemError || ReadLuaSourceStat(source, filesystemError) != observed || filesystemError) {
                    diagnostics_.emplace_back(source, MakeError(Assets::AssetSourceErrors::Conflict));
                    filesystemError.clear();
                    continue;
                }
                luaDecisionPrograms_.push_back({source, std::move(loaded).Value(), observed});
                continue;
            }
            auto loaded = Gameplay::LuaBehaviorProgram::LoadFiles(source, LuaMetadataPath(source));
            if (loaded.HasError()) {
                diagnostics_.emplace_back(source, loaded.ErrorValue());
                continue;
            }
            std::unique_ptr<Gameplay::LuaBehaviorProgram> program = std::move(loaded).Value();
            if (Result<void> registered = registry_.Register(program->Registration()); registered.HasError()) {
                diagnostics_.emplace_back(source, registered.ErrorValue());
                continue;
            }
            luaPrograms_.emplace_back(std::move(program));
            luaSources_.emplace_back(source);
            luaSourceStats_.emplace_back(ReadLuaSourceStat(source, filesystemError));
            filesystemError.clear();
        }

        if (Result<void> frozen = registry_.Freeze(); frozen.HasError())
            diagnostics_.emplace_back(scriptsRoot, frozen.ErrorValue());
    }

    void ProjectGameplayRegistry::InstallLuaGeneration(const ProjectLuaGenerationSnapshot &snapshot) {
        luaDecisionPrograms_ = snapshot.decisions;
        for (const ProjectLuaProgramSnapshot &entry : snapshot.programs) {
            auto cloned = entry.program->Clone();
            if (cloned.HasError()) {
                diagnostics_.emplace_back(entry.source, cloned.ErrorValue());
                continue;
            }
            std::unique_ptr<Gameplay::LuaBehaviorProgram> program = std::move(cloned).Value();
            if (Result<void> registered = registry_.Register(program->Registration()); registered.HasError()) {
                diagnostics_.emplace_back(entry.source, registered.ErrorValue());
                continue;
            }
            luaPrograms_.emplace_back(std::move(program));
            luaSources_.emplace_back(entry.source);
            luaSourceStats_.emplace_back(entry.sourceWriteTime, entry.metadataWriteTime, entry.sourceSize, entry.metadataSize);
        }
        if (Result<void> frozen = registry_.Freeze(); frozen.HasError())
            diagnostics_.emplace_back(std::filesystem::path{}, frozen.ErrorValue());
    }

    /** @copydoc ProjectGameplayRegistry::Registry */
    const Gameplay::BehaviorRegistry &ProjectGameplayRegistry::Registry() const noexcept {
        return registry_;
    }

    /** @copydoc ProjectGameplayRegistry::AssetTypes */
    const Gameplay::GameAssetTypeRegistry &ProjectGameplayRegistry::AssetTypes() const noexcept {
        return nativeModule_ != nullptr ? nativeModule_->AssetTypes() : missingAssetTypes_;
    }

    /** @copydoc ProjectGameplayRegistry::Components */
    const Gameplay::ComponentRegistry &ProjectGameplayRegistry::Components() const noexcept {
        return nativeModule_ != nullptr ? nativeModule_->Components() : missingComponents_;
    }

    /** @copydoc ProjectGameplayRegistry::Diagnostics */
    const std::vector<ProjectGameplayDiagnostic> &ProjectGameplayRegistry::Diagnostics() const noexcept {
        return diagnostics_;
    }

    /** @copydoc ProjectGameplayRegistry::BuildDecisionAuthoringGeneration */
    Result<std::shared_ptr<const AI::DecisionNodeGeneration>> ProjectGameplayRegistry::BuildDecisionAuthoringGeneration(
        const std::shared_ptr<const ProjectPackageDecisionNodeSource> &packages) const {
        using Generation = Result<std::shared_ptr<const AI::DecisionNodeGeneration>>;
        try {
            std::vector<std::shared_ptr<const AI::DecisionNodeGeneration>> parents;
            if (packages) {
                if (!*packages)
                    return Generation::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
                auto acquired = (*packages)();
                if (acquired.HasError())
                    return Generation::Failure(acquired.ErrorValue());
                parents = std::move(acquired).Value();
                if (parents.size() > AI::DecisionNodeGeneration::MaximumNodes)
                    return Generation::Failure(MakeError(AI::AIErrors::DecisionAssetLimitExceeded));
                if (std::ranges::any_of(parents, [](const auto &parent) {
                    return !parent || !parent->CanAdmit();
                }))
                    return Generation::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
            }
            parents.reserve(parents.size() + luaDecisionPrograms_.size() + static_cast<std::size_t>(nativeDecisionAvailable_));
            if (nativeDecisionAvailable_) {
                auto native = nativeModule_->AcquireDecisionNodes();
                if (native.HasError())
                    return Generation::Failure(native.ErrorValue());
                parents.push_back(std::move(native).Value());
            }
            for (const auto &entry : luaDecisionPrograms_) {
                auto script = entry.program->Acquire();
                if (script.HasError())
                    return Generation::Failure(script.ErrorValue());
                parents.push_back(std::move(script).Value());
            }
            if (parents.empty())
                return Generation::Success({});
            if (parents.size() == 1)
                return Generation::Success(std::move(parents.front()));
            return AI::DecisionNodeGeneration::Compose(parents);
        } catch (const std::bad_alloc &) {
            return Generation::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    /** @copydoc ProjectGameplayRegistry::RefreshDecisionAuthoringCatalog */
    Result<void> ProjectGameplayRegistry::RefreshDecisionAuthoringCatalog(
        const std::shared_ptr<const ProjectPackageDecisionNodeSource> &packages) {
        const auto generation = BuildDecisionAuthoringGeneration(packages);
        if (generation.HasError())
            return Result<void>::Failure(generation.ErrorValue());
        if (!generation.Value()) {
            decisionUsageMetadata_.reset();
            decisionAuthoringGeneration_.reset();
            return Result<void>::Success();
        }
        auto metadata = AI::DecisionBlackboardProviderMetadata::Capture(*generation.Value());
        if (metadata.HasError())
            return Result<void>::Failure(metadata.ErrorValue());
        decisionUsageMetadata_ = std::move(metadata).Value();
        decisionAuthoringGeneration_ = generation.Value();
        return Result<void>::Success();
    }

    /** @copydoc ProjectGameplayRegistry::ReplacePackageDecisionNodeSource */
    Result<void> ProjectGameplayRegistry::ReplacePackageDecisionNodeSource(std::shared_ptr<const ProjectPackageDecisionNodeSource> source) {
        if (source == packageDecisionNodes_)
            return Result<void>::Success();
        if (auto refreshed = RefreshDecisionAuthoringCatalog(source); refreshed.HasError())
            return refreshed;
        packageDecisionNodes_ = std::move(source);
        return Result<void>::Success();
    }

    /** @copydoc ProjectGameplayRegistry::ReloadChangedDecisionSources */
    void ProjectGameplayRegistry::ReloadChangedDecisionSources(std::vector<ProjectGameplayDiagnostic> &diagnostics) {
        for (auto &entry : luaDecisionPrograms_) {
            std::error_code error;
            const auto observed = ReadLuaSourceStat(entry.source, error);
            if (error) {
                diagnostics.emplace_back(entry.source, MakeError(Assets::AssetSourceErrors::ReadFailed, error.message()));
                continue;
            }
            if (entry.observation == observed)
                continue;
            entry.observation = observed;
            const auto updated = [&]() -> Result<void> {
                const auto identity = ReadDecisionScriptIdentity(projectRoot_, entry.source);
                if (identity.HasError())
                    return Result<void>::Failure(identity.ErrorValue());
                const auto previous = entry.program->Acquire();
                if (previous.HasError())
                    return Result<void>::Failure(previous.ErrorValue());
                const auto &descriptor = previous.Value()->Contracts().front().descriptor;
                if (!identity.Value() || identity.Value()->type != descriptor.type ||
                    identity.Value()->provider != descriptor.origin.provider)
                    return Result<void>::Failure(MakeError(AI::AIErrors::DecisionAssetSchemaInvalid,
                                                           "Decision script reload requires the original type and provider."));
                auto candidate = LoadDecisionScript(projectRoot_, entry.source, *identity.Value());
                if (candidate.HasError())
                    return Result<void>::Failure(candidate.ErrorValue());
                if (ReadLuaSourceStat(entry.source, error) != observed || error)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
                auto prior = std::exchange(entry.program, std::move(candidate).Value());
                if (const auto refreshed = RefreshDecisionAuthoringCatalog(packageDecisionNodes_); refreshed.HasError()) {
                    entry.program = std::move(prior);
                    return refreshed;
                }
                prior->Retire();
                return Result<void>::Success();
            }();
            if (updated.HasError())
                diagnostics.emplace_back(entry.source, updated.ErrorValue());
        }
    }

    /** @copydoc ProjectGameplayRegistry::HasBlockingDiagnostics */
    bool ProjectGameplayRegistry::HasBlockingDiagnostics() const noexcept {
        return !diagnostics_.empty() || replicationError_.has_value();
    }

    /** @copydoc ProjectGameplayRegistry::ReloadChangedLuaSources */
    std::vector<ProjectGameplayDiagnostic> ProjectGameplayRegistry::ReloadChangedLuaSources() {
        std::vector<ProjectGameplayDiagnostic> diagnostics;
        if (auto refreshed = RefreshReplicationSource(); refreshed.HasError())
            diagnostics.emplace_back(projectRoot_ / ".horo/project.json", refreshed.ErrorValue());
        for (const std::size_t index : std::views::iota(std::size_t{0}, luaPrograms_.size())) {
            std::error_code error;
            const LuaSourceStat sourceStat = ReadLuaSourceStat(luaSources_[index], error);
            if (error) {
                diagnostics.emplace_back(luaSources_[index],
                                         MakeError(Gameplay::GameplayErrors::InvalidBehaviorComponent, error.message()));
                continue;
            }
            if (sourceStat == luaSourceStats_[index])
                continue;
            luaSourceStats_[index] = sourceStat;
            auto candidate = Gameplay::LuaBehaviorProgram::LoadFiles(luaSources_[index], LuaMetadataPath(luaSources_[index]));
            if (candidate.HasError()) {
                diagnostics.emplace_back(luaSources_[index], candidate.ErrorValue());
                continue;
            }
            Result<void> replaced = ReplaceLuaGeneration(index, std::move(candidate).Value());
            if (replaced.HasError())
                diagnostics.emplace_back(luaSources_[index], replaced.ErrorValue());
        }
        ReloadChangedDecisionSources(diagnostics);
        return diagnostics;
    }

    /** @copydoc ProjectGameplayRegistry::ConsumeNativeArtifactChange */
    bool ProjectGameplayRegistry::ConsumeNativeArtifactChange() {
        std::error_code error;
        const bool exists = std::filesystem::exists(nativeManifestPath_, error);
        if (error)
            return false;
        if (!exists) {
            const bool changed = nativeManifestWriteTime_.has_value();
            nativeManifestWriteTime_.reset();
            return changed;
        }
        const auto writeTime = std::filesystem::last_write_time(nativeManifestPath_, error);
        if (error || (nativeManifestWriteTime_.has_value() && *nativeManifestWriteTime_ == writeTime))
            return false;
        nativeManifestWriteTime_ = writeTime;
        return true;
    }

    /** @copydoc ProjectGameplayRegistry::PreserveNativeArtifactForRollback */
    Result<NativeGameplayRollbackArtifact> ProjectGameplayRegistry::PreserveNativeArtifactForRollback(
        const std::filesystem::path &destination) const {
        if (!nativeModule_ || nativeModuleId_.empty() || nativeDescriptorRevision_ == 0 || !destination.is_absolute())
            return Result<NativeGameplayRollbackArtifact>::Failure(ManifestError("no active native generation can be preserved."));
        std::error_code error;
        std::filesystem::create_directories(destination, error);
        if (error)
            return Result<NativeGameplayRollbackArtifact>::Failure(ManifestError(error.message()));
        auto filename = std::filesystem::path{std::format("rollback-{}-", nativeDescriptorRevision_)};
        filename += nativeModule_->LoadedArtifactPath().filename();
        const std::filesystem::path preserved = destination / filename;
        std::filesystem::copy_file(nativeModule_->LoadedArtifactPath(), preserved, std::filesystem::copy_options::overwrite_existing,
                                   error);
        if (error)
            return Result<NativeGameplayRollbackArtifact>::Failure(ManifestError(error.message()));
        return Result<NativeGameplayRollbackArtifact>::Success(
            NativeGameplayRollbackArtifact{preserved, nativeModuleId_, nativeDescriptorRevision_});
    }

    /** @copydoc ProjectGameplayRegistry::CaptureLuaGeneration */
    Result<ProjectLuaGenerationSnapshot> ProjectGameplayRegistry::CaptureLuaGeneration() const {
        ProjectLuaGenerationSnapshot snapshot;
        snapshot.decisions = luaDecisionPrograms_;
        snapshot.programs.reserve(luaPrograms_.size());
        for (std::size_t index = 0; index < luaPrograms_.size(); ++index) {
            auto cloned = luaPrograms_[index]->Clone();
            if (cloned.HasError())
                return Result<ProjectLuaGenerationSnapshot>::Failure(cloned.ErrorValue());
            const LuaSourceStat &stat = luaSourceStats_[index];
            snapshot.programs.emplace_back(std::move(cloned).Value(), luaSources_[index], stat.sourceWriteTime, stat.metadataWriteTime,
                                           stat.sourceSize, stat.metadataSize);
        }
        return Result<ProjectLuaGenerationSnapshot>::Success(std::move(snapshot));
    }

    /** @copydoc ProjectGameplayRegistry::AcquireDecisionAuthoringGeneration */
    Result<std::shared_ptr<const AI::DecisionNodeGeneration>> ProjectGameplayRegistry::AcquireDecisionAuthoringGeneration() const {
        if (!decisionUsageMetadata_)
            return Result<std::shared_ptr<const AI::DecisionNodeGeneration>>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        if (auto current = decisionAuthoringGeneration_.lock(); current && current->CanAdmit())
            return Result<std::shared_ptr<const AI::DecisionNodeGeneration>>::Success(std::move(current));
        auto built = BuildDecisionAuthoringGeneration(packageDecisionNodes_);
        if (built.HasValue()) {
            if (!built.Value())
                return Result<std::shared_ptr<const AI::DecisionNodeGeneration>>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
            decisionAuthoringGeneration_ = built.Value();
        }
        return built;
    }

    /** @copydoc ProjectGameplayRegistry::AcquirePlayPerceptionGeneration */
    Result<std::shared_ptr<const AI::PerceptionSenseGeneration>> ProjectGameplayRegistry::AcquirePlayPerceptionGeneration() const {
        if (!nativeModule_)
            return Result<std::shared_ptr<const AI::PerceptionSenseGeneration>>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        return nativeModule_->AcquirePerceptionSenses();
    }

    /** @copydoc ProjectGameplayRegistry::DecisionUsageMetadata */
    const std::shared_ptr<const AI::DecisionBlackboardProviderMetadata> &ProjectGameplayRegistry::DecisionUsageMetadata() const noexcept {
        return decisionUsageMetadata_;
    }

    /** @copydoc ProjectGameplayRegistry::PrepareNativeReload */
    Result<Gameplay::GameModuleReloadSnapshot> ProjectGameplayRegistry::PrepareNativeReload() {
        if (!nativeModule_)
            return Result<Gameplay::GameModuleReloadSnapshot>::Failure(ManifestError("no active native generation is loaded."));
        auto prepared = nativeModule_->PrepareReload();
        if (prepared.HasValue()) {
            decisionUsageMetadata_.reset();
            decisionAuthoringGeneration_.reset();
        }
        return prepared;
    }

    /** @copydoc ProjectGameplayRegistry::RestoreNativeReload */
    Result<void> ProjectGameplayRegistry::RestoreNativeReload(const Gameplay::GameModuleReloadSnapshot &snapshot) {
        if (!nativeModule_)
            return Result<void>::Failure(ManifestError("no replacement native generation is loaded."));
        return nativeModule_->RestoreReload(snapshot);
    }

    /** @copydoc ProjectGameplayRegistry::HasNativeModule */
    bool ProjectGameplayRegistry::HasNativeModule() const noexcept {
        return nativeModule_ != nullptr;
    }
}  // namespace Horo::Editor
