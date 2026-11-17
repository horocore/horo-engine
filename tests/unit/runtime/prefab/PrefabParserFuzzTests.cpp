#include "Horo/Prefab/PrefabDiagnostics.h"
#include "Horo/Prefab/PrefabValueOverrides.h"
#include "PrefabFuzzCorpus.h"
#include "PrefabTemplateProviderTestSupport.h"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>

namespace Horo::Prefab {
    namespace {
        using Json = nlohmann::json;

        /** @brief Reads bounded frozen 1080 baseline bytes; fuzzing never rewrites those artifacts. */
        std::string Baseline(const std::string &name) {
            const auto path = std::filesystem::path{HORO_PREFAB_FUZZ_BASELINES} / name;
            REQUIRE(std::filesystem::file_size(path) <= 64U * 1024U);
            std::ifstream input{path, std::ios::binary};
            REQUIRE(input.is_open());
            return {std::istreambuf_iterator<char>{input}, {}};
        }

        /** @brief Captures an immutable explicit policy for each independently replayed operation. */
        PrefabLimitProfile Profile(const PrefabProjectPolicy &policy = {}) {
            return PrefabLimitProfile::Create(policy).Value();
        }

        /** @brief Preserves stable typed failure identity while avoiding presentation-text or address dependence. */
        std::string Signature(const Error &error) {
            return std::string{error.domain.Value()} + "/" + std::string{error.code.Value()};
        }

        /** @brief Turns an unexpected admission outcome into an automatically minimized replayable counterexample. */
        template <class T> std::string Admission(const Result<T> &result, const bool expected) {
            if (expected && result.HasError())
                return "unexpected denial " + Signature(result.ErrorValue());
            if (!expected && result.HasValue())
                return "unexpected admission";
            if (result.HasError() && (result.ErrorValue().domain.Value().empty() || result.ErrorValue().code.Value().empty()))
                return "untyped rejection";
            return {};
        }

        /** @brief Binds a parsed frozen document to its real canonical source digest. */
        PrefabDependencySource Source(const std::string &name) {
            const auto bytes = Baseline(name);
            auto document = PrefabDocument::Parse(bytes, Profile());
            REQUIRE(document.HasValue());
            return {std::move(document).Value(),
                    {Application::ParseHoroVersion("0.2.0").Value(), ComputeSha256(std::as_bytes(std::span(bytes)))}};
        }

        /** @brief Captures two real revision-pinned sources and their canonical registry identities. */
        struct GraphInputs final {
            Assets::AssetRegistry registry;
            std::vector<PrefabDependencySource> sources{Source("multi.prefab"), Source("nested.prefab"), Source("resource.prefab")};

            GraphInputs() {
                std::vector<Assets::AssetRecord> records;
                for (const auto &source : sources) {
                    const auto suffix = source.document.Data().assetId == Test::Asset(2)   ? "multi"
                                        : source.document.Data().assetId == Test::Asset(4) ? "nested"
                                                                                           : "resource";
                    const auto path = "assets/" + std::string{suffix} + ".prefab";
                    records.emplace_back(source.document.Data().assetId, TestSupport::Type("core.prefab"), ProjectPath::Parse(path).Value(),
                                         ProjectPath::Parse(path + ".horo").Value());
                }
                for (const auto id : {6, 7}) {
                    const auto path = "assets/resource" + std::to_string(id) + ".obj";
                    records.emplace_back(Test::Asset(static_cast<std::uint16_t>(id)), TestSupport::Type("core.mesh"),
                                         ProjectPath::Parse(path).Value(), ProjectPath::Parse(path + ".horo").Value());
                }
                REQUIRE(registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
            }
        };

