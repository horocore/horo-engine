#include "Horo/Prefab/PrefabAssetDependencyClosure.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabSourceResolver.h"
#include "Horo/Runtime/Ui/UiDocument.h"
#include "Horo/Scene/SceneRuntimeConversion.h"
#include "PrefabCookBuiltinProjection.h"
#include "PrefabCookSemanticInputs.h"
#include "PrefabSceneCookState.h"
#include "UiDocumentCooker.h"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <new>
#include <set>

namespace Horo::Application::PrefabCookDetail {
    namespace {
        /** @brief Length-delimits registered semantic values without paths or mutable catalog publication counters. */
        void Append(std::string &value, const std::string_view field) {
            value += std::to_string(field.size());
            value += ':';
            value += field;
        }

        /** @brief Derives a nonzero compact runtime identity; the host checks containing-scene collisions before publication. */
        std::uint64_t Compact(const Sha256Digest &digest) {
            std::uint64_t value{};
            for (std::size_t index = 0; index < 8; ++index)
                value |= static_cast<std::uint64_t>(digest.bytes[index]) << (8U * index);
            return value == 0 ? 1 : value;
        }

        /** @brief Parses every captured prefab and commits its canonical revision before any resolver is built. */
        /** @brief Validates actual registered component/behavior payloads independently of override permission metadata. */
        Result<void> AdmitPrefabObjects(const Prefab::PrefabDocument &document,
                                        const std::shared_ptr<const PrefabCookSchemaContext> &schemas) {
            for (const auto &object : document.Data().objects) {
                std::vector<Gameplay::SerializedComponent> components;
                CookBuiltinProjection builtins;
                for (const auto &component : object.components)
                    if (auto projected = builtins.Collect(component, components); projected.HasError())
                        return projected;
                if ((!schemas && (!components.empty() || !object.behaviors.empty())) ||
                    (schemas && schemas->Validate(components, object.behaviors).HasError()))
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            }
            return Result<void>::Success();
        }

        /** @brief Captures canonical semantic source identity while preserving raw input digests separately in cook provenance. */
        Result<Prefab::PrefabDependencySource> CapturePrefabSource(const Assets::AssetCookPinnedSource &input, const HostCapture &host,
                                                                   const Prefab::PrefabLimitProfile &limits,
                                                                   const std::shared_ptr<const PrefabCookSchemaContext> &schemas,
                                                                   const Prefab::PrefabMetricHandles &metrics) {
            using Outcome = Result<Prefab::PrefabDependencySource>;
            const std::string_view bytes{reinterpret_cast<const char *>(input.bytes.data()), input.bytes.size()};
            auto document = Prefab::PrefabDocument::Parse(bytes, limits,
                                                          {.expectedProjectVersion = host.metadata.horoVersion.value, .metrics = &metrics});
            if (document.HasError())
                return Outcome::Failure(document.ErrorValue());
            auto canonical = document.Value().MatchesCanonicalInput(bytes);
            if (canonical.HasError())
                return Outcome::Failure(canonical.ErrorValue());
            if (document.Value().Data().assetId != input.record.id || !canonical.Value())
                return Outcome::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            if (auto admitted = AdmitPrefabObjects(document.Value(), schemas); admitted.HasError())
                return Outcome::Failure(admitted.ErrorValue());
            auto encoded = document.Value().SerializeCanonical();
            if (encoded.HasError())
                return Outcome::Failure(encoded.ErrorValue());
            const auto semantic = ComputeSha256(std::as_bytes(std::span{encoded.Value()}));
            return Outcome::Success({std::move(document).Value(), {host.metadata.horoVersion.value, semantic}});
        }

        /** @brief Admits the actual captured prefab inventory without executing providers or expanding incomplete sources. */
        Result<std::vector<Prefab::PrefabDependencySource>> PrefabSources(const Assets::AssetCookInputSnapshot &inputs,
                                                                          const HostCapture &host, const Prefab::PrefabLimitProfile &limits,
                                                                          const CancellationToken &cancellation,
                                                                          const std::shared_ptr<const PrefabCookSchemaContext> &schemas,
                                                                          const Prefab::PrefabMetricHandles &metrics,
                                                                          std::optional<Assets::AssetRecord> &failedSource) {
            std::vector<Prefab::PrefabDependencySource> sources;
            for (const auto &input : inputs.Sources()) {
                if (cancellation.IsCancellationRequested())
                    return Result<std::vector<Prefab::PrefabDependencySource>>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                if (input.record.type.Value() != "core.prefab")
                    continue;
                auto source = CapturePrefabSource(input, host, limits, schemas, metrics);
                if (source.HasError()) {
                    failedSource = input.record;
                    return Result<std::vector<Prefab::PrefabDependencySource>>::Failure(source.ErrorValue());
                }
                sources.push_back(std::move(source).Value());
            }
            return Result<std::vector<Prefab::PrefabDependencySource>>::Success(std::move(sources));
        }

