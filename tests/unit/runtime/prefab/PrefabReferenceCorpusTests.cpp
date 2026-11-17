#include "Horo/Prefab/PrefabDiagnostics.h"
#include "Horo/Prefab/PrefabSceneExpansion.h"
#include "Horo/Prefab/PrefabValueOverrides.h"
#include "Horo/Scene/SceneRuntimeConversion.h"
#include "PrefabTemplateProviderTestSupport.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>

namespace Horo::Prefab {
    namespace {
        using Json = nlohmann::json;

        /** @brief Reads only bounded committed corpus input; tests never modify the fixture tree. */
        std::string Read(const std::string &name) {
            const auto path = std::filesystem::path{HORO_PREFAB_CORPUS_DIR} / name;
            REQUIRE(std::filesystem::file_size(path) <= 64U * 1024U);
            std::ifstream stream{path, std::ios::binary};
            REQUIRE(stream.is_open());
            return {std::istreambuf_iterator<char>{stream}, {}};
        }

        /** @brief Captures one immutable, validated corpus policy. */
        PrefabLimitProfile Limits(const PrefabProjectPolicy &policy = {}) {
            return PrefabLimitProfile::Create(policy).Value();
        }

        /** @brief Decodes exact manifest identity text without path or name fallback. */
        Assets::AssetId Id(const Json &text) {
            return Assets::AssetId::Parse(text.get<std::string>()).Value();
        }

        /** @brief Parses committed canonical source and binds its actual semantic revision. */
        PrefabDependencySource Load(const std::string &name) {
            const auto bytes = Read(name);
            PrefabSourceParseLimits parse;
            parse.expectedProjectVersion = Application::ParseHoroVersion("0.2.0").Value();
            auto document = PrefabDocument::Parse(bytes, Limits(), parse);
            REQUIRE(document.HasValue());
            const auto canonical = document.Value().SerializeCanonical();
            REQUIRE(canonical.HasValue());
            CHECK(canonical.Value() == bytes);
            const PrefabSourceRevision revision{document.Value().Data().projectVersion,
                                                ComputeSha256(std::as_bytes(std::span(bytes.data(), bytes.size())))};
            return {std::move(document).Value(), revision};
        }

        /** @brief Independently captures each registry and source publication in selectable input order. */
        struct Corpus final {
            Json manifest = Json::parse(Read("expected.json"));
            Assets::AssetRegistry registry;
            std::vector<PrefabDependencySource> sources;
            std::vector<std::uint8_t> resource;
            std::vector<std::uint8_t> secondResource;
            bool reverseResources{};

            explicit Corpus(const bool reverse = false) : reverseResources(reverse) {
                std::vector<Assets::AssetRecord> records;
                for (const auto &entry : manifest.at("cases")) {
                    sources.push_back(Load(entry.at("file")));
                    const auto path = "assets/" + entry.at("file").get<std::string>();
                    records.emplace_back(Id(entry.at("asset")), TestSupport::Type("core.prefab"), ProjectPath::Parse(path).Value(),
                                         ProjectPath::Parse(path + ".horo").Value());
                    CHECK(FormatSha256(sources.back().sourceRevision.contentDigest) == entry.at("sourceHash").get<std::string>());
                }
                records.emplace_back(Test::Asset(6), TestSupport::Type("core.mesh"), ProjectPath::Parse("assets/resource.bin").Value(),
                                     ProjectPath::Parse("assets/resource.bin.horo").Value());
                records.emplace_back(Test::Asset(7), TestSupport::Type("core.mesh"),
                                     ProjectPath::Parse("assets/resource-second.bin").Value(),
                                     ProjectPath::Parse("assets/resource-second.bin.horo").Value());
                if (reverse) {
                    std::ranges::reverse(records);
                    std::ranges::reverse(sources);
                }
                REQUIRE(registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
                const auto bytes = Read("resource.bin");
                resource = TestSupport::Envelope(Test::Asset(6), "core.mesh", {bytes.begin(), bytes.end()});
                const auto secondBytes = Read("resource-second.bin");
                secondResource = TestSupport::Envelope(Test::Asset(7), "core.mesh", {secondBytes.begin(), secondBytes.end()});
            }

            /** @brief Captures the actual immutable registry and complete source closure. */
            PrefabSourceResolverSnapshot Snapshot() const {
                auto snapshot = BuildPrefabSourceResolverSnapshot(registry.Snapshot(), sources, Limits());
                REQUIRE(snapshot.HasValue());
                return std::move(snapshot).Value();
            }

            /** @brief Cooks through the production resolver and canonical resource envelope boundary. */
            Result<CookedPrefab> Cook(const PrefabSourceResolverSnapshot &snapshot, const Assets::AssetId root,
                                      const PrefabLimitProfile &limits = Limits(), const CancellationToken &token = {}) const {
                std::array dependencies{PrefabTemplateCookResource{Test::Asset(6), resource},
                                        PrefabTemplateCookResource{Test::Asset(7), secondResource}};
                if (reverseResources)
                    std::ranges::reverse(dependencies);
                const bool needsResource = root == Test::Asset(3);
                return CookPrefabTemplate(snapshot, registry.Snapshot(), root,
                                          needsResource ? std::span<const PrefabTemplateCookResource>{dependencies}
                                                        : std::span<const PrefabTemplateCookResource>{},
                                          TestSupport::Target(), limits, {token});
            }
        };

