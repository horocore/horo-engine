#include "FractureDocumentFixture.h"

#include <limits>

namespace Horo::Editor {
    using namespace FractureTest;
    using namespace Destruction;

    TEST_CASE("Fracture source portable schema round-trips complete authoring settings and graph", "[unit][editor][fracture][codec]") {
        auto source = Source();
        source.settings.algorithm = FractureSourceAlgorithm::Voronoi;
        source.settings.sites = {{Id<DestructionChunkId>(10), {-1, 2, 3}}, {Id<DestructionChunkId>(20), {4, 5, 6}}};
        source.settings.exteriorUvScale = 2.5;
        source.settings.interiorUvScale = 3.5;
        source.damage.behavior.minimumDamageIntervalTicks = 12;
        const auto encoded = EncodeFractureAssetSource(source);
        REQUIRE(encoded.HasValue());
        REQUIRE(encoded.Value().size() > 8);
        CHECK(encoded.Value()[0] == std::byte{'H'});
        CHECK(encoded.Value()[1] == std::byte{'F'});
        CHECK(encoded.Value()[2] == std::byte{'R'});
        CHECK(encoded.Value()[3] == std::byte{'S'});
        CHECK(encoded.Value()[4] == std::byte{1});
        const auto decoded = DecodeFractureAssetSource(encoded.Value());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value() == source);
        const auto repeated = EncodeFractureAssetSource(decoded.Value());
        REQUIRE(repeated.HasValue());
        CHECK(repeated.Value() == encoded.Value());
    }

    TEST_CASE("Fracture source parser rejects every truncation trailing bytes and version skew", "[unit][editor][fracture][hostile]") {
        const auto encoded = EncodeFractureAssetSource(Source());
        REQUIRE(encoded.HasValue());
        for (std::size_t size = 0; size < encoded.Value().size(); ++size)
            CHECK(DecodeFractureAssetSource(std::span(encoded.Value()).first(size)).HasError());
        auto bytes = encoded.Value();
        bytes.push_back(std::byte{});
        ErrorIs(DecodeFractureAssetSource(bytes), FractureDocumentErrors::InvalidSource);
        bytes = encoded.Value();
        bytes[0] = std::byte{};
        ErrorIs(DecodeFractureAssetSource(bytes), FractureDocumentErrors::InvalidSource);
        bytes = encoded.Value();
        bytes[4] = std::byte{2};
        ErrorIs(DecodeFractureAssetSource(bytes), FractureDocumentErrors::UnsupportedVersion);
        ErrorIs(DecodeFractureAssetSource(std::vector<std::byte>(MaximumFractureSourceBytes + 1)), FractureDocumentErrors::LimitExceeded);
    }

    TEST_CASE("Fracture source wire rejects hostile counts booleans and floating encodings before open", "[unit][editor][fracture][wire]") {
        const auto encoded = EncodeFractureAssetSource(Source());
        REQUIRE(encoded.HasValue());
        auto bytes = encoded.Value();
        // Schema 1 fixture: fixed source settings precede the zero-site count at byte 166.
        SECTION("oversized site count") {
            for (std::size_t index = 166; index < 170; ++index)
                bytes[index] = std::byte{255};
        }
        SECTION("site count cannot fit remaining bytes") {
            bytes[166] = std::byte{};
            bytes[167] = std::byte{4};  // 1,024 sites are in the envelope, but absent in this capture.
        }
        SECTION("malformed chunk boolean") {
            bytes[194] = std::byte{2};
        }
        SECTION("noncanonical negative zero UV") {
            for (std::size_t index = 150; index < 158; ++index)
                bytes[index] = std::byte{};
            bytes[157] = std::byte{128};
        }
        SECTION("nonfinite UV bits") {
            bytes[156] = std::byte{240};
            bytes[157] = std::byte{127};
        }
        ErrorIs(DecodeFractureAssetSource(bytes), FractureDocumentErrors::InvalidSource);
    }

    TEST_CASE("Fracture source admits the exact high-tier chunk envelope with bounded portable output",
              "[unit][editor][fracture][ceiling]") {
        auto source = Source();
        source.settings.tier = DestructionFeatureTier::High;
        source.chunks.clear();
        source.contacts.clear();
        for (std::uint64_t id = 1; id <= DestructionHardLimits::ChunksPerDestructible; ++id)
            source.chunks.push_back({Id<DestructionChunkId>(id), {}, 0, true, true});
        const auto encoded = EncodeFractureAssetSource(source);
        REQUIRE(encoded.HasValue());
        CHECK(encoded.Value().size() < MaximumFractureSourceBytes);
        const auto decoded = DecodeFractureAssetSource(encoded.Value());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value() == source);
        source.settings.tier = DestructionFeatureTier::Standard;
        ErrorIs(ValidateFractureAssetSource(source), FractureDocumentErrors::LimitExceeded);
    }

    TEST_CASE("Fracture hierarchy validation rejects missing stable nodes cycles and tier depth", "[unit][editor][fracture][validation]") {
        auto source = Source();
        REQUIRE(ValidateFractureAssetSource(source).HasValue());
        SECTION("duplicate stable chunks") {
            source.chunks.push_back(source.chunks.back());
        }
        SECTION("missing parent") {
            source.chunks.back().parent = Id<DestructionChunkId>(99);
        }
        SECTION("hierarchy cycle") {
            source.chunks.front().parent = source.chunks.back().id;
        }
        SECTION("missing chunk identity") {
            source.chunks.front().id = {};
        }
        SECTION("unknown tier") {
            source.settings.tier = static_cast<DestructionFeatureTier>(99);
        }
        SECTION("tier hierarchy depth") {
            source.settings.tier = DestructionFeatureTier::Baseline;
        }
        CHECK(ValidateFractureAssetSource(source).HasError());
        CHECK(EncodeFractureAssetSource(source).HasError());
    }

    TEST_CASE("Fracture source validation requires exact dependency and recipe identities", "[unit][editor][fracture][validation]") {
        auto source = Source();
        REQUIRE(ValidateFractureAssetSource(source).HasValue());
        SECTION("missing dependency") {
            source.settings.sourceMesh = {};
        }
        SECTION("missing toolchain") {
            source.settings.toolchainDigest = {};
        }
        SECTION("unknown algorithm") {
            source.settings.algorithm = static_cast<FractureSourceAlgorithm>(2);
        }
        SECTION("zero recipe") {
            source.settings.recipe = {};
        }
        SECTION("missing source identity") {
            source.asset = {};
        }
        SECTION("missing source revision") {
            source.settings.sourceRevision = {};
        }
        SECTION("missing source digest") {
            source.settings.sourceDigest = {};
        }
        SECTION("missing recipe revision") {
            source.settings.recipeRevision = {};
        }
        SECTION("missing algorithm version") {
            source.settings.algorithmVersion = 0;
        }
        SECTION("nonfinite UV") {
            source.settings.interiorUvScale = std::numeric_limits<double>::infinity();
        }
        SECTION("zero UV") {
            source.settings.exteriorUvScale = 0;
        }
        CHECK(ValidateFractureAssetSource(source).HasError());
        CHECK(EncodeFractureAssetSource(source).HasError());
    }

    TEST_CASE("Fracture materials require ordered valid dependencies and complete slot references",
              "[unit][editor][fracture][validation]") {
        auto source = Source();
        REQUIRE(ValidateFractureAssetSource(source).HasValue());
        SECTION("missing material") {
            source.chunks.front().materialSlot = 99;
        }
        SECTION("duplicate material slot") {
            source.materials.push_back(source.materials.front());
        }
        SECTION("missing material identity") {
            source.materials.front().asset = {};
        }
        SECTION("missing material digest") {
            source.materials.front().digest = {};
        }
        SECTION("missing interior material") {
            source.settings.interiorMaterialSlot = 99;
        }
        CHECK(ValidateFractureAssetSource(source).HasError());
        CHECK(EncodeFractureAssetSource(source).HasError());
    }

    TEST_CASE("Fracture contacts require canonical endpoints finite weights and anchored required support",
              "[unit][editor][fracture][validation]") {
        auto source = Source();
        REQUIRE(ValidateFractureAssetSource(source).HasValue());
        SECTION("invalid edge order") {
            std::swap(source.contacts.front().low, source.contacts.front().high);
        }
        SECTION("duplicate edge") {
            source.contacts.push_back(source.contacts.front());
        }
        SECTION("zero edge weight") {
            source.contacts.front().weight = 0;
        }
        SECTION("unknown edge endpoint") {
            source.contacts.front().high = Id<DestructionChunkId>(99);
        }
        SECTION("required chunk disconnected from anchors") {
            source.contacts.clear();
        }
        SECTION("required graph without anchors") {
            source.chunks.front().anchor = false;
        }
        SECTION("nonfinite weight") {
            source.contacts.front().weight = std::numeric_limits<double>::quiet_NaN();
        }
        CHECK(ValidateFractureAssetSource(source).HasError());
        CHECK(EncodeFractureAssetSource(source).HasError());
    }

    TEST_CASE("Fracture damage and feature intent rejects invalid policies without fallback", "[unit][editor][fracture][validation]") {
        auto source = Source();
        REQUIRE(ValidateFractureAssetSource(source).HasValue());
        SECTION("nonfinite damage") {
            source.damage.health.maximumHealth = std::numeric_limits<float>::quiet_NaN();
        }
        SECTION("invalid thresholds") {
            source.damage.health.fractureHealthThreshold = 99;
        }
        SECTION("unknown damage policy") {
            source.damage.behavior.trigger = static_cast<DestructionTriggerPolicy>(99);
        }
        SECTION("unknown required feature") {
            source.settings.requiredFeatures.bits |= 0x80000000U;
        }
        SECTION("unsupported runtime cutting") {
            source.settings.requiredFeatures.bits |= DestructionFeatureBit<DestructionFeature::RuntimeGeometryGeneration>;
        }
        CHECK(ValidateFractureAssetSource(source).HasError());
        CHECK(EncodeFractureAssetSource(source).HasError());
    }

    TEST_CASE("Fracture source graph and site envelopes reject oversized or inconsistent input", "[unit][editor][fracture][bounds]") {
        auto source = Source();
        SECTION("chunk ceiling") {
            source.chunks.resize(DestructionHardLimits::ChunksPerDestructible + 1);
        }
        SECTION("edge ceiling") {
            source.contacts.resize(MaximumFractureContacts + 1);
        }
        SECTION("material ceiling") {
            source.materials.resize(MaximumFractureMaterials + 1);
        }
        SECTION("site ceiling") {
            source.settings.sites.resize(DestructionHardLimits::ChunksPerDestructible + 1);
        }
        ErrorIs(ValidateFractureAssetSource(source), FractureDocumentErrors::LimitExceeded);
    }

    TEST_CASE("Fracture explicit Voronoi sites must be complete stable finite and distinct", "[unit][editor][fracture][sites]") {
        auto source = Source();
        source.settings.algorithm = FractureSourceAlgorithm::Voronoi;
        source.settings.sites = {{Id<DestructionChunkId>(10), {0, 0, 0}}, {Id<DestructionChunkId>(20), {1, 0, 0}}};
        REQUIRE(ValidateFractureAssetSource(source).HasValue());
        SECTION("missing site") {
            source.settings.sites.pop_back();
        }
        SECTION("duplicate position") {
            source.settings.sites.back().position = source.settings.sites.front().position;
        }
        SECTION("wrong stable identity") {
            source.settings.sites.back().chunk = Id<DestructionChunkId>(99);
        }
        SECTION("nonfinite site") {
            source.settings.sites.back().position[2] = std::numeric_limits<double>::quiet_NaN();
        }
        SECTION("sites in imported source") {
            source.settings.algorithm = FractureSourceAlgorithm::PreFractured;
        }
        ErrorIs(ValidateFractureAssetSource(source), FractureDocumentErrors::InvalidSource);
    }

    TEST_CASE("Fracture material removal is transactional and oversized patch leaves source unchanged", "[unit][editor][fracture][patch]") {
        auto document = Document();
        const auto before = document.Snapshot();
        ErrorIs(document.Apply({{RemoveFractureMaterial{0}}}, Context(document)), FractureDocumentErrors::InvalidSource);
        CHECK(document.Snapshot().source == before.source);
        FractureSourcePatch patch;
        patch.operations.resize(MaximumFracturePatchOperations + 1, SetFractureDamage{Source().damage});
        ErrorIs(document.Apply(patch, Context(document)), FractureDocumentErrors::LimitExceeded);
        auto settings = Source().settings;
        settings.sites.resize(DestructionHardLimits::ChunksPerDestructible + 1);
        ErrorIs(document.Apply({{SetFractureSettings{settings}}}, Context(document)), FractureDocumentErrors::LimitExceeded);
        CHECK(document.Snapshot().source == before.source);
        const FractureSourceMaterial material{1, Asset(11), Digest(12)};
        const FractureSourcePatch addition{{PutFractureMaterial{material}}};
        REQUIRE(document.Apply(addition, Context(document)).HasValue());
        REQUIRE(document.Apply({{RemoveFractureMaterial{1}}}, Context(document)).HasValue());
        REQUIRE(document.Undo(Context(document)).HasValue());
        CHECK(document.Snapshot().source->materials.size() == 2);
        REQUIRE(document.Redo(Context(document)).HasValue());
        CHECK(document.Snapshot().source->materials.size() == 1);
    }
}  // namespace Horo::Editor
