#include "Horo/Gameplay/GameplayVfxBindingTable.h"
#include "Horo/Vfx/VfxWorld.h"
#include "support/VfxTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Gameplay {
    namespace {
        using Json = nlohmann::json;
        using JsonMutation = void (*)(Json &);

        [[nodiscard]] Assets::AssetId CodecAsset() {
            return Assets::AssetId::Parse("17490000-0000-4000-8000-000000000001").Value();
        }

        [[nodiscard]] GameplayVfxBindingDocument CodecDocument() {
            GameplayVfxLayer layer;
            layer.effectAsset = CodecAsset();
            layer.scale = 1.5F;
            layer.constants.schema = 9;
            layer.constants.count = 1;
            layer.constants.values[0] = {2, 0.5F};
            layer.routes.push_back({5, 3});
            GameplayVfxBinding binding;
            binding.destination = 10;
            binding.event = GameplayEventTypeId{"test.vfx.codec"};
            binding.payloadSchema = 11;
            binding.inputs.push_back({5, 0.0F, 1.0F});
            binding.layers.push_back(layer);
            GameplayVfxBinding tagged;
            tagged.destination = 20;
            tagged.tag.value = 99;
            tagged.requirement = Vfx::VfxRequirementClass::GameplayRequired;
            layer.constants = {};
            layer.routes.clear();
            tagged.layers.push_back(std::move(layer));
            return {.generation = 7, .bindings = {std::move(binding), std::move(tagged)}};
        }

        [[nodiscard]] std::string Encoded(const GameplayVfxBindingDocument &document = CodecDocument()) {
            auto result = EncodeGameplayVfxBindings(document);
            INFO((result.HasError() ? result.ErrorValue().message : "encoded"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        [[nodiscard]] Json WireDocument() {
            return Json::parse(Encoded());
        }

        void RequireRejected(const Json &wire) {
            const auto decoded = DecodeGameplayVfxBindings(wire.dump());
            REQUIRE(decoded.HasError());
        }

        [[nodiscard]] GameplayVfxBindingDocument CookDocument() {
            auto document = CodecDocument();
            document.bindings.resize(1);
            auto &binding = document.bindings[0];
            binding.payloadSchema = 0;
            binding.inputs.clear();
            binding.layers[0].constants = {};
            binding.layers[0].routes.clear();
            return document;
        }

        [[nodiscard]] std::unique_ptr<Vfx::VfxWorld> CookingWorld() {
            auto data = Vfx::Tests::ValidParticleDescriptorData();
            data.maximumParticles = 4;
            data.simulationPreference = Vfx::SimulationPreference::RequireCPU;
            data.spawnRate = {0.0, 0.0};
            data.collisionMode = Vfx::ParticleCollisionMode::None;
            data.sortMode = Vfx::ParticleSortMode::None;
            const Vfx::VfxSpawnAssetPreparation recipe{.asset = CodecAsset(),
                                                       .particles = std::make_shared<const Vfx::ParticleSystemDescriptor>(
                                                           Vfx::Tests::ParticleDescriptor(data)),
                                                       .simulation = {.effectSeed = 7,
                                                                      .maximumBurstParticles = 4,
                                                                      .maximumDeltaSeconds = 1.0F},
                                                       .maximumInstances = 1,
                                                       .initialBurst = 1,
                                                       .durationSeconds = 1.0F,
                                                       .material = {1}};
            const Vfx::VfxSpawnBudget budget{.maximumPending = 4,
                                             .maximumResults = 4,
                                             .maximumOwners = 2,
                                             .maximumProducers = 2,
                                             .maximumInstances = 1,
                                             .maximumParticles = 4,
                                             .maximumBytes = 1024U * 1024U,
                                             .maximumDeltaSeconds = 1.0F};
            const Vfx::VfxExtractionBudget extraction{.maximumBatches = 2,
                                                      .maximumInstances = 4,
                                                      .maximumDrawCalls = 2,
                                                      .frameSlots = 2,
                                                      .maximumBytes = 1024U * 1024U};
            auto result =
                Vfx::VfxWorld::PrepareSpawns(Vfx::VfxIdentityScope::Create(1749).Value(), std::span{&recipe, 1}, budget, extraction);
            INFO((result.HasError() ? result.ErrorValue().message : "prepared"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        [[nodiscard]] Assets::AssetRecord CookRecord(const std::string_view type = "core.vfx.effect") {
            return {.id = CodecAsset(),
                    .type = Assets::AssetTypeId::Parse(type).Value(),
                    .sourcePath = ProjectPath::Parse("Assets/Effects/codec.vfx").Value(),
                    .metadataPath = ProjectPath::Parse("Assets/Effects/codec.vfx.horo").Value()};
        }

        struct CookingFixture final {
            Assets::AssetRegistry registry;
            Assets::MemoryAssetProvider provider;
            std::unique_ptr<Vfx::VfxWorld> world{CookingWorld()};
            Vfx::VfxSpawnAssetInfo prepared;

            CookingFixture() {
                REQUIRE(registry.Publish({CookRecord()}).status == Assets::AssetRegistryBuildStatus::Complete);
                // Cook checks availability; genuine recipe/schema facts come from the prepared World.
                provider.Insert(CodecAsset(), {'{', '}'});
                REQUIRE(world->InspectSpawnAsset(CodecAsset(), prepared) == Vfx::VfxSpawnStatus::Accepted);
            }

            [[nodiscard]] auto Cook(GameplayVfxBindingDocument document = CookDocument()) const {
                return GameplayVfxBindingTable::Cook(std::move(document), registry.Snapshot(), provider, std::span{&prepared, 1});
            }
        };
    }  // namespace

    TEST_CASE("VFX binding codec preserves authored selectors channels and layer order", "[unit][gameplay][vfx][codec]") {
        const auto encoded = Encoded();
        const auto decoded = DecodeGameplayVfxBindings(encoded);
        REQUIRE(decoded.HasValue());
        const auto &document = decoded.Value();
        REQUIRE(document.bindings.size() == 2);
        CHECK(document.version == 1);
        CHECK(document.generation == 7);
        const auto &event = document.bindings[0];
        CHECK(event.destination == 10);
        CHECK(event.event.Value() == "test.vfx.codec");
        CHECK_FALSE(event.tag.IsValid());
        CHECK(event.payloadSchema == 11);
        REQUIRE(event.inputs.size() == 1);
        CHECK(event.inputs[0].channel == 5);
        CHECK(event.inputs[0].minimum == 0.0F);
        CHECK(event.inputs[0].maximum == 1.0F);
        REQUIRE(event.layers.size() == 1);
        CHECK(event.layers[0].effectAsset == CodecAsset());
        CHECK(event.layers[0].scale == 1.5F);
        CHECK(event.layers[0].constants.schema == 9);
        CHECK(event.layers[0].constants.values[0] == Vfx::VfxParameterValue{2, 0.5F});
        REQUIRE(event.layers[0].routes.size() == 1);
        CHECK(event.layers[0].routes[0].source == 5);
        CHECK(event.layers[0].routes[0].target == 3);
        CHECK(document.bindings[1].tag.value == 99);
        CHECK_FALSE(document.bindings[1].event.IsValid());
        CHECK(document.bindings[1].requirement == Vfx::VfxRequirementClass::GameplayRequired);
        CHECK(Encoded(document) == encoded);
        CHECK(Encoded() == encoded);
    }

    TEST_CASE("VFX binding codec preserves full unsigned generation and destination precision", "[unit][gameplay][vfx][codec]") {
        auto document = CodecDocument();
        document.generation = std::numeric_limits<std::uint64_t>::max();
        document.bindings[1].destination = std::numeric_limits<std::uint64_t>::max();
        document.bindings[1].tag.value = std::numeric_limits<std::uint64_t>::max();
        const auto decoded = DecodeGameplayVfxBindings(Encoded(document));
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().generation == document.generation);
        CHECK(decoded.Value().bindings[1].destination == document.bindings[1].destination);
        CHECK(decoded.Value().bindings[1].tag.value == document.bindings[1].tag.value);
    }

    TEST_CASE("VFX binding codec rejects nonobject and malformed source documents", "[unit][gameplay][vfx][codec]") {
        for (const auto source : {"", "null", "[]", "true", "1", "{", "{\"version\":1} trailing"})
            CHECK(DecodeGameplayVfxBindings(source).HasError());
    }

    TEST_CASE("VFX binding codec rejects lossy numeric conversions", "[unit][gameplay][vfx][codec]") {
        const std::array<JsonMutation, 13> mutations{
            [](Json &j) {
            j["generation"] = 0;
        },
            [](Json &j) {
            j["generation"] = -1;
        },
            [](Json &j) {
            j["generation"] = 1.5;
        },
            [](Json &j) {
            j["generation"] = "7";
        },
            [](Json &j) {
            j["generation"] = 1.0e100;
        },
            [](Json &j) {
            j["version"] = true;
        },
            [](Json &j) {
            j["bindings"][0]["schemaVersion"] = 0;
        },
            [](Json &j) {
            j["bindings"][0]["schemaVersion"] = 4'294'967'296ULL;
        },
            [](Json &j) {
            j["bindings"][0]["destination"] = 10.5;
        },
            [](Json &j) {
            j["bindings"][0]["inputs"][0]["channel"] = 65'536;
        },
            [](Json &j) {
            j["bindings"][0]["layers"][0]["scale"] = 1.0e100;
        },
            [](Json &j) {
            j["bindings"][0]["layers"][0]["constants"]["values"][0]["channel"] = 2.5;
        },
            [](Json &j) {
            j["bindings"][0]["layers"][0]["constants"]["values"][0]["value"] = true;
        },
        };
        for (const auto mutate : mutations) {
            auto wire = WireDocument();
            mutate(wire);
            RequireRejected(wire);
        }
    }

    TEST_CASE("VFX binding codec rejects unsupported versions explicitly", "[unit][gameplay][vfx][codec]") {
        auto wire = WireDocument();
        wire["version"] = 2;
        const auto decoded = DecodeGameplayVfxBindings(wire.dump());
        REQUIRE(decoded.HasError());
        CHECK(decoded.ErrorValue().code.Value() == GameplayVfxErrors::VersionUnsupported.code.Value());
    }

    TEST_CASE("VFX binding codec rejects duplicate root and nested fields", "[unit][gameplay][vfx][codec]") {
        auto rootDuplicate = Encoded();
        rootDuplicate.insert(1, "\"generation\":7,");
        const auto root = DecodeGameplayVfxBindings(rootDuplicate);
        REQUIRE(root.HasError());
        CHECK(root.ErrorValue().code.Value() == GameplayVfxErrors::DuplicateField.code.Value());
        auto nestedDuplicate = Encoded();
        const auto channel = nestedDuplicate.find("\"channel\"");
        REQUIRE(channel != std::string::npos);
        nestedDuplicate.insert(channel, "\"channel\":2,");
        const auto nested = DecodeGameplayVfxBindings(nestedDuplicate);
        REQUIRE(nested.HasError());
        CHECK(nested.ErrorValue().code.Value() == GameplayVfxErrors::DuplicateField.code.Value());
    }

    TEST_CASE("VFX binding codec rejects unknown fields at each schema boundary", "[unit][gameplay][vfx][codec]") {
        const std::array<JsonMutation, 7> mutations{
            [](Json &j) {
            j["unknown"] = 1;
        },
            [](Json &j) {
            j["bindings"][0]["unknown"] = 1;
        },
            [](Json &j) {
            j["bindings"][0]["inputs"][0]["unknown"] = 1;
        },
            [](Json &j) {
            j["bindings"][0]["layers"][0]["unknown"] = 1;
        },
            [](Json &j) {
            j["bindings"][0]["layers"][0]["constants"]["unknown"] = 1;
        },
            [](Json &j) {
            j["bindings"][0]["layers"][0]["constants"]["values"][0]["unknown"] = 1;
        },
            [](Json &j) {
            j["bindings"][0]["layers"][0]["routes"][0]["unknown"] = 1;
        },
        };
        for (const auto mutate : mutations) {
            auto wire = WireDocument();
            mutate(wire);
            RequireRejected(wire);
        }
    }

    TEST_CASE("VFX binding codec requires complete metadata instead of defaulting missing fields", "[unit][gameplay][vfx][codec]") {
        const std::array<JsonMutation, 12> mutations{
            [](Json &j) {
            j.erase("version");
        },
            [](Json &j) {
            j.erase("generation");
        },
            [](Json &j) {
            j.erase("bindings");
        },
            [](Json &j) {
            j["bindings"][0].erase("destination");
        },
            [](Json &j) {
            j["bindings"][0].erase("schemaVersion");
        },
            [](Json &j) {
            j["bindings"][0].erase("payloadSchema");
        },
            [](Json &j) {
            j["bindings"][0].erase("inputs");
        },
            [](Json &j) {
            j["bindings"][0].erase("requirement");
        },
            [](Json &j) {
            j["bindings"][0].erase("layers");
        },
            [](Json &j) {
            j["bindings"][0]["layers"][0].erase("constants");
        },
            [](Json &j) {
            j["bindings"][0]["layers"][0].erase("routes");
        },
            [](Json &j) {
            j["bindings"][0]["layers"][0].erase("scale");
        },
        };
        for (const auto mutate : mutations) {
            auto wire = WireDocument();
            mutate(wire);
            RequireRejected(wire);
        }
    }

    TEST_CASE("VFX binding codec accepts exactly one typed selector per binding", "[unit][gameplay][vfx][codec]") {
        const std::array<JsonMutation, 5> mutations{
            [](Json &j) {
            j["bindings"][0]["tag"] = 99;
        },
            [](Json &j) {
            j["bindings"][0].erase("event");
        },
            [](Json &j) {
            j["bindings"][0]["event"] = 5;
        },
            [](Json &j) {
            j["bindings"][1]["tag"] = 0;
        },
            [](Json &j) {
            j["bindings"][1]["tag"] = 1.5;
        },
        };
        for (const auto mutate : mutations) {
            auto wire = WireDocument();
            mutate(wire);
            RequireRejected(wire);
        }
    }

    TEST_CASE("VFX binding codec rejects ambiguous ordering channels and parameter routes", "[unit][gameplay][vfx][codec]") {
        const std::array<JsonMutation, 8> mutations{
            [](Json &j) {
            j["bindings"][1]["destination"] = 10;
        },
            [](Json &j) {
            j["bindings"][1]["destination"] = 9;
        },
            [](Json &j) {
            j["bindings"][0]["inputs"].push_back(j["bindings"][0]["inputs"][0]);
        },
            [](Json &j) {
            j["bindings"][0]["inputs"][0]["minimum"] = 2.0;
        },
            [](Json &j) {
            j["bindings"][0]["layers"][0]["constants"]["values"].push_back({{"channel", 2}, {"value", 1.0}});
        },
            [](Json &j) {
            j["bindings"][0]["layers"][0]["routes"][0]["target"] = 2;
        },
            [](Json &j) {
            j["bindings"][0]["layers"][0]["routes"][0]["source"] = 6;
        },
            [](Json &j) {
            j["bindings"][0]["requirement"] = "silently-drop";
        },
        };
        for (const auto mutate : mutations) {
            auto wire = WireDocument();
            mutate(wire);
            RequireRejected(wire);
        }
    }

    TEST_CASE("VFX binding encoder rejects nonfinite scalar data before JSON conversion", "[unit][gameplay][vfx][codec]") {
        auto document = CodecDocument();
        document.bindings[0].layers[0].scale = std::numeric_limits<float>::infinity();
        CHECK(EncodeGameplayVfxBindings(document).HasError());
        document = CodecDocument();
        document.bindings[0].layers[0].constants.values[0].value = std::numeric_limits<float>::quiet_NaN();
        CHECK(EncodeGameplayVfxBindings(document).HasError());
        document = CodecDocument();
        document.bindings[0].inputs[0].minimum = -std::numeric_limits<float>::infinity();
        CHECK(EncodeGameplayVfxBindings(document).HasError());
    }

    TEST_CASE("VFX binding codec enforces finite source and collection ceilings", "[unit][gameplay][vfx][codec]") {
        const auto oversized = DecodeGameplayVfxBindings(std::string(GameplayVfxBindingLimits::SourceBytes + 1, ' '));
        REQUIRE(oversized.HasError());
        CHECK(oversized.ErrorValue().code.Value() == GameplayVfxErrors::LimitExceeded.code.Value());
        auto document = CodecDocument();
        document.bindings[0].layers.resize(Vfx::VfxSpawnHardLimits::Layers + 1, document.bindings[0].layers[0]);
        CHECK(EncodeGameplayVfxBindings(document).HasError());
        document = CodecDocument();
        document.bindings[0].inputs.clear();
        for (std::uint16_t channel = 0; channel <= Vfx::VfxSpawnHardLimits::Parameters; ++channel)
            document.bindings[0].inputs.push_back({channel, 0.0F, 1.0F});
        CHECK(EncodeGameplayVfxBindings(document).HasError());
        document = CodecDocument();
        document.bindings[0].layers[0].constants.count = Vfx::VfxSpawnHardLimits::Parameters + 1;
        CHECK(EncodeGameplayVfxBindings(document).HasError());
        document = CodecDocument();
        auto &layer = document.bindings[0].layers[0];
        layer.constants = {};
        layer.routes.clear();
        for (std::uint16_t target = 0; target <= Vfx::VfxSpawnHardLimits::Parameters; ++target)
            layer.routes.push_back({5, target});
        CHECK(EncodeGameplayVfxBindings(document).HasError());
    }

    TEST_CASE("VFX binding codec admits its exact binding ceiling and rejects one additional binding", "[unit][gameplay][vfx][codec]") {
        auto document = CodecDocument();
        const auto prototype = document.bindings[0];
        document.bindings.clear();
        for (std::size_t index = 0; index <= GameplayVfxBindingLimits::Bindings; ++index) {
            auto binding = prototype;
            binding.destination = index + 1;
            binding.event = GameplayEventTypeId{"test.vfx.event" + std::to_string(index)};
            document.bindings.push_back(std::move(binding));
        }
        CHECK(EncodeGameplayVfxBindings(document).HasError());
        document.bindings.pop_back();
        const auto decoded = DecodeGameplayVfxBindings(Encoded(document));
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().bindings.size() == GameplayVfxBindingLimits::Bindings);
    }

    TEST_CASE("VFX binding codec rejects excessive JSON depth and invalid UTF8", "[unit][gameplay][vfx][codec]") {
        std::string deep;
        for (std::size_t depth = 0; depth <= GameplayVfxBindingLimits::JsonDepth; ++depth)
            deep += "[";
        deep += "0";
        deep.append(GameplayVfxBindingLimits::JsonDepth + 1, ']');
        CHECK(DecodeGameplayVfxBindings(deep).HasError());
        auto invalid = Encoded();
        const auto event = invalid.find("test.vfx.codec");
        REQUIRE(event != std::string::npos);
        invalid[event] = static_cast<char>(0xFF);
        CHECK(DecodeGameplayVfxBindings(invalid).HasError());
    }

    TEST_CASE("VFX binding cook retains pinned metadata without borrowing provider or World", "[unit][gameplay][vfx][cook]") {
        std::shared_ptr<const GameplayVfxBindingTable> table;
        {
            CookingFixture fixture;
            auto cooked = fixture.Cook();
            INFO((cooked.HasError() ? cooked.ErrorValue().message : "cooked"));
            REQUIRE(cooked.HasValue());
            table = std::move(cooked).Value();
            fixture.provider.Remove(CodecAsset());
            REQUIRE(fixture.registry.Publish(std::vector<Assets::AssetRecord>{}).status == Assets::AssetRegistryBuildStatus::Complete);
            fixture.world.reset();
        }
        CHECK(table->Generation() == 7);
        const auto found = table->FindEvent("test.vfx.codec");
        CHECK(found.status == Vfx::VfxSpawnStatus::Accepted);
        CHECK(found.key.generation == 7);
        CHECK(found.key.destination == 10);
        CHECK(found.payloadSchema == 0);
    }

    TEST_CASE("VFX binding cook distinguishes missing wrong-type and unavailable assets", "[unit][gameplay][vfx][cook]") {
        CookingFixture fixture;
        SECTION("Missing pinned registry record") {
            REQUIRE(fixture.registry.Publish(std::vector<Assets::AssetRecord>{}).status == Assets::AssetRegistryBuildStatus::Complete);
            const auto result = fixture.Cook();
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == GameplayVfxErrors::MissingAsset.code.Value());
        }
        SECTION("Wrong registered type") {
            REQUIRE(fixture.registry.Publish({CookRecord("core.mesh")}).status == Assets::AssetRegistryBuildStatus::Complete);
            const auto result = fixture.Cook();
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == GameplayVfxErrors::AssetTypeMismatch.code.Value());
        }
        SECTION("Unavailable actual provider artifact") {
            fixture.provider.Remove(CodecAsset());
            const auto result = fixture.Cook();
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == GameplayVfxErrors::AssetUnavailable.code.Value());
        }
    }

    TEST_CASE("VFX binding cook checks parameter schemas against genuinely prepared recipes", "[unit][gameplay][vfx][cook]") {
        CookingFixture fixture;
        auto document = CookDocument();
        document.bindings[0].layers[0].constants.schema = 9;
        document.bindings[0].layers[0].constants.count = 1;
        document.bindings[0].layers[0].constants.values[0] = {2, 0.5F};
        const auto result = fixture.Cook(std::move(document));
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == GameplayVfxErrors::ParameterMismatch.code.Value());
    }

    TEST_CASE("Cancelled VFX binding preparation publishes no table", "[unit][gameplay][vfx][cook]") {
        CookingFixture fixture;
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        const auto result = GameplayVfxBindingTable::Cook(CookDocument(), fixture.registry.Snapshot(), fixture.provider,
                                                          std::span{&fixture.prepared, 1}, cancellation.Token());
        CHECK(result.HasError());
    }
}  // namespace Horo::Gameplay