        /** @brief Compares semantic golden identities, ancestry and local transforms independently of wire encoding. */
        void CheckExpansion(const EffectivePrefabCandidate &candidate, const Json &expected) {
            REQUIRE(candidate.Objects().size() == expected.size());
            for (std::size_t index = 0; index < expected.size(); ++index) {
                const auto &object = candidate.Objects()[index];
                const auto &entry = expected[index];
                CHECK(object.key.instance.Value() == 41);
                CHECK(object.sourcePrefab == Id(entry.at("source")));
                CHECK(object.key.object.SourceObject().value == entry.at("localId"));
                const auto scope = object.key.object.NestedInstanceScope();
                REQUIRE(scope.size() == entry.at("scope").size());
                for (std::size_t slot = 0; slot < scope.size(); ++slot)
                    CHECK(scope[slot].value == entry.at("scope")[slot]);
                const int parent = entry.at("parent");
                if (parent < 0)
                    CHECK_FALSE(object.parent);
                else {
                    REQUIRE(object.parent);
                    CHECK(*object.parent == candidate.Objects()[static_cast<std::size_t>(parent)].key);
                }
                CHECK(object.effectiveLocalTransform.translation.x == entry.at("x").get<float>());
            }
        }
    }  // namespace

    TEST_CASE("Committed prefab reference corpus expands and cooks independently of capture order", "[prefab][corpus][headless]") {
        Corpus forward;
        Corpus reverse{true};
        const auto first = forward.Snapshot();
        const auto second = reverse.Snapshot();
        for (const auto &entry : forward.manifest.at("cases")) {
            INFO(entry.at("file"));
            const auto asset = Id(entry.at("asset"));
            const auto resolved = first.Resolve(asset, PrefabInstanceId::Create(41).Value(), Limits());
            const auto independentlyResolved = second.Resolve(asset, PrefabInstanceId::Create(41).Value(), Limits());
            REQUIRE(resolved.HasValue());
            REQUIRE(independentlyResolved.HasValue());
            CheckExpansion(resolved.Value(), entry.at("objects"));
            CHECK(std::ranges::equal(resolved.Value().Objects(), independentlyResolved.Value().Objects()));
            const auto cooked = forward.Cook(first, asset);
            const auto independentlyCooked = reverse.Cook(second, asset);
            REQUIRE(cooked.HasValue());
            REQUIRE(independentlyCooked.HasValue());
            CHECK(FormatSha256(ComputeSha256(cooked.Value().Bytes())) == entry.at("cookedHash").get<std::string>());
            CHECK(std::ranges::equal(cooked.Value().Bytes(), independentlyCooked.Value().Bytes()));
            const auto parsed = CookedPrefab::Parse(cooked.Value().Bytes(), asset, Limits());
            REQUIRE(parsed.HasValue());
            CHECK(parsed.Value().Data() == independentlyCooked.Value().Data());
            REQUIRE(cooked.Value().Data().entities.size() == entry.at("objects").size());
            for (std::size_t index = 0; index < cooked.Value().Data().entities.size(); ++index) {
                const auto &entity = cooked.Value().Data().entities[index];
                const auto &golden = entry.at("objects")[index];
                CHECK(entity.provenance.sourceAsset == Id(golden.at("source")));
                CHECK(entity.provenance.sourceObject.SourceObject().value == golden.at("localId"));
                CHECK(entity.localTransform.translation.x == golden.at("x").get<float>());
                const int parent = golden.at("parent");
                if (parent < 0)
                    CHECK_FALSE(entity.parent);
                else {
                    REQUIRE(entity.parent);
                    CHECK(entity.parent->value == static_cast<std::uint32_t>(parent));
                }
            }
            auto identities = RemapPrefabCandidateToScene(resolved.Value(), {}, {}, Limits());
            REQUIRE(identities.HasValue());
            std::vector<PrefabRuntimeComponentProjection> projections;
            for (const auto &object : resolved.Value().Objects())
                projections.push_back({object.key, {}});
            const auto subtree = ExpandPrefabSceneSubtree(resolved.Value(), identities.Value(), projections, {}, Limits());
            REQUIRE(subtree.HasValue());
            REQUIRE(subtree.Value().Entities().size() == entry.at("objects").size());
            Runtime::SceneDefinitionBuilder scene{{1}, {1}};
            for (const auto &entity : subtree.Value().Entities())
                scene.Add(entity);
            CHECK(std::move(scene).Build().HasValue());
        }
    }

