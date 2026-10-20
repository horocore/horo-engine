#include "Horo/Application/NavigationProjectBakeSource.h"

#include "Horo/Application/ProjectAssetRegistrySource.h"
#include "Horo/Navigation/NavigationDefinitionSerialization.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <array>
#include <new>

namespace Horo::Application {
    namespace NavigationBakeDetail {
        /** @brief Exact source bytes, separate from process-local registry revisions and derived semantic cache keys. */
        struct ProjectFileEvidence final {
            std::string path;
            Sha256Digest digest;
            bool sidecar{};
        };

        /** @brief Detached source owner; immutable after Capture returns, including every retained capability and callback. */
        struct ProjectSourceState final {
            std::shared_ptr<const Platform::IProjectReadFiles> files;
            Platform::ProjectReadContext context;
            NavigationProjectSourceLimits limits;
            std::shared_ptr<const ProjectSourceDocument> project;
            std::shared_ptr<const NavigationProjectSettings> settings;
            Assets::AssetRegistrySnapshot registry;
            std::vector<NavigationSceneBakeSource> scenes;
            std::vector<NavigationDefinitionBakeSource> definitions;
            std::vector<NavigationMaterialBakeSource> materials;
            NavigationPackageBakeSource packages;
            std::vector<ProjectFileEvidence> evidence;
            std::size_t capturedBytes{};
        };
    }  // namespace NavigationBakeDetail

    namespace {
        using NavigationBakeDetail::ProjectSourceState;