        /** @brief Immutable, fully validated domain output; no raw source or mutable resolver escapes the host operation. */
        struct PreparedScene final {
            Assets::AssetId asset;
            Sha256Digest sourceDigest;
            Runtime::SceneDefinitionId id;
            Runtime::SceneDefinitionRevision revision;
            std::vector<std::uint8_t> payload;
            std::vector<Assets::AssetId> dependencies;
            Assets::AssetCookKeySelection key;
            Sha256Digest keyDigest;
            Sha256Digest payloadDigest;
        };

        /** @brief Exact authored occurrence paired with a dependency already prepared from the same pinned snapshot. */
        struct PreparedChildScene final {
            Runtime::SceneChildDefinitionReference reference;
            const PreparedScene *source{};
        };

        /** @brief Pure strategy over exact prepared outputs; Assets owns envelope construction and publication. */
        class SceneStrategy final : public Assets::ICookerStrategy {
        public:
            SceneStrategy(std::vector<PreparedScene> scenes, const Sha256Digest &settings, const SceneCook::CookedSceneLimits limits)
                : scenes_(std::move(scenes)), settings_(settings), limits_(limits) {}

            Assets::CookerCacheIdentity CacheIdentity() const noexcept override {
                return {.version = "horo.scene.prefab-inline.v3", .settingsDigest = settings_, .settingsSchemaVersion = 1};
            }

            Result<Assets::CookOutputSink> Cook(const Assets::CookSourceView &source,
                                                const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<Assets::CookOutputSink>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                const auto *scene = Find(source);
                if (!scene)
                    return Result<Assets::CookOutputSink>::Failure(MakeError(PrefabSceneCookErrors::Stale));
                return Result<Assets::CookOutputSink>::Success({scene->payload, scene->dependencies, {}});
            }

            Result<void> ValidateCookedPayload(const Assets::CookSourceView &source,
                                               const std::span<const std::uint8_t> payload) const override {
                const auto *scene = Find(source);
                if (!scene)
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Stale));
                if (const auto decoded = SceneCook::DecodeCookedSceneDefinition(payload, scene->id, scene->revision, limits_);
                    decoded.HasError())
                    return Result<void>::Failure(decoded.ErrorValue());
                // A valid but different payload under the requested full key is not a usable cache hit.
                if (!std::ranges::equal(payload, scene->payload))
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                return Result<void>::Success();
            }

        private:
            /** @brief Requires exact admitted source identity for both fresh output and cached domain validation. */
            const PreparedScene *Find(const Assets::CookSourceView &source) const {
                const auto found = std::ranges::find(scenes_, source.id, &PreparedScene::asset);
                return found != scenes_.end() && found->sourceDigest == source.sourceDigest ? std::to_address(found) : nullptr;
            }