    TEST_CASE("Committed prefab reference scene resolves nested and overridden placements headlessly", "[prefab][corpus][scene]") {
        Corpus corpus;
        const auto scene = SceneSource::DecodeSceneSource(Read("composition.scene"));
        REQUIRE(scene.HasValue());
        const SceneSource::SceneSourceView view{scene.Value().objects, scene.Value().prefabInstances};
        const auto snapshot = corpus.Snapshot();
        const auto projection = SceneSource::BuildScenePrefabProjection(view, snapshot, Limits());
        REQUIRE(projection.HasValue());
        CHECK_FALSE(projection.Value().HasBrokenInstances());
        REQUIRE(projection.Value().instances.size() == 2);
        REQUIRE(projection.Value().instances[1].expanded);
        CHECK(projection.Value().instances[1].expanded->Objects()[1].effectiveLocalTransform.translation.x == 9);
        const auto limits = Limits();
        const auto runtime = SceneSource::ConvertScenePrefabProjectionToRuntime(view, {7}, {1}, projection.Value(), {snapshot, limits});
        REQUIRE(runtime.HasValue());
        CHECK(runtime.Value().Entities().size() == 5);
    }

    TEST_CASE("Committed prefab reference corpus preserves typed diagnostics and retired snapshots", "[prefab][corpus][malformed]") {
        Corpus corpus;
        const auto pinned = corpus.Snapshot();
        const auto saved = pinned.Resolve(Test::Asset(4), PrefabInstanceId::Create(41).Value(), Limits()).Value();
        for (const auto &failure : corpus.manifest.at("failures")) {
            INFO(failure.at("file"));
            const auto bytes = Read(failure.at("file"));
            auto document = PrefabDocument::Parse(bytes, Limits());
            std::optional<Error> error;
            auto asset = Test::Asset(1);
            auto revision = corpus.sources.front().sourceRevision;
            if (failure.at("stage") == "parse") {
                REQUIRE(document.HasError());
                error = document.ErrorValue();
            } else {
                REQUIRE(document.HasValue());
                asset = document.Value().Data().assetId;
                revision = {document.Value().Data().projectVersion, ComputeSha256(std::as_bytes(std::span(bytes.data(), bytes.size())))};
                auto sources = corpus.sources;
                sources.push_back({std::move(document).Value(), revision});
                auto records = std::vector(corpus.registry.Snapshot().Records().begin(), corpus.registry.Snapshot().Records().end());
                const auto path = "assets/" + failure.at("file").get<std::string>();
                records.emplace_back(asset, TestSupport::Type("core.prefab"), ProjectPath::Parse(path).Value(),
                                     ProjectPath::Parse(path + ".horo").Value());
                Assets::AssetRegistry registry;
                REQUIRE(registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
                const auto graph = BuildPrefabDependencyGraph(registry.Snapshot(), sources, Limits());
                REQUIRE(graph.HasError());
                error = graph.ErrorValue();
            }
            REQUIRE(error);
            CHECK(error->code.Value() == failure.at("code").get<std::string>());
            const auto diagnostic =
                MakePrefabDiagnosticRecord(failure.at("stage") == "parse" ? PrefabDiagnosticCategory::Source
                                                                          : PrefabDiagnosticCategory::Graph,
                                           *error,
                                           {.prefabAsset = asset,
                                            .revision = revision,
                                            .registryRevision = pinned.RegistryRevision(),
                                            .source = DiagnosticSourceLocation{(std::filesystem::path{HORO_PREFAB_CORPUS_DIR} /
                                                                                failure.at("file").get<std::string>())
                                                                                   .string(),
                                                                               1, 1}});
            REQUIRE(diagnostic.HasValue());
            CHECK(diagnostic.Value().code.Value() == error->code.Value());
            const auto output = MakePrefabBuildOutputRecord(diagnostic.Value());
            CHECK(output.code.Value() == error->code.Value());
            REQUIRE(output.source);
            CHECK(output.source->line == 1);
        }
        const auto repeated = pinned.Resolve(Test::Asset(4), PrefabInstanceId::Create(41).Value(), Limits());
        REQUIRE(repeated.HasValue());
        CHECK(std::ranges::equal(saved.Objects(), repeated.Value().Objects()));
    }

    TEST_CASE("Committed prefab reference corpus enforces parser expansion override and cook budgets", "[prefab][corpus][boundary]") {
        Corpus corpus;
        const auto snapshot = corpus.Snapshot();
        PrefabExpansionBudget work{Limits()};
        const auto allowance = Limits().MaximumExpansionWorkItems();
        REQUIRE(work.Consume(allowance).HasValue());
        CHECK(work.Consume(1).ErrorValue().code.Value() == PrefabErrors::WorkBudgetExceeded.code.Value());
        const auto graph = BuildPrefabDependencyGraph(corpus.registry.Snapshot(), corpus.sources, Limits());
        REQUIRE(graph.HasValue());
        const std::array root{Test::Asset(4)};
        const auto closure = graph.Value().DependencyClosure(root);
        REQUIRE(closure.HasValue());
        CHECK(closure.Value() == std::vector{Test::Asset(2)});
        PrefabProjectPolicy graphPolicy;
        graphPolicy.maximumReferencedAssets = 2;
        REQUIRE(BuildPrefabDependencyGraph(corpus.registry.Snapshot(), corpus.sources, Limits(graphPolicy)).HasValue());
        graphPolicy.maximumReferencedAssets = 1;
        const auto deniedGraph = BuildPrefabDependencyGraph(corpus.registry.Snapshot(), corpus.sources, Limits(graphPolicy));
        REQUIRE(deniedGraph.HasError());
        CHECK(deniedGraph.ErrorValue().code.Value() == PrefabErrors::ReferenceCountExceeded.code.Value());
        const auto source = snapshot.Resolve(Test::Asset(2), PrefabInstanceId::Create(41).Value(), Limits()).Value();
        const auto bytes = Read("multi.prefab");
        PrefabSourceParseLimits parse{.maximumSourceBytes = bytes.size()};
        REQUIRE(PrefabDocument::Parse(bytes, Limits(), parse).HasValue());
        --parse.maximumSourceBytes;
        CHECK(PrefabDocument::Parse(bytes, Limits(), parse).ErrorValue().code.Value() == PrefabErrors::PayloadTooLarge.code.Value());
        PrefabProjectPolicy policy;
        policy.maximumObjectCount = 3;
        REQUIRE(snapshot.Resolve(Test::Asset(4), PrefabInstanceId::Create(41).Value(), Limits(policy)).HasValue());
        policy.maximumObjectCount = 2;
        CHECK(snapshot.Resolve(Test::Asset(4), PrefabInstanceId::Create(41).Value(), Limits(policy)).ErrorValue().code.Value() ==
              PrefabErrors::ObjectCountExceeded.code.Value());
        Math::Transform assigned;
        assigned.translation = {9, 0, 0};
        const PrefabValueEdit edit{PrefabValueEditKind::Assign, PrefabObjectAddress::Create({}, {17}).Value(), assigned};
        const auto prepared = PreparePrefabValueEdit(source, {}, edit, nullptr, Limits());
        REQUIRE(prepared.HasValue());
        REQUIRE(prepared.Value().effective);
        CHECK(prepared.Value().effective->Objects()[1].effectiveLocalTransform.translation.x == 9);
        CHECK(source.Objects()[1].effectiveLocalTransform.translation.x == 2);
        const auto encoded = SerializePrefabValueOverrides(prepared.Value().rebased, Limits());
        REQUIRE(encoded.HasValue());
        CHECK(encoded.Value() == Read("instance-overrides.json"));
        const auto decoded = ParsePrefabValueOverrides(Read("instance-overrides.json"), Limits());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value() == prepared.Value().rebased);
        policy = {};
        policy.maximumOverrideRecords = 1;
        REQUIRE(ValidatePrefabValueOverrideSet(decoded.Value(), Limits(policy)).HasValue());
        auto excessive = decoded.Value();
        excessive.records.push_back(excessive.records.front());
        CHECK(ValidatePrefabValueOverrideSet(excessive, Limits(policy)).HasError());
        const auto cooked = corpus.Cook(snapshot, Test::Asset(2));
        REQUIRE(cooked.HasValue());
        const std::vector saved(cooked.Value().Bytes().begin(), cooked.Value().Bytes().end());
        policy = {};
        policy.maximumCookedPayloadBytes = cooked.Value().Bytes().size() - CookedPrefabHeaderBytes;
        REQUIRE(corpus.Cook(snapshot, Test::Asset(2), Limits(policy)).HasValue());
        --policy.maximumCookedPayloadBytes;
        CHECK(corpus.Cook(snapshot, Test::Asset(2), Limits(policy)).HasError());
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        CHECK(corpus.Cook(snapshot, Test::Asset(2), Limits(), cancelled.Token()).HasError());
        CHECK(std::ranges::equal(saved, cooked.Value().Bytes()));
        auto corrupt = saved;
        corrupt.back() ^= std::byte{1};
        const auto rejected = CookedPrefab::Parse(corrupt, Test::Asset(2), Limits());
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::CorruptedPayload.code.Value());
        CHECK(std::ranges::equal(saved, cooked.Value().Bytes()));
    }