        template <typename T> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(Navigation::NavigationErrors::BakeInputInvalid));
        }

        /** @brief Reduces the original invocation deadline for one complete capture or verification phase. */
        Platform::ProjectReadContext PhaseContext(const ProjectSourceState &state) {
            auto context = state.context;
            context.deadline = std::min(context.deadline, std::chrono::steady_clock::now() + state.limits.inventory.maximumDuration);
            return context;
        }

        /** @brief Retains Assets' complete canonical candidate and refuses degraded or ambiguous registries. */
        Result<Assets::AssetRegistrySnapshot> CaptureRegistry(const ProjectSourceState &state,
                                                              const Platform::ProjectReadContext &context) {
            auto source = CreateProjectAssetRegistrySource(state.files, context, state.limits.inventory);
            if (source.HasError())
                return Result<Assets::AssetRegistrySnapshot>::Failure(source.ErrorValue());
            auto candidate = Assets::PrepareAssetRegistryCandidate(*source.Value());
            if (candidate.HasError())
                return Result<Assets::AssetRegistrySnapshot>::Failure(candidate.ErrorValue());
            if (candidate.Value().status != Assets::AssetRegistryBuildStatus::Complete) {
                if (!candidate.Value().diagnostics.empty())
                    return Result<Assets::AssetRegistrySnapshot>::Failure(candidate.Value().diagnostics.front().error);
                return Invalid<Assets::AssetRegistrySnapshot>();
            }
            Assets::AssetRegistry registry;
            if (const auto published = registry.Publish(std::move(candidate).Value());
                published.status != Assets::AssetRegistryBuildStatus::Complete)
                return Invalid<Assets::AssetRegistrySnapshot>();
            return Result<Assets::AssetRegistrySnapshot>::Success(registry.Snapshot());
        }

        /** @brief Compares persistent registry identity rather than unrelated process-local publication counters. */
        bool SameRegistry(const Assets::AssetRegistrySnapshot &left, const Assets::AssetRegistrySnapshot &right) {
            return std::ranges::equal(left.Records(), right.Records(), [](const auto &a, const auto &b) {
                return a.id == b.id && a.type == b.type && a.sourcePath.String() == b.sourcePath.String() &&
                       a.metadataPath.String() == b.metadataPath.String();
            });
        }

        /** @brief Reads through the retained capability with no path-based native reopen or replacement of its authority. */
        Result<std::shared_ptr<const std::string>> Read(const ProjectSourceState &state, const Platform::ProjectReadContext &context,
                                                        const std::string_view path, const bool sidecar) {
            if (auto admitted = Platform::CheckProjectReadContext(context); admitted.HasError())
                return Result<std::shared_ptr<const std::string>>::Failure(admitted.ErrorValue());
            auto captured = sidecar ? state.files->InventoryText(path, context) : state.files->SourceText(path, context, {});
            if (captured.HasError())
                return Result<std::shared_ptr<const std::string>>::Failure(captured.ErrorValue());
            const auto &snapshot = captured.Value();
            if (!snapshot.text || snapshot.projectIdentity != context.projectIdentity ||
                snapshot.projectGeneration != context.projectGeneration || snapshot.path != path)
                return Result<std::shared_ptr<const std::string>>::Failure(MakeError(Platform::ProjectReadErrors::Stale));
            return Result<std::shared_ptr<const std::string>>::Success(snapshot.text);
        }

        /** @brief Charges exact raw source bytes before retaining digest evidence or decoding owned domain values. */
        Result<std::shared_ptr<const std::string>> CaptureFile(ProjectSourceState &state, const Platform::ProjectReadContext &context,
                                                               const std::string_view path, const bool sidecar = false) {
            auto bytes = Read(state, context, path, sidecar);
            if (bytes.HasError())
                return bytes;
            if (bytes.Value()->size() > state.limits.maximumCapturedBytes - state.capturedBytes)
                return Result<std::shared_ptr<const std::string>>::Failure(MakeError(Platform::ProjectReadErrors::Capacity));
            state.capturedBytes += bytes.Value()->size();
            state.evidence.emplace_back(std::string{path}, ComputeSha256(std::as_bytes(std::span{*bytes.Value()})), sidecar);
            return bytes;
        }

        /** @brief Observes optional package controls without interpreting their presence as package restore or trust. */
        Result<std::array<bool, 2>> PackagePresence(const ProjectSourceState &state, const Platform::ProjectReadContext &context) {
            auto inventory = state.files->Inventory(".horo", Platform::ProjectInventoryDepth::Children, context, state.limits.inventory);
            if (inventory.HasError())
                return Result<std::array<bool, 2>>::Failure(inventory.ErrorValue());
            std::array<bool, 2> result{};
            for (const auto &entry : inventory.Value().entries) {
                result[0] = result[0] || entry.path == ".horo/packages.json";
                result[1] = result[1] || entry.path == ".horo/packages.lock";
            }
            return Result<std::array<bool, 2>>::Success(result);
        }

        /** @brief Captures project authority through its sole codecs and matches the host's actual compatibility inspection. */
        Result<void> CapturePolicy(ProjectSourceState &state, const Platform::ProjectReadContext &context,
                                   const ProjectCompatibilitySnapshot &compatibility) {
            auto projectBytes = CaptureFile(state, context, ".horo/project.json");
            if (projectBytes.HasError())
                return Result<void>::Failure(projectBytes.ErrorValue());
            auto project = DecodeProjectSourceDocument(*projectBytes.Value());
            if (project.HasError())
                return Result<void>::Failure(project.ErrorValue());
            if (const auto &actual = project.Value().metadata; actual.projectId != compatibility.metadata->projectId ||
                                                               actual.horoVersion != compatibility.metadata->horoVersion ||
                                                               actual.persistentContract != compatibility.metadata->persistentContract)
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::BakeInputStale));
            state.project = std::make_shared<const ProjectSourceDocument>(std::move(project).Value());
            auto settingsBytes = CaptureFile(state, context, ".horo/navigation.json");
            if (settingsBytes.HasError())
                return Result<void>::Failure(settingsBytes.ErrorValue());
            auto settings = DecodeNavigationProjectSettings(*settingsBytes.Value());
            if (settings.HasError())
                return Result<void>::Failure(settings.ErrorValue());
            state.settings = std::make_shared<const NavigationProjectSettings>(std::move(settings).Value());
            auto packages = PackagePresence(state, context);
            if (packages.HasError())
                return Result<void>::Failure(packages.ErrorValue());
            constexpr std::array names{".horo/packages.json", ".horo/packages.lock"};
            const std::array destinations{&state.packages.intent, &state.packages.lock};
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (!packages.Value()[i])
                    continue;
                auto bytes = CaptureFile(state, context, names[i]);
                if (bytes.HasError())
                    return Result<void>::Failure(bytes.ErrorValue());
                *destinations[i] = *bytes.Value();
            }
            return Result<void>::Success();
        }

        /** @brief Uses only required NAVDEF01 source with no generated payload or provider-byte reinterpretation. */
        Result<void> CaptureDefinition(ProjectSourceState &state, const Assets::AssetRecord &record,
                                       const Platform::ProjectReadContext &context) {
            if (state.definitions.size() >= state.limits.maximumDefinitions)
                return Result<void>::Failure(MakeError(Platform::ProjectReadErrors::Capacity));
            auto bytes = CaptureFile(state, context, record.sourcePath.String());
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            const std::array support{Navigation::NavigationDefinitionRecordSupport()};
            auto records =
                Navigation::DeserializeNavigationSourceRecords(std::as_bytes(std::span{*bytes.Value()}), {.supportedRecords = support});
            if (records.HasError())
                return Result<void>::Failure(records.ErrorValue());
            if (records.Value().Records().size() != 1 || !records.Value().GeneratedPayloads().empty() ||
                records.Value().HasQuarantinedGeneratedPayloads())
                return Invalid<void>();
            auto definition = Navigation::DecodeNavigationDefinitionRecord(records.Value().Records().front());
            if (definition.HasError())
                return Result<void>::Failure(definition.ErrorValue());
            state.definitions.emplace_back(record.id, record.sourcePath, state.evidence.back().digest,
                                           std::make_shared<const Navigation::NavigationDefinition>(std::move(definition).Value()));
            return Result<void>::Success();
        }

        /** @brief Decodes fixed-field physical materials through their sole owner codec and exact sidecar identity. */
        Result<void> CaptureMaterial(ProjectSourceState &state, const Assets::AssetRecord &record,
                                     const Platform::ProjectReadContext &context) {
            if (state.materials.size() >= state.limits.maximumMaterials)
                return Result<void>::Failure(MakeError(Platform::ProjectReadErrors::Capacity));
            auto bytes = CaptureFile(state, context, record.sourcePath.String());
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            auto material = Physics::DecodePhysicsMaterialAsset(std::as_bytes(std::span{*bytes.Value()}));
            if (material.HasError())
                return Result<void>::Failure(material.ErrorValue());
            if (material.Value().Id().Asset() != record.id)
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::BakeInputInvalid,
                                                       "Navigation physical material payload differs from its canonical sidecar"));
            state.materials.emplace_back(record.id, record.sourcePath, state.evidence.back().digest, std::move(material).Value());
            return Result<void>::Success();
        }

        /** @brief Decodes one complete registered Scene under the shared byte/count/deadline budget. */
        Result<void> CaptureScene(ProjectSourceState &state, const Assets::AssetRecord &record,
                                  const Platform::ProjectReadContext &context) {
            if (state.scenes.size() >= state.limits.maximumScenes)
                return Result<void>::Failure(MakeError(Platform::ProjectReadErrors::Capacity));
            auto bytes = CaptureFile(state, context, record.sourcePath.String());
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            auto scene = SceneSource::DecodeSceneSource(*bytes.Value());
            if (scene.HasError())
                return Result<void>::Failure(scene.ErrorValue());
            state.scenes.emplace_back(record.id, record.sourcePath, state.evidence.back().digest, std::move(scene).Value());
            return Result<void>::Success();
        }

        /** @brief Captures every registry Scene/definition while independently checking exact canonical identity sidecars. */
        Result<void> CaptureAssets(ProjectSourceState &state, const Platform::ProjectReadContext &context) {
            for (const auto &record : state.registry.Records()) {
                auto metadata = CaptureFile(state, context, record.metadataPath.String(), true);
                if (metadata.HasError())
                    return Result<void>::Failure(metadata.ErrorValue());
                const auto identity = Assets::DecodeAssetIdentitySidecar(record.sourcePath.String(), *metadata.Value());
                if (identity.HasError())
                    return Result<void>::Failure(identity.ErrorValue());
                if (identity.Value().id != record.id || identity.Value().type != record.type)
                    return Result<void>::Failure(MakeError(Navigation::NavigationErrors::BakeInputStale));
                if (record.type.Value() == "core.navmesh") {
                    if (auto captured = CaptureDefinition(state, record, context); captured.HasError())
                        return captured;
                } else if (record.type.Value() == Physics::PhysicsMaterialAssetTypeName) {
                    if (auto captured = CaptureMaterial(state, record, context); captured.HasError())
                        return captured;
                } else if (record.type.Value() == "core.scene") {
                    if (auto captured = CaptureScene(state, record, context); captured.HasError())
                        return captured;
                }
            }
            if (const auto *definition = state.registry.Find(state.settings->Input().definition);
                !definition || definition->type.Value() != "core.navmesh")
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::BakeInputReferenceMissing));
            return Result<void>::Success();
        }

        /** @brief Rechecks one complete source closure under one unchanged phase deadline. */
        Result<void> VerifySource(const ProjectSourceState &state, const Platform::ProjectReadContext &context) {
            auto registry = CaptureRegistry(state, context);
            if (registry.HasError())
                return Result<void>::Failure(registry.ErrorValue());
            if (!SameRegistry(state.registry, registry.Value()))
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::BakeInputStale));
            auto packages = PackagePresence(state, context);
            if (packages.HasError())
                return Result<void>::Failure(packages.ErrorValue());
            if (packages.Value() != std::array{state.packages.intent.has_value(), state.packages.lock.has_value()})
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::BakeInputStale));
            for (const auto &evidence : state.evidence) {
                auto current = Read(state, context, evidence.path, evidence.sidecar);
                if (current.HasError())
                    return Result<void>::Failure(current.ErrorValue());
                if (ComputeSha256(std::as_bytes(std::span{*current.Value()})) != evidence.digest)
                    return Result<void>::Failure(MakeError(Navigation::NavigationErrors::BakeInputStale));
            }
            return Platform::CheckProjectReadContext(context);
        }
    }  // namespace

    /** @copydoc NavigationProjectBakeSource::NavigationProjectBakeSource */
    NavigationProjectBakeSource::NavigationProjectBakeSource(std::shared_ptr<ProjectSourceState> state) : state_(std::move(state)) {}

    /** @copydoc NavigationProjectBakeSource::Capture */
    Result<std::shared_ptr<const NavigationProjectBakeSource>> NavigationProjectBakeSource::Capture(
        std::shared_ptr<const Platform::IProjectReadFiles> files, Platform::ProjectReadContext context,
        const ProjectCompatibilitySnapshot &compatibility, const NavigationProjectSourceLimits &limits) {
        using Return = Result<std::shared_ptr<const NavigationProjectBakeSource>>;
        if (!files || !compatibility.metadata || compatibility.markerUpdateRequired ||
            (compatibility.status != ProjectCompatibilityStatus::Current &&
             compatibility.status != ProjectCompatibilityStatus::CompatibleReleaseLine))
            return compatibility.diagnostic ? Return::Failure(*compatibility.diagnostic)
                                            : Invalid<std::shared_ptr<const NavigationProjectBakeSource>>();
        if (const NavigationProjectSourceLimits hard;
            limits.maximumScenes == 0 || limits.maximumScenes > hard.maximumScenes || limits.maximumDefinitions == 0 ||
            limits.maximumDefinitions > hard.maximumDefinitions || limits.maximumMaterials == 0 ||
            limits.maximumMaterials > hard.maximumMaterials || limits.maximumCapturedBytes == 0 ||
            limits.maximumCapturedBytes > hard.maximumCapturedBytes || limits.inventory.maximumEntries == 0 ||
            limits.inventory.maximumEntries > hard.inventory.maximumEntries || limits.inventory.maximumManifestBytes == 0 ||
            limits.inventory.maximumManifestBytes > hard.inventory.maximumManifestBytes || limits.inventory.maximumDuration.count() <= 0 ||
            limits.inventory.maximumDuration > hard.inventory.maximumDuration)
            return Invalid<std::shared_ptr<const NavigationProjectBakeSource>>();
        try {
            auto state = std::make_shared<ProjectSourceState>();
            state->files = std::move(files);
            state->context = std::move(context);
            state->limits = limits;
            const auto phase = PhaseContext(*state);
            auto registry = CaptureRegistry(*state, phase);
            if (registry.HasError())
                return Return::Failure(registry.ErrorValue());
            state->registry = std::move(registry).Value();
            if (auto policy = CapturePolicy(*state, phase, compatibility); policy.HasError())
                return Return::Failure(policy.ErrorValue());
            if (auto assets = CaptureAssets(*state, phase); assets.HasError())
                return Return::Failure(assets.ErrorValue());
            if (auto current = VerifySource(*state, phase); current.HasError())
                return Return::Failure(current.ErrorValue());
            return Return::Success(std::make_shared<const NavigationProjectBakeSource>(NavigationProjectBakeSource{std::move(state)}));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(Platform::ProjectReadErrors::Capacity));
        }
    }

    /** @copydoc NavigationProjectBakeSource::VerifyCurrent */
    Result<void> NavigationProjectBakeSource::VerifyCurrent() const {
        try {
            return VerifySource(*state_, PhaseContext(*state_));
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Platform::ProjectReadErrors::Capacity));
        }
    }

    /** @copydoc NavigationProjectBakeSource::ValidateCurrent */
    Result<void> NavigationProjectBakeSource::ValidateCurrent(const CancellationToken &cancellation) const {
        try {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(
                    WrapError(Navigation::NavigationErrors::BakeInputCancelled, MakeError(Platform::ProjectReadErrors::Cancelled)));
            auto context = PhaseContext(*state_);
            context.authorityStopped = [retainedStop = std::move(context.authorityStopped), cancellation] {
                return cancellation.IsCancellationRequested() || (retainedStop && retainedStop());
            };
            auto verified = VerifySource(*state_, context);
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(
                    WrapError(Navigation::NavigationErrors::BakeInputCancelled, MakeError(Platform::ProjectReadErrors::Cancelled)));
            if (verified.HasError() && ErrorChainContains(verified.ErrorValue(), Platform::ProjectReadErrors::Cancelled.domain,
                                                          Platform::ProjectReadErrors::Cancelled.code))
                return Result<void>::Failure(WrapError(Navigation::NavigationErrors::BakeInputCancelled, std::move(verified).ErrorValue()));
            return verified;
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Platform::ProjectReadErrors::Capacity));
        }
    }

    /** @copydoc NavigationProjectBakeSource::Project */
    const ProjectSourceDocument &NavigationProjectBakeSource::Project() const noexcept {
        return *state_->project;
    }

    /** @copydoc NavigationProjectBakeSource::Settings */
    const NavigationProjectSettings &NavigationProjectBakeSource::Settings() const noexcept {
        return *state_->settings;
    }

    /** @copydoc NavigationProjectBakeSource::Registry */
    const Assets::AssetRegistrySnapshot &NavigationProjectBakeSource::Registry() const noexcept {
        return state_->registry;
    }

    /** @copydoc NavigationProjectBakeSource::Scenes */
    std::span<const NavigationSceneBakeSource> NavigationProjectBakeSource::Scenes() const noexcept {
        return state_->scenes;
    }

    /** @copydoc NavigationProjectBakeSource::Definitions */
    std::span<const NavigationDefinitionBakeSource> NavigationProjectBakeSource::Definitions() const noexcept {
        return state_->definitions;
    }

    /** @copydoc NavigationProjectBakeSource::Materials */
    std::span<const NavigationMaterialBakeSource> NavigationProjectBakeSource::Materials() const noexcept {
        return state_->materials;
    }

    /** @copydoc NavigationProjectBakeSource::Packages */
    const NavigationPackageBakeSource &NavigationProjectBakeSource::Packages() const noexcept {
        return state_->packages;
    }
}  // namespace Horo::Application