            const std::vector<PreparedScene> scenes_;
            const Sha256Digest settings_;
            const SceneCook::CookedSceneLimits limits_;
        };

        /** @brief Preserves registered resource strategies while adding actual host semantics to every derived cache key. */
        class HostKeyStrategy final : public Assets::ICookerStrategy {
        public:
            HostKeyStrategy(std::shared_ptr<const Assets::ICookerStrategy> strategy, const Sha256Digest &host,
                            std::optional<SceneSource::TriggerVolumeConversionContext> triggerBinding)
                : strategy_(std::move(strategy)), identity_(strategy_->CacheIdentity()), triggerBinding_(std::move(triggerBinding)) {
                std::string preimage{"horo.host.cooker-input.v1"};
                Append(preimage, FormatSha256(host));
                Append(preimage, FormatSha256(identity_.settingsDigest));
                Append(preimage, std::to_string(identity_.settingsSchemaVersion));
                identity_.settingsDigest = ComputeSha256(std::as_bytes(std::span{preimage}));
                identity_.settingsSchemaVersion = 1;
            }

            Assets::CookerCacheIdentity CacheIdentity() const noexcept override {
                return identity_;
            }

            Result<std::optional<std::vector<Assets::AssetId>>> InputDependencies(const Assets::CookSourceView &source,
                                                                                  const CancellationToken &cancellation) const override {
                return strategy_->InputDependencies(source, cancellation);
            }

            Result<Assets::CookOutputSink> Cook(const Assets::CookSourceView &source,
                                                const CancellationToken &cancellation) const override {
                auto output = strategy_->Cook(source, cancellation);
                if (output.HasError())
                    return output;
                if (const auto valid = ValidateBinding(source, output.Value().payload); valid.HasError())
                    return Result<Assets::CookOutputSink>::Failure(valid.ErrorValue());
                return output;
            }

            Result<void> ValidateCookedPayload(const Assets::CookSourceView &source,
                                               const std::span<const std::uint8_t> payload) const override {
                if (const auto valid = ValidateBinding(source, payload); valid.HasError())
                    return valid;
                return strategy_->ValidateCookedPayload(source, payload);
            }

        private:
            /** @brief Validates the actual selected material source and cooked/cache bytes before generation publication. */
            Result<void> ValidateBinding(const Assets::CookSourceView &source, const std::span<const std::uint8_t> payload) const {
                if (triggerBinding_ && source.id == triggerBinding_->binding.material.id &&
                    (source.type != triggerBinding_->binding.material.expectedType ||
                     source.sourceDigest != triggerBinding_->materialSourceDigest ||
                     ComputeSha256(std::as_bytes(payload)) != triggerBinding_->binding.materialPayloadDigest))
                    return Result<void>::Failure(
                        MakeError(PrefabSceneCookErrors::Stale,
                                  "The real project material source or cooked payload differs from the trigger binding capture."));
                return Result<void>::Success();
            }

            const std::shared_ptr<const Assets::ICookerStrategy> strategy_;
            Assets::CookerCacheIdentity identity_;
            const std::optional<SceneSource::TriggerVolumeConversionContext> triggerBinding_;
        };

        /** @brief Admits retained project schemas and rejects unsupported required mesh projections before conversion. */
        Result<void> AdmitSceneObjects(const std::span<const SceneSource::SceneObjectSnapshot> objects,
                                       const std::shared_ptr<const PrefabCookSchemaContext> &schemas) {
            for (const auto &object : objects) {
                if ((!schemas && (!object.components.gameplayComponents.empty() || !object.components.behaviors.empty())) ||
                    (schemas && schemas->Validate(object.components.gameplayComponents, object.components.behaviors).HasError()))
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                // RuntimeEntityDefinition has no imported-mesh field: never silently drop a required authored asset reference.
                if (object.meshAsset)
                    return Result<void>::Failure(MakeError(SceneCook::SceneCookErrors::Unsupported));
            }
            return Result<void>::Success();
        }

        /** @brief Selects the authored prefab roots whose transitive assets belong in a cooked scene. */
        std::vector<Assets::AssetId> PrefabRoots(const SceneSource::SceneSourceView &source) {
            std::vector<Assets::AssetId> roots;
            for (const auto &placement : source.prefabInstances)
                roots.push_back(placement.sourcePrefab.Asset());
            return roots;
        }

        /** @brief Coherent host operation context borrowed only through the synchronous joined preparation. */
        struct ScenePreparationContext final {
            const PrefabSceneCookRequest &request;
            const Sha256Digest &settings;
            const Assets::AssetCookInputSnapshot &inputs;
            const Assets::CookerCatalogSnapshot &catalog;
            const CancellationToken &cancellation;
            SceneSource::ScenePrefabExpansionOwner &expansion;
        };

        /** @brief Submits and joins one detached Scene attempt; a failed join drains ownership before returning. */
        Result<Runtime::RuntimeSceneDefinition> ExpandScene(const SceneSource::SceneSourceDocument &source,
                                                            const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                            const Prefab::PrefabLimitProfile &limits,
                                                            const ScenePreparationContext &context, const Runtime::SceneDefinitionId id,
                                                            const Runtime::SceneDefinitionRevision revision) {
            try {
                SceneSource::ScenePrefabExpansionRequest request{.documentSession = id.value,
                                                                 .scene = id,
                                                                 .revision = revision,
                                                                 // Prefab expansion owns objects; this catalog binds children separately.
                                                                 .document = {source.objects, source.prefabInstances,
                                                                              source.unsupportedComponentOwners},
                                                                 .resolver = resolver,
                                                                 .limits = limits,
                                                                 .valueSchemas = context.request.overrideSchemas
                                                                                     ? std::optional{*context.request.overrideSchemas}
                                                                                     : std::nullopt,
                                                                 .triggerBinding = context.request.triggerBinding};
                if (const auto admitted = context.expansion.Submit(request, context.cancellation); admitted.HasError())
                    return Result<Runtime::RuntimeSceneDefinition>::Failure(admitted.ErrorValue());
                const JoinOptions options{WaitPolicy::MainThreadPumpAllowed, Duration::FromMilliseconds(30'000)};
                if (const auto joined = context.expansion.Join(options); joined.HasError()) {
                    const auto drained = context.expansion.ReplaceScene(options);
                    return Result<Runtime::RuntimeSceneDefinition>::Failure(drained.HasError() ? drained.ErrorValue()
                                                                                               : joined.ErrorValue());
                }
                auto completed = context.expansion.TakeCompleted(request);
                if (completed.HasError())
                    return Result<Runtime::RuntimeSceneDefinition>::Failure(completed.ErrorValue());
                if (!completed.Value())
                    return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(PrefabSceneCookErrors::Stale));
                return Result<Runtime::RuntimeSceneDefinition>::Success(*std::move(completed).Value());
            } catch (const std::bad_alloc &) {
                return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheAllocationFailed));
            }
        }

        /** @brief Validates the same typed Scene requirements against the pinned registry even when no Prefab is authored. */
        Result<std::vector<Runtime::SceneAssetDependency>> SceneDependencies(const Runtime::RuntimeSceneDefinition &definition,
                                                                             const Assets::AssetRegistrySnapshot &registry,
                                                                             const Prefab::PrefabDependencyGraphSnapshot &graph,
                                                                             const std::span<const Assets::AssetId> roots,
                                                                             const std::size_t maximumDependencies) {
            if (!roots.empty()) {
                std::vector<Prefab::PrefabAssetDependency> existing;
                for (const auto &dependency : definition.AssetDependencies())
                    existing.push_back({dependency.id, dependency.expectedType, {}});
                auto closure = Prefab::BuildPrefabAssetDependencyClosure(registry, graph, roots, existing, maximumDependencies);
                if (closure.HasError())
                    return Result<std::vector<Runtime::SceneAssetDependency>>::Failure(closure.ErrorValue());
                const auto dependencies = closure.Value().RuntimeDependencies();
                return Result<std::vector<Runtime::SceneAssetDependency>>::Success({dependencies.begin(), dependencies.end()});
            }
            if (registry.Revision() != graph.RegistryRevision())
                return Result<std::vector<Runtime::SceneAssetDependency>>::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
            if (maximumDependencies == 0 || definition.AssetDependencies().size() > maximumDependencies)
                return Result<std::vector<Runtime::SceneAssetDependency>>::Failure(
                    MakeError(Prefab::PrefabErrors::DependencyClosureCapacityExceeded));
            for (const auto &dependency : definition.AssetDependencies()) {
                const auto *record = registry.Find(dependency.id);
                if (!record)
                    return Result<std::vector<Runtime::SceneAssetDependency>>::Failure(
                        MakeError(Prefab::PrefabErrors::DependencyUnavailable));
                if (record->type != dependency.expectedType)
                    return Result<std::vector<Runtime::SceneAssetDependency>>::Failure(MakeError(Prefab::PrefabErrors::DependencyConflict));
            }
            const auto dependencies = definition.AssetDependencies();
            return Result<std::vector<Runtime::SceneAssetDependency>>::Success({dependencies.begin(), dependencies.end()});
        }

        /** @brief Builds the complete source-free dependency closure and bounded cooked payload after Scene conversion. */
        Result<PreparedScene> FinishScene(const Assets::AssetCookPinnedSource &input, const Runtime::RuntimeSceneDefinition &definition,
                                          const Assets::AssetRegistrySnapshot &registry, const Prefab::PrefabDependencyGraphSnapshot &graph,
                                          const std::span<const Assets::AssetId> roots, const SceneCook::CookedSceneLimits &limits,
                                          const Runtime::SceneDefinitionRevision revision,
                                          const std::span<const PreparedChildScene> children) {
            if (definition.Entities().size() > limits.maximumEntities)
                return Result<PreparedScene>::Failure(MakeError(SceneCook::SceneCookErrors::TooLarge));
            auto closure = SceneDependencies(definition, registry, graph, roots, limits.maximumDependencies);
            if (closure.HasError())
                return Result<PreparedScene>::Failure(closure.ErrorValue());
            Runtime::SceneDefinitionBuilder builder{definition.Id(), revision};
            for (const auto &entity : definition.Entities())
                builder.Add(entity);
            PreparedScene prepared{input.record.id, input.sourceDigest, definition.Id(), revision, {}, {}, {}, {}, {}};
            for (const auto &dependency : closure.Value()) {
                if (auto admitted = builder.RequireAsset(dependency); admitted.HasError())
                    return Result<PreparedScene>::Failure(admitted.ErrorValue());
                prepared.dependencies.push_back(dependency.id);
            }
            for (const auto &child : children) {
                if (auto required = builder.RequireChildScene(child.reference); required.HasError())
                    return Result<PreparedScene>::Failure(required.ErrorValue());
                prepared.dependencies.push_back(child.reference.asset);
            }
            if (!children.empty()) {
                std::ranges::sort(prepared.dependencies);
                const auto duplicate = std::ranges::unique(prepared.dependencies);
                prepared.dependencies.erase(duplicate.begin(), duplicate.end());
            }
            for (const auto &evidence : definition.TriggerVolumeEvidence())
                if (auto required = builder.RequireTriggerVolume(evidence); required.HasError())
                    return Result<PreparedScene>::Failure(required.ErrorValue());
            auto complete = std::move(builder).Build();
            if (complete.HasError())
                return Result<PreparedScene>::Failure(complete.ErrorValue());
            auto encoded = SceneCook::EncodeCookedSceneDefinition(complete.Value(), limits);
            if (encoded.HasError())
                return Result<PreparedScene>::Failure(encoded.ErrorValue());
            prepared.payload = std::move(encoded).Value();
            prepared.payloadDigest = ComputeSha256(std::as_bytes(std::span{prepared.payload}));
            return Result<PreparedScene>::Success(std::move(prepared));
        }

        /** @brief Commits exact child source, provider and cook-key evidence into the parent's semantic selection. */
        Result<void> FoldChildCookInputs(Assets::AssetCookKeySelection &selection, const std::span<const PreparedChildScene> children,
                                         const std::size_t maximumInputs, const CancellationToken &cancellation) {
            if (children.empty())
                return Result<void>::Success();
            std::set<Assets::AssetId> inputs(selection.inputs.begin(), selection.inputs.end());
            std::string identity{"horo.scene.activated-child-inputs.v1"};
            Append(identity, FormatSha256(selection.producerSettings));
            for (const auto &child : children) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                Append(identity, std::to_string(child.reference.occurrence.value));
                Append(identity, child.reference.asset.ToString());
                Append(identity, FormatSha256(child.source->keyDigest));
                for (const auto id : child.source->key.inputs) {
                    inputs.insert(id);
                    if (inputs.size() > maximumInputs)
                        return Result<void>::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
                }
            }
            selection.inputs.assign(inputs.begin(), inputs.end());
            selection.producerSettings = ComputeSha256(std::as_bytes(std::span{identity}));
            return Result<void>::Success();
        }

        /** @brief Admits canonical authored values before invoking owned expansion and source-free cooking. */
        Result<PreparedScene> PrepareScene(const Assets::AssetCookPinnedSource &input, const SceneSource::SceneSourceDocument &source,
                                           const Assets::AssetRegistrySnapshot &registry,
                                           const Prefab::PrefabSourceResolverSnapshot &resolver,
                                           const Prefab::PrefabDependencyGraphSnapshot &graph, const Prefab::PrefabLimitProfile &limits,
                                           const ScenePreparationContext &context, const std::span<const PreparedChildScene> children) {
            const std::string_view bytes{reinterpret_cast<const char *>(input.bytes.data()), input.bytes.size()};
            const SceneSource::SceneSourceView view{source.objects, source.prefabInstances, source.childScenes};
            if (SceneSource::EncodeSceneSource(view) != bytes || source.objects.size() > context.request.sceneLimits.maximumEntities)
                return Result<PreparedScene>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            if (auto admitted = AdmitSceneObjects(source.objects, context.request.schemas); admitted.HasError())
                return Result<PreparedScene>::Failure(admitted.ErrorValue());
            const Runtime::SceneDefinitionId id = SceneCook::SceneDefinitionIdForAsset(input.record.id);
            const std::string revisionInput = FormatSha256(input.sourceDigest) + FormatSha256(context.settings);
            const Runtime::SceneDefinitionRevision revision{Compact(ComputeSha256(std::as_bytes(std::span{revisionInput})))};
            const auto definition = ExpandScene(source, resolver, limits, context, id, revision);
            if (definition.HasError())
                return Result<PreparedScene>::Failure(definition.ErrorValue());
            std::vector<Assets::AssetId> resources;
            for (const auto &dependency : definition.Value().AssetDependencies())
                resources.push_back(dependency.id);
            auto semantic = CaptureSemanticCookInputs(context.inputs, graph, resolver, input.record.id, context.request.schemas,
                                                      context.catalog, context.request.assets.target,
                                                      context.request.assets.limits.maximumAssets, context.cancellation, resources);
            if (semantic.HasError())
                return Result<PreparedScene>::Failure(semantic.ErrorValue());
            Assets::AssetCookKeySelection selection{input.record.id, semantic.Value().sources, semantic.Value().producerSettings};
            if (auto folded = FoldChildCookInputs(selection, children, context.request.assets.limits.maximumAssets, context.cancellation);
                folded.HasError())
                return Result<PreparedScene>::Failure(folded.ErrorValue());
            auto key = Assets::AssetCookKeyProjection::Create(context.inputs, {selection}, context.request.assets.limits.maximumAssets);
            if (key.HasError())
                return Result<PreparedScene>::Failure(key.ErrorValue());
            const auto digest = key.Value()->Digest(input.record.id);
            const std::string finalIdentity = FormatSha256(context.settings) + FormatSha256(digest);
            const Runtime::SceneDefinitionRevision finalRevision{Compact(ComputeSha256(std::as_bytes(std::span{finalIdentity})))};
            auto prepared = FinishScene(input, definition.Value(), registry, graph, PrefabRoots(view), context.request.sceneLimits,
                                        finalRevision, children);
            if (prepared.HasError())
                return prepared;
            auto value = std::move(prepared).Value();
            value.key = std::move(selection);
            value.keyDigest = digest;
            return Result<PreparedScene>::Success(std::move(value));
        }

        /** @brief Couples admitted resource strategies with the complete source, host and resource-settings commitment. */
        struct ResourceCatalogInputs final {
            std::vector<Assets::CookerContribution> contributions;
            Sha256Digest settings;
            std::shared_ptr<const Animation::PreparedAnimationCook> animation;
        };

        Result<std::shared_ptr<const Animation::PreparedAnimationCook>> PrepareAnimationInputs(const PrefabSceneCookRequest &request,
                                                                                               const Assets::AssetCookInputSnapshot &inputs,
                                                                                               const CancellationToken &cancellation) {
            using Return = Result<std::shared_ptr<const Animation::PreparedAnimationCook>>;
            if (const bool present = std::ranges::any_of(inputs.Sources(),
                                                         [](const auto &source) {
                const auto &type = source.record.type.Value();
                return type == "core.animation.skeleton" || type == "core.animation.clip" || type == "core.animation.graph";
            });
                !present)
                return Return::Success({});
            Animation::AnimationAssetCookLimits limits;
            limits.maximumAssets = std::min(limits.maximumAssets, request.assets.limits.maximumAssets);
            limits.maximumPreparedBytes = std::min(limits.maximumPreparedBytes, request.maximumCapturedBytes);
            limits.codec.bytes = std::min<std::uint64_t>(limits.codec.bytes, request.assets.limits.maximumArtifactBytes);
            limits.source.bytes = std::min(limits.source.bytes, request.assets.limits.maximumSourceBytes);
            auto prepared = Animation::PreparedAnimationCook::Create(inputs, request.assets.target, limits, cancellation);
            if (prepared.HasError())
                return Return::Failure(std::move(prepared).ErrorValue());
            return Return::Success(std::make_shared<const Animation::PreparedAnimationCook>(std::move(prepared).Value()));
        }

        Result<std::vector<Assets::CookerContribution>> CollectResourceContributions(
            const Assets::AssetCookInputSnapshot &inputs, const Assets::CookerCatalogSnapshot &catalog, const AssetCookTargetId &target,
            const CancellationToken &cancellation, std::vector<Assets::CookerContribution> contributions) {
            using Return = Result<std::vector<Assets::CookerContribution>>;
            for (const auto &input : inputs.Sources()) {
                if (cancellation.IsCancellationRequested())
                    return Return::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                if (input.record.type.Value() == "core.prefab" || input.record.type.Value() == "core.scene")
                    continue;
                if (std::ranges::find(contributions, input.record.type, &Assets::CookerContribution::assetType) != contributions.end())
                    continue;
                if (input.record.type.Value() == Runtime::Ui::UiDocumentAssetTypeName) {
                    contributions.push_back(MakeUiDocumentCooker(inputs.Registry(), target));
                    continue;
                }
                const auto *original = catalog.FindContribution(input.record.type, target);
                if (!original || !original->strategy)
                    return Return::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable,
                                                     std::format("No cooker for resource type '{}' on target '{}'.",
                                                                 input.record.type.Value(), target.Value())));
                contributions.push_back(*original);
            }
            return Return::Success(std::move(contributions));
        }

        /** @brief Captures every registered resource strategy identity before constructing scene or resource wrappers. */
        Result<ResourceCatalogInputs> PrepareResourceCatalogInputs(const PrefabSceneCookRequest &request, const HostCapture &host,
                                                                   const Assets::AssetCookInputSnapshot &inputs,
                                                                   const Assets::CookerCatalogSnapshot &catalog,
                                                                   const CancellationToken &cancellation) {
            std::string identity{"horo.static-scene-cook.resolver-v1.output-v1"};
            Append(identity, FormatSha256(host.cacheDigest));
            auto animation = PrepareAnimationInputs(request, inputs, cancellation);
            if (animation.HasError())
                return Result<ResourceCatalogInputs>::Failure(std::move(animation).ErrorValue());
            std::vector<Assets::CookerContribution> contributions;
            if (animation.Value())
                contributions = animation.Value()->Contributions();
            auto gathered = CollectResourceContributions(inputs, catalog, request.assets.target, cancellation, std::move(contributions));
            if (gathered.HasError())
                return Result<ResourceCatalogInputs>::Failure(std::move(gathered).ErrorValue());
            contributions = std::move(gathered).Value();
            const auto settings = ComputeSha256(std::as_bytes(std::span{identity}));
            for (auto &contribution : contributions)
                contribution.strategy = std::make_shared<HostKeyStrategy>(contribution.strategy, settings, request.triggerBinding);
            return Result<ResourceCatalogInputs>::Success({std::move(contributions), settings, animation.Value()});
        }

        /** @brief Parses assets through the bounded child-aware source intake. */
        Result<SceneSource::SceneSourceDocument> ReadCookScene(const Assets::AssetCookPinnedSource &input) {
            return SceneSource::DecodeSceneSourceWithChildReferences(
                {reinterpret_cast<const char *>(input.bytes.data()), input.bytes.size()});
        }

        /** @brief Joins direct children with pinned leaf artifacts, rejecting every nested or cyclic edge. */
        Result<std::vector<PreparedChildScene>> ResolveSceneChildren(const Assets::AssetId parent,
                                                                     const std::span<const SceneSource::SceneChildReference> references,
                                                                     const Assets::AssetCookInputSnapshot &inputs,
                                                                     const std::map<Assets::AssetId, std::size_t> &leaves,
                                                                     const std::span<const PreparedScene> prepared) {
            using Outcome = Result<std::vector<PreparedChildScene>>;
            std::vector<PreparedChildScene> children;
            children.reserve(references.size());
            for (const auto &reference : references) {
                const auto *pinned = inputs.Find(reference.asset);
                if (!pinned)
                    return Outcome::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable, reference.asset.ToString()));
                if (pinned->record.type.Value() != "core.scene")
                    return Outcome::Failure(MakeError(Prefab::PrefabErrors::DependencyConflict, reference.asset.ToString()));
                const auto found = leaves.find(reference.asset);
                if (reference.asset == parent || found == leaves.end())
                    return Outcome::Failure(
                        MakeError(PrefabSceneCookErrors::Invalid,
                                  "An Activated Scene child must be a distinct direct leaf. Nested or cyclic references cannot cook."));
                const auto &child = prepared[found->second];
                children.push_back(
                    {{reference.occurrence, child.asset, child.id, child.revision, child.payloadDigest, reference.mode}, &child});
            }
            std::ranges::sort(children, {}, [](const PreparedChildScene &child) {
                return child.reference.occurrence;
            });
            return Outcome::Success(std::move(children));
        }

        /** @brief Owns complete source-free scenes after aggregate payload and compact-identity collision admission. */
        Result<std::vector<PreparedScene>> PrepareScenes(const PrefabSceneCookRequest &request,
                                                         const Assets::AssetCookInputSnapshot &inputs,
                                                         const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                         const Prefab::PrefabDependencyGraphSnapshot &graph,
                                                         const Prefab::PrefabLimitProfile &limits, const ScenePreparationContext &context,
                                                         std::optional<Assets::AssetRecord> &failedSource) {
            using Outcome = Result<std::vector<PreparedScene>>;
            std::vector<PreparedScene> scenes;
            std::map<Assets::AssetId, std::size_t> leaves;
            std::set<Runtime::SceneDefinitionId> definitions;
            std::vector<const Assets::AssetCookPinnedSource *> parents;
            std::uint64_t totalPayloadBytes{};
            std::size_t totalInputReferences{};
            const auto prepare = [&](const Assets::AssetCookPinnedSource &input, const SceneSource::SceneSourceDocument &source,
                                     const std::span<const PreparedChildScene> children) -> Result<void> {
                auto scene = PrepareScene(input, source, inputs.Registry(), resolver, graph, limits, context, children);
                if (scene.HasError())
                    return Result<void>::Failure(scene.ErrorValue());
                const auto &value = scene.Value();
                if (value.payload.size() > request.maximumCapturedBytes - totalPayloadBytes || !definitions.insert(value.id).second)
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                if (value.key.inputs.size() > 1'048'576U - totalInputReferences)
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
                totalInputReferences += value.key.inputs.size();
                totalPayloadBytes += value.payload.size();
                scenes.push_back(std::move(scene).Value());
                return Result<void>::Success();
            };
            // A selected child is always a leaf. Prepare leaves before parents regardless of source order.
            for (const auto &input : inputs.Sources()) {
                if (context.cancellation.IsCancellationRequested())
                    return Outcome::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                if (input.record.type.Value() != "core.scene")
                    continue;
                const auto selected = ReadCookScene(input);
                if (selected.HasError()) {
                    failedSource = input.record;
                    return Outcome::Failure(selected.ErrorValue());
                }
                if (!selected.Value().childScenes.empty()) {
                    parents.push_back(&input);
                    continue;
                }
                const auto index = scenes.size();
                if (const auto staged = prepare(input, selected.Value(), {}); staged.HasError()) {
                    failedSource = input.record;
                    return Outcome::Failure(staged.ErrorValue());
                }
                leaves.emplace(input.record.id, index);
            }
            for (const auto *input : parents) {
                if (context.cancellation.IsCancellationRequested())
                    return Outcome::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                const auto selected = ReadCookScene(*input);
                if (selected.HasError()) {
                    failedSource = input->record;
                    return Outcome::Failure(selected.ErrorValue());
                }
                const auto children = ResolveSceneChildren(input->record.id, selected.Value().childScenes, inputs, leaves, scenes);
                if (children.HasError()) {
                    failedSource = input->record;
                    return Outcome::Failure(children.ErrorValue());
                }
                if (const auto staged = prepare(*input, selected.Value(), children.Value()); staged.HasError()) {
                    failedSource = input->record;
                    return Outcome::Failure(staged.ErrorValue());
                }
            }
            return Outcome::Success(std::move(scenes));
        }

        /** @brief Publishes prepared Scene/resource strategies as one isolated catalog registration transaction. */
        Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>> PublishContributions(ResourceCatalogInputs resources,
                                                                                          std::vector<PreparedScene> scenes,
                                                                                          const PrefabSceneCookRequest &request) {
            if (!scenes.empty())
                resources.contributions.push_back(
                    {"horo.builtin.scene_prefab_inline",
                     Assets::AssetTypeId::Parse("core.scene").Value(),
                     {request.assets.target},
                     std::make_shared<SceneStrategy>(std::move(scenes), resources.settings, request.sceneLimits)});
            Assets::CookerCatalog candidate;
            for (auto &contribution : resources.contributions) {
                if (auto registered = candidate.Register(std::move(contribution)); registered.HasError())
                    return Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>>::Failure(registered.ErrorValue());
            }
            return candidate.Publish();
        }

        /** @brief Attaches a fully admitted dependent-template phase to the prepared catalog. */
        Result<PreparedCatalog> AttachTemplatePhase(PreparedCatalog prepared, TemplateCookState state) {
            auto phase = PrepareTemplatePhase(std::move(state));
            if (phase.HasError())
                return Result<PreparedCatalog>::Failure(phase.ErrorValue());
            prepared.templates = std::move(phase).Value();
            return Result<PreparedCatalog>::Success(std::move(prepared));
        }

        /** @brief Excludes source-only prefabs and Scene roots already captured by their actual converted-resource closure. */
        bool SelectedResource(const Assets::AssetCookPinnedSource &input, const PrefabSceneCookRequest &request) {
            if (input.record.type.Value() == "core.scene")
                return false;
            return input.record.type.Value() != "core.prefab" ||
                   std::ranges::find(request.runtimePrefabRoots, input.record.id) != request.runtimePrefabRoots.end();
        }

        /** @brief Builds complete immutable cook keys from the resolver's one validated graph and actual converted Scene dependencies. */
        Result<std::shared_ptr<const Assets::AssetCookKeyProjection>> CaptureSelections(
            const PrefabSceneCookRequest &request, const Assets::AssetCookInputSnapshot &inputs,
            const Prefab::PrefabSourceResolverSnapshot &resolver, std::span<const PreparedScene> scenes,
            const Assets::CookerCatalogSnapshot &catalog, const CancellationToken &cancellation) {
            using Outcome = Result<std::shared_ptr<const Assets::AssetCookKeyProjection>>;
            std::vector<Assets::AssetCookKeySelection> selections;
            std::size_t total{};
            for (const auto &scene : scenes) {
                if (scene.key.inputs.size() > 1'048'576U - total)
                    return Outcome::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
                total += scene.key.inputs.size();
                selections.push_back(scene.key);
            }
            for (const auto &input : inputs.Sources()) {
                if (!SelectedResource(input, request))
                    continue;
                auto semantic =
                    CaptureSemanticCookInputs(inputs, resolver.DependencyGraph(), resolver, input.record.id, request.schemas, catalog,
                                              request.assets.target, request.assets.limits.maximumAssets, cancellation);
                if (semantic.HasError())
                    return Outcome::Failure(semantic.ErrorValue());
                if (semantic.Value().sources.size() > 1'048'576U - total)
                    return Outcome::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
                total += semantic.Value().sources.size();
                selections.emplace_back(input.record.id, semantic.Value().sources, semantic.Value().producerSettings);
            }
            return Assets::AssetCookKeyProjection::Create(inputs, std::move(selections), request.assets.limits.maximumAssets);
        }

    }  // namespace

    /** @copydoc PrepareCatalog */
    Result<PreparedCatalog> PrepareCatalog(const PrefabSceneCookRequest &request, const HostCapture &host,
                                           const Assets::AssetCookInputSnapshot &inputs, const Prefab::PrefabLimitProfile &limits,
                                           const Assets::CookerCatalogSnapshot &catalog, const CancellationToken &cancellation,
                                           SceneSource::ScenePrefabExpansionOwner &expansion,
                                           std::optional<Assets::AssetRecord> &failedSource) {
        auto sources = PrefabSources(inputs, host, limits, cancellation, request.schemas, request.metrics, failedSource);
        if (sources.HasError())
            return Result<PreparedCatalog>::Failure(sources.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<PreparedCatalog>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
        auto resolver = Prefab::BuildPrefabSourceResolverSnapshot(inputs.Registry(), std::move(sources).Value(), limits, request.metrics,
                                                                  request.overrideSchemas);
        if (resolver.HasError())
            return Result<PreparedCatalog>::Failure(resolver.ErrorValue());
        auto resources = PrepareResourceCatalogInputs(request, host, inputs, catalog, cancellation);
        if (resources.HasError())
            return Result<PreparedCatalog>::Failure(resources.ErrorValue());
        const auto settings = resources.Value().settings;
        const auto animation = resources.Value().animation;
        auto resourceCatalog = PublishContributions(resources.Value(), {}, request);
        if (resourceCatalog.HasError())
            return Result<PreparedCatalog>::Failure(std::move(resourceCatalog).ErrorValue());
        const auto &graph = resolver.Value().DependencyGraph();
        auto scenes = PrepareScenes(request, inputs, resolver.Value(), graph, limits,
                                    {request, settings, inputs, *resourceCatalog.Value(), cancellation, expansion}, failedSource);
        if (scenes.HasError())
            return Result<PreparedCatalog>::Failure(scenes.ErrorValue());
        auto projection = CaptureSelections(request, inputs, resolver.Value(), scenes.Value(), *resourceCatalog.Value(), cancellation);
        if (projection.HasError())
            return Result<PreparedCatalog>::Failure(projection.ErrorValue());
        auto published = PublishContributions(std::move(resources).Value(), std::move(scenes).Value(), request);
        if (published.HasError())
            return Result<PreparedCatalog>::Failure(published.ErrorValue());
        PreparedCatalog prepared{published.Value(), {}, std::move(projection).Value()};
        if (request.runtimePrefabRoots.empty())
            return Result<PreparedCatalog>::Success(std::move(prepared));
        auto templateGraph = graph;
        return AttachTemplatePhase(std::move(prepared), {std::move(resolver).Value(), std::move(templateGraph), inputs.Registry(),
                                                         request.runtimePrefabRoots, request.assets.target, limits, request.assets.limits,
                                                         settings, request.schemas, request.maximumCapturedBytes, animation, inputs});
    }
}  // namespace Horo::Application::PrefabCookDetail