        /** @brief Obtains real canonical HPFB bytes from the production cook path over frozen source. */
        CookedPrefab Cook(const bool single = false) {
            const auto id = Test::Asset(single ? 1 : 2);
            Assets::AssetRegistry registry;
            REQUIRE(registry
                        .Publish({{id, TestSupport::Type("core.prefab"), ProjectPath::Parse("assets/multi.prefab").Value(),
                                   ProjectPath::Parse("assets/multi.prefab.horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            const auto resolver =
                BuildPrefabSourceResolverSnapshot(registry.Snapshot(), {Source(single ? "single.prefab" : "multi.prefab")}, Profile());
            REQUIRE(resolver.HasValue());
            auto cooked = CookPrefabTemplate(resolver.Value(), registry.Snapshot(), id, {}, TestSupport::Target(), Profile());
            REQUIRE(cooked.HasValue());
            const auto manifest = Json::parse(Baseline("expected.json"));
            const auto &cases = manifest.at("cases");
            const auto expected = std::ranges::find_if(cases, [&](const auto &entry) {
                return entry.at("file").template get<std::string>() == (single ? "single.prefab" : "multi.prefab");
            });
            REQUIRE(expected != cases.end());
            CHECK(FormatSha256(ComputeSha256(cooked.Value().Bytes())) == expected->at("cookedHash").get<std::string>());
            return std::move(cooked).Value();
        }

        /** @brief Writes one HPFB network-order header word without native-layout assumptions. */
        void Word(std::vector<std::byte> &bytes, const std::size_t offset, const std::uint32_t value) {
            REQUIRE(offset + 4 <= bytes.size());
            for (std::size_t index = 0; index < 4; ++index)
                bytes[offset + index] = static_cast<std::byte>((value >> (24U - 8U * index)) & 0xffU);
        }
    }  // namespace

    TEST_CASE("Prefab source schema fuzz preserves opaque data and rejects malformed bounded inputs", "[prefab][prefab-fuzz][source]") {
        const auto baseline = Baseline("multi.prefab");
        const auto retained = PrefabDocument::Parse(baseline, Profile()).Value();
        Fuzz::Run("source", [&](const Fuzz::Recipe &recipe) {
            const auto input = Fuzz::Source(baseline, recipe);
            PrefabSourceParseLimits limits{.expectedProjectVersion = Application::ParseHoroVersion("0.2.0").Value()};
            limits.maximumSourceBytes = input.bytes.size() + (input.bytes.empty() ? 1 : 0);
            if (input.lowerByteLimit)
                limits.maximumSourceBytes = input.bytes.size() - 1;
            const auto parsed = PrefabDocument::Parse(input.bytes, Profile(), limits);
            if (auto failure = Admission(parsed, input.accepts); !failure.empty())
                return failure;
            if (parsed.HasValue()) {
                if (parsed.Value().Data().objects.size() != 2 || parsed.Value().Data().objects[1].localId.value != 17 ||
                    parsed.Value().Data().objects[1].localTransform.translation.x != 2)
                    return std::string{"source semantic drift"};
                if (recipe.words[0] == 7 &&
                    (parsed.Value().Data().objects[0].components.size() != 1 ||
                     parsed.Value().Data().objects[0].components[0].component.typeId.Value() != "game.fuzz.opaque" ||
                     parsed.Value().Data().objects[0].components[0].component.payload != std::vector{std::byte{'{'}, std::byte{'}'}}))
                    return std::string{"unknown opaque component was discarded"};
                const auto wire = parsed.Value().SerializeCanonical();
                if (wire.HasError())
                    return "source serialization " + Signature(wire.ErrorValue());
                const auto originalBudgetReplay = PrefabDocument::Parse(wire.Value(), Profile(), limits);
                if (wire.Value().size() > limits.maximumSourceBytes) {
                    if (originalBudgetReplay.HasValue() ||
                        originalBudgetReplay.ErrorValue().code.Value() != PrefabErrors::PayloadTooLarge.code.Value())
                        return std::string{"canonical expansion bypassed the original input byte cap"};
                } else if (originalBudgetReplay.HasError()) {
                    return "canonical replay under original cap " + Signature(originalBudgetReplay.ErrorValue());
                }
                // Parser admission and canonical-output replay are distinct bounded invocations; source policy stays unchanged.
                auto replayLimits = limits;
                replayLimits.maximumSourceBytes = wire.Value().size();
                const auto replay = PrefabDocument::Parse(wire.Value(), Profile(), replayLimits);
                if (replay.HasError() || replay.Value().Data() != parsed.Value().Data())
                    return std::string{"source canonical replay drift"};
            }
            if (retained.SerializeCanonical().Value() != baseline)
                return std::string{"source failure changed retained document"};
            return std::string{};
        });
    }

    TEST_CASE("Prefab sidecar fuzz rejects ambiguous identity version and path inputs without publishing",
              "[prefab][prefab-fuzz][sidecar]") {
        constexpr auto baseline = R"({"schemaVersion":1,"assetId":"00000000-0000-0000-0000-000000000002","assetType":"core.prefab"})";
        const auto record = Assets::DecodeAssetIdentitySidecar("assets/multi.prefab", baseline).Value();
        Assets::AssetRegistry registry;
        REQUIRE(registry.Publish({record}).status == Assets::AssetRegistryBuildStatus::Complete);
        const auto pinned = registry.Snapshot();
        Fuzz::Run("sidecar", [&](const Fuzz::Recipe &recipe) {
            std::string bytes{baseline};
            std::string path{"assets/multi.prefab"};
            const bool valid = recipe.words[0] == 0;
            switch (recipe.words[0]) {
                case 0:
                    bytes.insert(0, recipe.words[1] % 17, ' ');
                    break;
                case 1:
                    Fuzz::Replace(bytes, "1,", "1e9999,");
                    break;
                case 2:
                    bytes.insert(1, "\"schemaVersion\":1,");
                    break;
                case 3:
                    bytes.resize(recipe.words[1] % (bytes.size() / 2));
                    break;
                case 4:
                    bytes.assign(Fuzz::MaximumInputBytes, 'x');
                    break;
                case 5:
                    if (recipe.words[1] % 2)
                        path = "../escape.prefab";
                    else
                        path.insert(0, 1, static_cast<char>(0xff));
                    break;
                case 6:
                    Fuzz::Replace(bytes, "\"schemaVersion\":1", "\"schemaVersion\":2");
                    break;
                case 7:
                    Fuzz::Replace(bytes, "core.prefab", "game.fuzz.unknown");
                    break;
            }
            const auto decoded = Assets::DecodeAssetIdentitySidecar(path, bytes);
            if (auto failure = Admission(decoded, valid); !failure.empty())
                return failure;
            if (valid && (decoded.Value().id != record.id || decoded.Value().sourcePath.String() != record.sourcePath.String() ||
                          decoded.Value().type != record.type))
                return std::string{"sidecar semantic drift"};
            if (registry.Snapshot().Revision() != pinned.Revision() || pinned.Find(record.id) == nullptr)
                return std::string{"malformed sidecar changed registry publication"};
            return std::string{};
        });
        auto duplicate = record;
        duplicate.sourcePath = ProjectPath::Parse("assets/duplicate.prefab").Value();
        duplicate.metadataPath = ProjectPath::Parse("assets/duplicate.prefab.horo").Value();
        CHECK(registry.Publish({record, duplicate}).status == Assets::AssetRegistryBuildStatus::Failed);
        CHECK(registry.Snapshot().Revision() == pinned.Revision());
    }

    TEST_CASE("Prefab override fuzz preserves exact canonical intent and never mutates a source candidate",
              "[prefab][prefab-fuzz][override]") {
        GraphInputs inputs;
        const auto resolver = BuildPrefabSourceResolverSnapshot(inputs.registry.Snapshot(), inputs.sources, Profile()).Value();
        const auto source = resolver.Resolve(Test::Asset(2), PrefabInstanceId::Create(41).Value(), Profile()).Value();
        const auto objects = std::vector(source.Objects().begin(), source.Objects().end());
        const auto baseline = Baseline("instance-overrides.json");
        Fuzz::Run("override", [&](const Fuzz::Recipe &recipe) {
            auto bytes = baseline;
            PrefabProjectPolicy policy;
            const bool valid = recipe.words[0] == 0;
            switch (recipe.words[0]) {
                case 0:
                    break;
                case 1:
                    Fuzz::Replace(bytes, "9.0", "1e" + std::to_string(999U + recipe.words[1] % 100));
                    break;
                case 2: {
                    auto data = Json::parse(bytes);
                    for (unsigned count = 0; count <= recipe.words[1] % 4; ++count)
                        data["records"].push_back(data["records"][0]);
                    bytes = data.dump(0);
                    break;
                }
                case 3:
                    bytes.resize(recipe.words[1] % (bytes.size() / 2));
                    break;
                case 4:
                    policy.maximumOverrideValueBytes = 1;
                    policy.maximumOverrideSetBytes = 1;
                    break;
                case 5:
                    bytes.insert(recipe.words[1] % bytes.size(), 1, static_cast<char>(0xff));
                    break;
                case 6:
                    Fuzz::Replace(bytes, "\"schema\": 1", "\"schema\": 2");
                    break;
                case 7:
                    Fuzz::Replace(bytes, "\"kind\": \"transform\"", "\"kind\": \"unregistered" + std::to_string(recipe.words[1]) + "\"");
                    break;
            }
            const auto parsed = ParsePrefabValueOverrides(bytes, Profile(policy));
            if (auto failure = Admission(parsed, valid); !failure.empty())
                return failure;
            if (parsed.HasValue()) {
                const auto resolved = ResolvePrefabValueOverrides(source, parsed.Value(), nullptr, Profile(policy));
                if (resolved.HasError() || !resolved.Value().effective || resolved.Value().effective->Objects().size() != 2 ||
                    resolved.Value().effective->Objects()[1].effectiveLocalTransform.translation.x != 9)
                    return std::string{"valid override resolution drift"};
                if (SerializePrefabValueOverrides(parsed.Value(), Profile(policy)).Value() != baseline)
                    return std::string{"canonical override intent drift"};
            }
            if (!std::ranges::equal(source.Objects(), objects))
                return std::string{"override fuzz mutated source candidate"};
            return std::string{};
        });
    }

    TEST_CASE("Prefab graph fuzz rejects incoherent closure and preserves the captured graph", "[prefab][prefab-fuzz][graph]") {
        GraphInputs inputs;
        const auto retained = BuildPrefabDependencyGraph(inputs.registry.Snapshot(), inputs.sources, Profile()).Value();
        const auto edges = std::vector(retained.Edges().begin(), retained.Edges().end());
        Fuzz::Run("graph", [&](const Fuzz::Recipe &recipe) {
            auto sources = inputs.sources;
            auto records = std::vector(inputs.registry.Snapshot().Records().begin(), inputs.registry.Snapshot().Records().end());
            PrefabProjectPolicy policy;
            const bool valid = recipe.words[0] == 0;
            switch (recipe.words[0]) {
                case 0:
                    policy.maximumReferencedAssets = 2;
                    if (recipe.words[1] % 2)
                        std::ranges::reverse(sources);
                    break;
                case 1:
                    sources[0].sourceRevision.projectVersion.prerelease = "invalid version text";
                    break;
                case 2:
                    sources.push_back(sources[recipe.words[1] % sources.size()]);
                    break;
                case 3:
                    sources.erase(sources.begin());
                    break;
                case 4:
                    policy.maximumReferencedAssets = 1;
                    break;
                case 5:
                    records.erase(records.begin());
                    break;
                case 6:
                    sources[0].sourceRevision.contentDigest.bytes[recipe.words[1] % 32] ^= 1;
                    break;
                case 7:
                    records[0].type = Assets::AssetTypeId::Parse("game.fuzz.unknown").Value();
                    records[0].sourcePath = ProjectPath::Parse("assets/foreign.obj").Value();
                    records[0].metadataPath = ProjectPath::Parse("assets/foreign.obj.horo").Value();
                    break;
            }
            Assets::AssetRegistry registry;
            const auto publication = registry.Publish(records);
            if (publication.status == Assets::AssetRegistryBuildStatus::Failed)
                return valid ? std::string{"valid graph registry denied"} : std::string{};
            const auto graph = BuildPrefabDependencyGraph(registry.Snapshot(), sources, Profile(policy));
            if (auto failure = Admission(graph, valid); !failure.empty())
                return failure;
            if (graph.HasValue() && !std::ranges::equal(graph.Value().Edges(), edges))
                return std::string{"graph order changed semantic edges"};
            if (!std::ranges::equal(retained.Edges(), edges))
                return std::string{"failed graph candidate replaced retained snapshot"};
            return std::string{};
        });
    }

    TEST_CASE("Prefab graph cyclic input remains bounded and fails expansion without replacing prior evidence",
              "[prefab][prefab-fuzz][graph]") {
        GraphInputs inputs;
        const auto retained = BuildPrefabSourceResolverSnapshot(inputs.registry.Snapshot(), inputs.sources, Profile()).Value();
        const auto before = retained.Resolve(Test::Asset(4), PrefabInstanceId::Create(41).Value(), Profile()).Value();
        auto data = inputs.sources[0].document.Data();
        data.referencedAssets.push_back(Test::Asset(4));
        data.composition = PrefabComposition{.nestedPlacements = {{.placementLocalId = {9},
                                                                   .sourcePrefab = PrefabAssetReference::Create(Test::Asset(4)).Value(),
                                                                   .authoredAgainst = inputs.sources[1].sourceRevision}}};
        const auto document = PrefabDocument::Create(std::move(data), Profile());
        REQUIRE(document.HasValue());
        inputs.sources[0].document = document.Value();
        const auto cyclic = BuildPrefabSourceResolverSnapshot(inputs.registry.Snapshot(), inputs.sources, Profile());
        REQUIRE(cyclic.HasValue());
        const auto rejected = cyclic.Value().Resolve(Test::Asset(4), PrefabInstanceId::Create(41).Value(), Profile());
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::DependencyGraphInvalid.code.Value());
        const auto repeated = retained.Resolve(Test::Asset(4), PrefabInstanceId::Create(41).Value(), Profile());
        REQUIRE(repeated.HasValue());
        CHECK(std::ranges::equal(repeated.Value().Objects(), before.Objects()));
    }

    TEST_CASE("Prefab cooked template fuzz checks framing integrity schema and finite budgets", "[prefab][prefab-fuzz][cooked]") {
        const auto retained = Cook();
        const std::vector baseline(retained.Bytes().begin(), retained.Bytes().end());
        Fuzz::Run("cooked", [&](const Fuzz::Recipe &recipe) {
            auto bytes = baseline;
            PrefabProjectPolicy policy;
            const bool valid = recipe.words[0] == 0;
            switch (recipe.words[0]) {
                case 0:
                    policy.maximumObjectCount = 2;
                    policy.maximumCookedPayloadBytes = bytes.size() - CookedPrefabHeaderBytes;
                    break;
                case 1: {
                    auto data = retained.Data();
                    data.entities[recipe.words[1] % data.entities.size()].localTransform.translation.x =
                        std::numeric_limits<float>::infinity();
                    return Admission(CookedPrefab::Create(std::move(data), Profile()), false);
                }
                case 2:
                    bytes.insert(bytes.end(), baseline.begin() + CookedPrefabHeaderBytes, baseline.end());
                    break;
                case 3:
                    bytes.resize(recipe.words[1] % bytes.size());
                    break;
                case 4:
                    policy.maximumCookedPayloadBytes = bytes.size() - CookedPrefabHeaderBytes - 1;
                    break;
                case 5:
                    bytes[recipe.words[1] % 4] ^= std::byte{0xff};
                    break;
                case 6:
                    Word(bytes, 4, CurrentCookedPrefabVersion + 1 + recipe.words[1] % 7);
                    break;
                case 7: {
                    auto data = retained.Data();
                    RawComponentPayload opaque;
                    opaque.instance = PrefabComponentInstanceId::Create(1).Value();
                    opaque.component.typeId = Gameplay::ComponentTypeId::Parse("game.fuzz.unknown").Value();
                    opaque.component.payload = {std::byte{'{'}, std::byte{'}'}};
                    data.entities[0].members.push_back(opaque);
                    const auto encoded = CookedPrefab::Create(data, Profile());
                    if (auto failure = Admission(encoded, true); !failure.empty())
                        return failure;
                    const auto decoded = CookedPrefab::Parse(encoded.Value().Bytes(), Test::Asset(2), Profile());
                    if (auto failure = Admission(decoded, true); !failure.empty())
                        return failure;
                    return decoded.Value().Data() == data ? std::string{} : std::string{"unknown cooked member was discarded"};
                }
            }
            const auto parsed = CookedPrefab::Parse(bytes, Test::Asset(2), Profile(policy));
            if (auto failure = Admission(parsed, valid); !failure.empty())
                return failure;
            if (parsed.HasValue() && parsed.Value().Data() != retained.Data())
                return std::string{"valid cooked semantic drift"};
            if (!std::ranges::equal(retained.Bytes(), baseline))
                return std::string{"cooked failure changed resident template"};
            return std::string{};
        });
    }

    TEST_CASE("Prefab runtime fuzz rejects corrupt templates before any deferred Scene publication", "[prefab][prefab-fuzz][runtime]") {
        const auto original = Cook(true);
        const std::vector baseline(original.Bytes().begin(), original.Bytes().end());
        for (unsigned mutation = 0; mutation < 8; ++mutation) {
            INFO("runtime mutation=" << mutation);
            auto bytes = baseline;
            if (mutation == 1)
                bytes.back() ^= std::byte{1};
            if (mutation == 2)
                bytes.push_back(std::byte{0});
            if (mutation == 3)
                bytes.resize(CookedPrefabHeaderBytes - 1);
            if (mutation == 5)
                bytes.front() ^= std::byte{0xff};
            if (mutation == 6)
                Word(bytes, 4, CurrentCookedPrefabVersion + 1);
            if (mutation == 7) {
                auto data = original.Data();
                RawComponentPayload opaque;
                opaque.instance = PrefabComponentInstanceId::Create(1).Value();
                opaque.component.typeId = Gameplay::ComponentTypeId::Parse("game.fuzz.unknown").Value();
                opaque.component.payload = {std::byte{'{'}, std::byte{'}'}};
                data.entities[0].members.push_back(opaque);
                const auto encoded = CookedPrefab::Create(std::move(data), Profile());
                REQUIRE(encoded.HasValue());
                bytes.assign(encoded.Value().Bytes().begin(), encoded.Value().Bytes().end());
            }
            std::vector<std::uint8_t> payload;
            for (const auto byte : bytes)
                payload.push_back(std::to_integer<std::uint8_t>(byte));
            const auto envelope = TestSupport::Envelope(Test::Asset(), "core.prefab", std::move(payload));
            PrefabTemplateProviderLimits limits;
            if (mutation == 4)
                limits.maximumArtifactBytes = envelope.size() - 1;
            TestSupport::Fixture runtime{limits};
            runtime.root = envelope;
            runtime.bytes.Memory().Insert(Test::Asset(), envelope);
            auto handle = runtime.Load();
            runtime.PumpUntil([&handle] {
                return handle.State() != PrefabTemplateLoadState::Loading;
            });
            const auto lease = runtime.provider.TakeResult(handle);
            if (mutation == 0 || mutation == 7) {
                REQUIRE(lease.HasValue());
                const auto prepared = runtime.provider.QueuePreparedGroup(lease.Value(), std::vector<Runtime::RuntimeComponentSet>(1));
                if (mutation == 0) {
                    REQUIRE(prepared.HasValue());
                    runtime.Commit();
                    CHECK(runtime.scenes.ActiveScene()->SlotCount() == 1);
                    runtime.provider.Shutdown();
                    CHECK(runtime.provider.ValidateAdmission(lease.Value()).HasError());
                    CHECK(lease.Value().Template()->Data() == original.Data());
                } else {
                    REQUIRE(prepared.HasError());
                    runtime.Commit();
                    CHECK(runtime.scenes.ActiveScene()->SlotCount() == 0);
                }
            } else {
                REQUIRE(lease.HasError());
                CHECK_FALSE(lease.ErrorValue().code.Value().empty());
                runtime.Commit();
                CHECK(runtime.scenes.ActiveScene()->SlotCount() == 0);
            }
            CHECK(std::ranges::equal(original.Bytes(), baseline));
        }
    }

    TEST_CASE("Prefab fuzz retains canonical typed diagnostics and source navigation for replay", "[prefab][prefab-fuzz][diagnostics]") {
        const auto original = Source("multi.prefab");
        Fuzz::Recipe recipe{{2, 8, 7, 6, 5, 4, 3, 2}};
        const auto input = Fuzz::Source(Baseline("multi.prefab"), recipe);
        const auto parsed = PrefabDocument::Parse(input.bytes, Profile());
        REQUIRE(parsed.HasError());
        const auto path = (std::filesystem::path{HORO_PREFAB_FUZZ_BASELINES} / "multi.prefab").string();
        const auto diagnostic = MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Source, parsed.ErrorValue(),
                                                           {.prefabAsset = Test::Asset(2),
                                                            .revision = original.sourceRevision,
                                                            .registryRevision = {1},
                                                            .source = DiagnosticSourceLocation{path, 1, 1}},
                                                           PrefabDiagnosticOperation::Parse);
        REQUIRE(diagnostic.HasValue());
        CHECK(diagnostic.Value().code.Value() == parsed.ErrorValue().code.Value());
        const auto output = MakePrefabBuildOutputRecord(diagnostic.Value());
        CHECK(output.code.Value() == parsed.ErrorValue().code.Value());
        REQUIRE(output.source);
        CHECK(output.source->absolutePath == path);
        CHECK(output.source->line == 1);
    }

    TEST_CASE("Prefab fuzz minimizes and exactly replays real parser counterexamples", "[prefab][prefab-fuzz][replay]") {
        const auto classify = [](const std::string &bytes) {
            const auto parsed = PrefabDocument::Parse(bytes, Profile());
            return parsed.HasError() ? Signature(parsed.ErrorValue()) : std::string{};
        };
        const auto original = Baseline("multi.prefab").substr(0, 120);
        const auto signature = classify(original);
        REQUIRE_FALSE(signature.empty());
        std::size_t attempts{};
        const auto minimized = Fuzz::MinimizeBytes(original, signature, classify, attempts);
        CHECK(minimized.size() < original.size());
        CHECK(attempts <= Fuzz::MaximumShrinkAttempts);
        CHECK(classify(minimized) == signature);
        CHECK(classify(std::string{minimized.data(), minimized.size()}) == signature);
        Fuzz::Recipe recipe{{3, 12345, 99, 77, 88, 66, 55, 44}};
        const auto baseline = Baseline("multi.prefab");
        const auto classifyRecipe = [&](const Fuzz::Recipe &candidate) {
            return classify(Fuzz::Source(baseline, candidate).bytes);
        };
        const auto shrunk = Fuzz::Minimize(recipe, classifyRecipe(recipe), classifyRecipe, attempts);
        CHECK(shrunk != recipe);
        CHECK(shrunk.words[1] == 0);
        CHECK(classifyRecipe(shrunk) == classifyRecipe(recipe));
        CHECK(Fuzz::Decode(Fuzz::Encode(shrunk)) == shrunk);
    }
}  // namespace Horo::Prefab