    TEST_CASE("Committed prefab reference corpus template publishes through real headless Assets and Scene owners",
              "[prefab][corpus][runtime][lifecycle]") {
        Corpus corpus;
        const auto cooked = corpus.Cook(corpus.Snapshot(), Test::Asset(1));
        REQUIRE(cooked.HasValue());
        std::vector<std::uint8_t> payload;
        for (const auto byte : cooked.Value().Bytes())
            payload.push_back(std::to_integer<std::uint8_t>(byte));
        const auto envelope = TestSupport::Envelope(Test::Asset(1), "core.prefab", std::move(payload));
        PrefabTemplateProviderLimits providerLimits;
        providerLimits.maximumArtifactBytes = envelope.size();
        TestSupport::Fixture runtime{providerLimits};
        runtime.root = envelope;
        runtime.bytes.Memory().Insert(Test::Asset(1), runtime.root);
        const auto lease = runtime.Ready();
        REQUIRE(lease.Template());
        const auto queued = runtime.provider.QueuePreparedGroup(lease, std::vector<Runtime::RuntimeComponentSet>(1));
        REQUIRE(queued.HasValue());
        runtime.Commit();
        REQUIRE(runtime.scenes.ActiveScene());
        CHECK(runtime.scenes.ActiveScene()->SlotCount() == 1);
        runtime.provider.Shutdown();
        CHECK(runtime.provider.ValidateAdmission(lease).HasError());
        CHECK(lease.Template()->Data() == cooked.Value().Data());
        --providerLimits.maximumArtifactBytes;
        TestSupport::Fixture bounded{providerLimits};
        bounded.root = envelope;
        bounded.bytes.Memory().Insert(Test::Asset(1), bounded.root);
        auto denied = bounded.Load();
        bounded.PumpUntil([&denied] {
            return denied.State() != PrefabTemplateLoadState::Loading;
        });
        CHECK(bounded.provider.TakeResult(denied).HasError());
    }
}  // namespace Horo::Prefab
