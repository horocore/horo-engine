#include "Horo/Prefab/CookedPrefab.h"
#include "PrefabTestUtils.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <type_traits>

namespace Horo::Prefab {
    namespace {
        [[nodiscard]] PrefabLimitProfile Limits(PrefabProjectPolicy policy = {}) {
            auto result = PrefabLimitProfile::Create(policy);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        [[nodiscard]] Sha256Digest Digest(const std::string_view value = "source") {
            return ComputeSha256(std::as_bytes(std::span(value.data(), value.size())));
        }

        [[nodiscard]] RawComponentPayload Component(const std::uint64_t instance = 1) {
            RawComponentPayload member;
            member.instance = PrefabComponentInstanceId::Create(instance).Value();
            member.component.typeId = Gameplay::ComponentTypeId::Parse("game.sample.health").Value();
            constexpr std::string_view payload = "{\"health\":42}";
            const auto bytes = std::as_bytes(std::span(payload.data(), payload.size()));
            member.component.payload.assign(bytes.begin(), bytes.end());
            return member;
        }

        [[nodiscard]] CookedPrefabData Candidate() {
            CookedPrefabData data;
            data.assetId = Test::Asset();
            CookedPrefabEntity root;
            root.provenance = {data.assetId, {}, Digest()};
            root.members.push_back(Component());
            Gameplay::BehaviorComponent behavior;
            behavior.instanceId = {1};
            behavior.typeId = Gameplay::BehaviorTypeId::Parse("game.sample.controller").Value();
            behavior.fields = {{"empty", std::monostate{}},       {"enabled", true},
                               {"count", std::int64_t{-12}},      {"speed", 2.5},
                               {"name", std::string{"test"}},     {"uv", Math::Vec2{1, 2}},
                               {"position", Math::Vec3{3, 4, 5}}, {"rotation", Math::Quaternion{}}};
            root.members.push_back(behavior);
            CookedPrefabEntity child;
            child.parent = CookedPrefabEntitySlot{0};
            child.localTransform.translation = {1, 2, 3};
            child.provenance = {data.assetId, PrefabObjectAddress::Create({}, {7}).Value(), Digest()};
            child.members.push_back(Component(9));
            data.entities = {root, child};
            data.dependencies.push_back({{Test::Asset(2), Assets::AssetTypeId::Parse("core.mesh").Value()}, Digest("cooked mesh")});
            data.bindings.push_back({PrefabPropertyId::Create(3).Value(), std::nullopt, true});
            data.bindings.push_back(
                {PrefabPropertyId::Create(5).Value(), std::get<RawComponentPayload>(root.members[0]).component.typeId, false});
            data.references = {{{{0}, 0}, PrefabPropertyId::Create(1).Value(), CookedPrefabEntitySlot{1}},
                               {{{0}, 0}, PrefabPropertyId::Create(2).Value(), CookedPrefabMemberSlot{{1}, 0}},
                               {{{0}, 1}, PrefabPropertyId::Create(1).Value(), CookedPrefabAssetSlot{0}},
                               {{{0}, 1}, PrefabPropertyId::Create(2).Value(), CookedPrefabBindingSlot{1}}};
            return data;
        }

        [[nodiscard]] std::vector<std::byte> Encoded(const CookedPrefabData &data = Candidate()) {
            auto result = CookedPrefab::Create(data, Limits());
            REQUIRE(result.HasValue());
            return {result.Value().Bytes().begin(), result.Value().Bytes().end()};
        }

        void Word(std::vector<std::byte> &bytes, const std::size_t offset, const std::uint32_t value) {
            for (std::size_t index = 0; index < 4; ++index)
                bytes[offset + index] = static_cast<std::byte>((value >> ((3 - index) * 8U)) & 0xffU);
        }

        void Reseal(std::vector<std::byte> &bytes) {
            const auto digest = ComputeSha256(std::span(bytes).subspan(CookedPrefabHeaderBytes));
            for (std::size_t index = 0; index < digest.bytes.size(); ++index)
                bytes[32 + index] = static_cast<std::byte>(digest.bytes[index]);
        }

        [[nodiscard]] std::size_t TypeMetadataOffset(const std::vector<std::byte> &bytes, const std::string_view type) {
            const auto match = std::ranges::search(bytes, std::as_bytes(std::span(type.data(), type.size())));
            REQUIRE(!match.empty());
            return static_cast<std::size_t>(match.begin() - bytes.begin()) + type.size() + sizeof(std::uint32_t);
        }

        template <typename T> void Reject(const Result<T> &result, const ErrorCodeDescriptor &error) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == error.code.Value());
        }
    }  // namespace

    TEST_CASE("Cooked prefab retains complete typed tables and exact canonical integrity", "[prefab][cooked]") {
        const auto data = Candidate();
        auto artifact = CookedPrefab::Create(data, Limits());
        REQUIRE(artifact.HasValue());
        CHECK(artifact.Value().GetAssetId() == data.assetId);
        CHECK(artifact.Value().GetObjectCount() == 2);
        CHECK(artifact.Value().Data() == data);
        CHECK(artifact.Value().PayloadDigest() == ComputeSha256(artifact.Value().Bytes().subspan(CookedPrefabHeaderBytes)));
        CHECK(artifact.Value().PayloadDigest() != data.entities[0].provenance.sourceDigest);
        const auto decoded = CookedPrefab::Parse(artifact.Value().Bytes(), data.assetId, Limits());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().Data() == data);
        CHECK(std::ranges::equal(decoded.Value().Bytes(), artifact.Value().Bytes()));
        CHECK(Encoded(data) == Encoded(data));
        CHECK(artifact.Value().Bytes()[0] == std::byte{'H'});
        CHECK(artifact.Value().Bytes()[7] == static_cast<std::byte>(CurrentCookedPrefabVersion));
    }

    TEST_CASE("Cooked prefab owns its source-free data across candidate destruction and replacement", "[prefab][cooked]") {
        auto candidate = Candidate();
        auto original = CookedPrefab::Create(candidate, Limits());
        REQUIRE(original.HasValue());
        const auto originalBytes = Encoded(candidate);
        candidate.entities[0].localTransform.translation.x = 99;
        auto replacement = CookedPrefab::Create(candidate, Limits());
        REQUIRE(replacement.HasValue());
        candidate = {};
        CHECK(original.Value().Data().entities[0].localTransform.translation.x == 0);
        CHECK(replacement.Value().Data().entities[0].localTransform.translation.x == 99);
        CHECK(original.Value().PayloadDigest() != replacement.Value().PayloadDigest());
        auto moved = std::move(original).Value();
        CHECK(std::ranges::equal(moved.Bytes(), originalBytes));
        static_assert(std::is_same_v<decltype(moved.Data()), const CookedPrefabData &>);
        static_assert(std::is_same_v<decltype(moved.Bytes()), std::span<const std::byte>>);
        static_assert(!std::is_copy_assignable_v<CookedPrefab>);
        static_assert(!std::is_move_assignable_v<CookedPrefab>);
    }

    TEST_CASE("Cooked prefab preserves text bytes and numeric boundary bits", "[prefab][cooked]") {
        auto data = Candidate();
        auto &behavior = std::get<Gameplay::BehaviorComponent>(data.entities[0].members[1]);
        behavior.enabled = false;
        std::string text = "Türkçe 東京";
        text.push_back('\0');
        text += "suffix";
        behavior.fields = {{"text", text},
                           {"minimum", std::numeric_limits<std::int64_t>::min()},
                           {"maximum", std::numeric_limits<std::int64_t>::max()},
                           {"zero", -0.0}};
        const auto bytes = Encoded(data);
        const auto parsed = CookedPrefab::Parse(bytes, data.assetId, Limits());
        REQUIRE(parsed.HasValue());
        CHECK(parsed.Value().Data() == data);
        CHECK(std::ranges::equal(parsed.Value().Bytes(), bytes));
    }

    TEST_CASE("Cooked prefab admits exact captured payload and table bounds", "[prefab][cooked]") {
        const auto bytes = Encoded();
        PrefabProjectPolicy policy;
        policy.maximumCookedPayloadBytes = bytes.size() - CookedPrefabHeaderBytes;
        policy.maximumObjectCount = 2;
        policy.maximumHierarchyDepth = 2;
        policy.maximumComponentsPerObject = 2;
        policy.maximumReferencedAssets = 1;
        policy.maximumBindingSlots = 2;
        policy.maximumBindingUses = 4;
        CHECK(CookedPrefab::Create(Candidate(), Limits(policy)).HasValue());
        CHECK(CookedPrefab::Parse(bytes, Test::Asset(), Limits(policy)).HasValue());
        --policy.maximumCookedPayloadBytes;
        Reject(CookedPrefab::Create(Candidate(), Limits(policy)), PrefabErrors::CookPayloadTooLarge);
        Reject(CookedPrefab::Parse(bytes, Test::Asset(), Limits(policy)), PrefabErrors::CookPayloadTooLarge);
    }

    TEST_CASE("Cooked prefab accepts a minimal template and maximum flattened table capacity", "[prefab][cooked]") {
        auto data = Candidate();
        data.references.clear();
        data.bindings.clear();
        data.dependencies.clear();
        data.entities.resize(1);
        data.entities[0].members.clear();
        CHECK(CookedPrefab::Parse(Encoded(data), data.assetId, Limits()).HasValue());
        for (std::uint32_t index = 1; index < PrefabHardLimits::SourceObjectCount; ++index) {
            CookedPrefabEntity entity;
            entity.parent = CookedPrefabEntitySlot{0};
            entity.provenance = {data.assetId, PrefabObjectAddress::Create({}, {index}).Value(), Digest()};
            data.entities.push_back(entity);
        }
        const auto bytes = Encoded(data);
        CHECK(CookedPrefab::Parse(bytes, data.assetId, Limits()).HasValue());
        data.entities.push_back(data.entities.back());
        Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::ObjectCountExceeded);
    }

    TEST_CASE("Cooked prefab applies caller-lowered collection and hierarchy ceilings", "[prefab][cooked]") {
        PrefabProjectPolicy policy;
        SECTION("objects") {
            policy.maximumObjectCount = 1;
            Reject(CookedPrefab::Create(Candidate(), Limits(policy)), PrefabErrors::ObjectCountExceeded);
        }
        SECTION("depth") {
            policy.maximumHierarchyDepth = 1;
            Reject(CookedPrefab::Create(Candidate(), Limits(policy)), PrefabErrors::HierarchyDepthExceeded);
        }
        SECTION("members") {
            policy.maximumComponentsPerObject = 1;
            Reject(CookedPrefab::Create(Candidate(), Limits(policy)), PrefabErrors::ComponentCountExceeded);
        }
        SECTION("bindings") {
            policy.maximumBindingSlots = 1;
            Reject(CookedPrefab::Create(Candidate(), Limits(policy)), PrefabErrors::ReferenceCountExceeded);
        }
        SECTION("uses") {
            policy.maximumBindingUses = 3;
            Reject(CookedPrefab::Create(Candidate(), Limits(policy)), PrefabErrors::ReferenceCountExceeded);
        }
        SECTION("dependencies") {
            policy.maximumReferencedAssets = 1;
            auto data = Candidate();
            auto dependency = data.dependencies[0];
            dependency.asset.id = Test::Asset(3);
            data.dependencies.push_back(dependency);
            Reject(CookedPrefab::Create(data, Limits(policy)), PrefabErrors::ReferenceCountExceeded);
        }
    }

    TEST_CASE("Cooked prefab rejects malformed identities hierarchy and source evidence", "[prefab][cooked]") {
        auto data = Candidate();
        SECTION("template identity") {
            data.assetId = {};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::IdentityInvalid);
        }
        SECTION("empty hierarchy") {
            data.entities.clear();
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::ObjectCountExceeded);
        }
        SECTION("root parent") {
            data.entities[0].parent = CookedPrefabEntitySlot{0};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::HierarchyInvalid);
        }
        SECTION("second root") {
            data.entities[1].parent.reset();
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::HierarchyInvalid);
        }
        SECTION("forward cycle") {
            data.entities[1].parent = CookedPrefabEntitySlot{1};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::HierarchyInvalid);
        }
        SECTION("invalid source") {
            data.entities[0].provenance.sourceAsset = {};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::CookArtifactInvalid);
        }
        SECTION("missing source evidence") {
            data.entities[0].provenance.sourceDigest = {};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::CookArtifactInvalid);
        }
        SECTION("duplicate source address") {
            data.entities[1].provenance.sourceObject = {};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::CookArtifactInvalid);
        }
    }

    TEST_CASE("Cooked prefab rejects malformed transforms and portable members", "[prefab][cooked]") {
        auto data = Candidate();
        SECTION("nonfinite transform") {
            data.entities[0].localTransform.scale.x = std::numeric_limits<float>::infinity();
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::CookArtifactInvalid);
        }
        SECTION("invalid rotation") {
            data.entities[0].localTransform.rotation = {0, 0, 0, 0};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::CookArtifactInvalid);
        }
        SECTION("nonfinite field") {
            std::get<Gameplay::BehaviorComponent>(data.entities[0].members[1]).fields[3].value = std::numeric_limits<double>::quiet_NaN();
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::CookArtifactInvalid);
        }
        SECTION("duplicate member occurrence") {
            data.entities[0].members.push_back(data.entities[0].members[0]);
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::CookArtifactInvalid);
        }
        SECTION("invalid member identity") {
            std::get<RawComponentPayload>(data.entities[0].members[0]).instance = {};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::IdentityInvalid);
        }
    }

    TEST_CASE("Cooked prefab rejects ambiguous dependencies and binding declarations", "[prefab][cooked]") {
        auto data = Candidate();
        SECTION("missing dependency digest") {
            data.dependencies[0].artifactDigest = {};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::CookArtifactInvalid);
        }
        SECTION("mistyped dependency") {
            data.dependencies[0].asset.expectedType = {};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::CookArtifactInvalid);
        }
        SECTION("self dependency") {
            data.dependencies[0].asset.id = data.assetId;
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::CookArtifactInvalid);
        }
        SECTION("duplicate dependency") {
            data.dependencies.push_back(data.dependencies[0]);
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::CookArtifactInvalid);
        }
        SECTION("duplicate binding") {
            data.bindings.push_back(data.bindings[0]);
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::CookArtifactInvalid);
        }
        SECTION("untyped component binding") {
            data.bindings[0].componentType = Gameplay::ComponentTypeId{};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::CookArtifactInvalid);
        }
    }

    TEST_CASE("Cooked prefab rejects dangling and ambiguous property fixups", "[prefab][cooked]") {
        auto data = Candidate();
        SECTION("dangling owner") {
            data.references[0].owner.member = 2;
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::ReferenceRewriteInvalid);
        }
        SECTION("dangling entity") {
            data.references[0].target = CookedPrefabEntitySlot{2};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::ReferenceRewriteInvalid);
        }
        SECTION("dangling member") {
            data.references[0].target = CookedPrefabMemberSlot{{1}, 1};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::ReferenceRewriteInvalid);
        }
        SECTION("dangling asset") {
            data.references[0].target = CookedPrefabAssetSlot{1};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::ReferenceRewriteInvalid);
        }
        SECTION("dangling binding") {
            data.references[0].target = CookedPrefabBindingSlot{2};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::ReferenceRewriteInvalid);
        }
        SECTION("missing property") {
            data.references[0].property = {};
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::ReferenceRewriteInvalid);
        }
        SECTION("duplicate property fixup") {
            data.references.insert(data.references.begin(), data.references[0]);
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::ReferenceRewriteInvalid);
        }
        SECTION("unordered references") {
            std::swap(data.references[0], data.references[1]);
            Reject(CookedPrefab::Create(data, Limits()), PrefabErrors::ReferenceRewriteInvalid);
        }
    }

    TEST_CASE("Cooked prefab checks every envelope truncation and identity before decoding", "[prefab][cooked]") {
        const auto bytes = Encoded();
        for (std::size_t length = 0; length < bytes.size(); ++length)
            CHECK(CookedPrefab::Parse(std::span(bytes).first(length), Test::Asset(), Limits()).HasError());
        Reject(CookedPrefab::Parse(bytes, Test::Asset(3), Limits()), PrefabErrors::CorruptedPayload);
        Reject(CookedPrefab::Parse(bytes, {}, Limits()), PrefabErrors::CorruptedPayload);
        auto altered = bytes;
        SECTION("bad magic") {
            altered[0] = std::byte{'X'};
        }
        SECTION("payload corruption") {
            altered.back() ^= std::byte{1};
        }
        SECTION("digest corruption") {
            altered[32] ^= std::byte{1};
        }
        SECTION("false payload length") {
            Word(altered, 28, 0xffffffffU);
        }
        SECTION("trailing bytes") {
            altered.push_back(std::byte{0});
        }
        Reject(CookedPrefab::Parse(altered, Test::Asset(), Limits()), PrefabErrors::CorruptedPayload);
    }

    TEST_CASE("Cooked prefab rejects unsupported runtime formats independently from source versions", "[prefab][cooked]") {
        auto bytes = Encoded();
        Word(bytes, 4, CurrentCookedPrefabVersion + 1);
        Reject(CookedPrefab::Parse(bytes, Test::Asset(), Limits()), PrefabErrors::UnsupportedCookedVersion);
        Word(bytes, 4, 0);
        Reject(CookedPrefab::Parse(bytes, Test::Asset(), Limits()), PrefabErrors::UnsupportedCookedVersion);
    }

    TEST_CASE("Cooked prefab rejects hostile tables even with a matching payload digest", "[prefab][cooked]") {
        auto bytes = Encoded();
        SECTION("object count exceeds header bound") {
            Word(bytes, 24, 0xffffffffU);
            Reject(CookedPrefab::Parse(bytes, Test::Asset(), Limits()), PrefabErrors::ObjectCountExceeded);
        }
        SECTION("payload object count exceeds allocation bound") {
            Word(bytes, 64, 0xffffffffU);
            Reseal(bytes);
            Reject(CookedPrefab::Parse(bytes, Test::Asset(), Limits()), PrefabErrors::CorruptedPayload);
        }
        SECTION("header and payload count disagree") {
            Word(bytes, 24, 1);
            Reject(CookedPrefab::Parse(bytes, Test::Asset(), Limits()), PrefabErrors::CorruptedPayload);
        }
        SECTION("scope exceeds fixed storage") {
            Word(bytes, 160, 0xffffffffU);
            Reseal(bytes);
            Reject(CookedPrefab::Parse(bytes, Test::Asset(), Limits()), PrefabErrors::CorruptedPayload);
        }
        SECTION("member tag unknown") {
            bytes[172] = std::byte{2};
            Reseal(bytes);
            Reject(CookedPrefab::Parse(bytes, Test::Asset(), Limits()), PrefabErrors::CorruptedPayload);
        }
        SECTION("unrecognized reference tag") {
            // The final binding target is followed by the v2 initialization-table count.
            constexpr auto trailerBytes = 2 * sizeof(std::uint32_t);
            bytes[bytes.size() - trailerBytes - 1] = std::byte{4};
            Reseal(bytes);
            Reject(CookedPrefab::Parse(bytes, Test::Asset(), Limits()), PrefabErrors::CorruptedPayload);
        }
    }

    TEST_CASE("Cooked prefab rejects malformed member metadata before payload publication", "[prefab][cooked]") {
        auto bytes = Encoded();
        SECTION("unknown component encoding") {
            bytes[TypeMetadataOffset(bytes, "game.sample.health")] = std::byte{1};
        }
        SECTION("oversized component bytes") {
            Word(bytes, TypeMetadataOffset(bytes, "game.sample.health") + 1, 0xffffffffU);
        }
        SECTION("invalid behavior boolean") {
            bytes[TypeMetadataOffset(bytes, "game.sample.controller")] = std::byte{2};
        }
        Reseal(bytes);
        Reject(CookedPrefab::Parse(bytes, Test::Asset(), Limits()), PrefabErrors::CorruptedPayload);
    }
}  // namespace Horo::Prefab
