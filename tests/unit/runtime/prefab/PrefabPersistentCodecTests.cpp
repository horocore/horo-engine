#include "AllocationProbe.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabPersistentCodec.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "SceneAllocationQualification.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <optional>

namespace {
    using namespace Horo;
    using namespace Horo::Prefab;
    using namespace Horo::Runtime;
    namespace AllocationProbe = Horo::Tests::AllocationProbe;

    /** @brief Explicit metadata-only identities; these fixtures never claim issued Scene/native/provider authority. */
    template <typename Identity> Identity Id(std::uint8_t value) {
        SaveIdentityDetail::Bytes bytes{};
        bytes.back() = value;
        auto identity = Identity::FromBytes(bytes);
        REQUIRE(identity.HasValue());
        return std::move(identity).Value();
    }

    /** @brief Real canonical semantic policy used by the codec, not a second validation model. */
    PrefabLimitProfile Profile() {
        return PrefabLimitProfile::Create({}).Value();
    }

    /** @brief Creates complete typed metadata for isolated wire/fault tests; actual provider/world qualification is separate. */
    PrefabPersistentState State() {
        std::array<std::uint8_t, 16> assetBytes{};
        assetBytes.back() = 1;
        const auto asset = Assets::AssetId::FromBytes(assetBytes);
        const auto revision =
            PrefabSourceRevision{Application::ParseHoroVersion("1.2.3").Value(), ComputeSha256(std::as_bytes(std::span{"root", 4}))};
        const auto instance = Id<SavePrefabInstanceId>(2);
        const auto definition = SaveAssetId::FromBytes(asset.Bytes()).Value();
        PrefabPersistentInstance recipe;
        recipe.identity = instance;
        recipe.creator = "editor.play";
        recipe.source = {asset, ComputeSha256(std::as_bytes(std::span{"artifact", 8})), AssetCookTargetId::Parse("headless-null").Value()};
        recipe.rootSourceRevision = revision;
        recipe.placement.translation = {1, 2, 3};
        recipe.creationLineage = {asset};
        recipe.overrides = {asset, revision, {}, {}, {}, {}};
        recipe.initialization = {{{std::numeric_limits<std::uint64_t>::max()},
                                  std::string{"long owned initialization text without native handles"}},
                                 {{7}, std::int64_t{-21}}};
        recipe.members = {
            {{Id<PersistentEntityId>(3), {1}, SpawnedPersistentEntityProvenance{definition, instance}, PersistentEntityDisposition::Live},
             {asset, {}, revision.contentDigest}}};
        return {Id<SaveWorldId>(4), {recipe}};
    }

    /** @brief Measures actual public decode work including canonical re-encoding, then injects at every real allocation. */
    void RejectEveryDecodeAllocation(const std::vector<std::byte> &bytes) {
        const auto profile = Profile();
        const PrefabPersistenceLimits limits;
        {
            const auto warm = DecodePrefabPersistentState(bytes, limits, profile);
            REQUIRE(warm.HasValue());
        }
        AllocationProbe::Measurement measured;
        std::optional<Result<PrefabPersistentState>> success;
        {
            AllocationProbe::ScopedMeasurement measurement;
            success.emplace(DecodePrefabPersistentState(bytes, limits, profile));
            measured = measurement.Snapshot();
        }
        REQUIRE(success->HasValue());
        REQUIRE(measured.requests > 0);
        const auto before = bytes;
        for (std::size_t index = 0; index < measured.requests; ++index) {
            INFO(index);
            std::optional<Result<PrefabPersistentState>> result;
            {
                AllocationProbe::ScopedFailure failure{index};
                result.emplace(DecodePrefabPersistentState(bytes, limits, profile));
            }
            REQUIRE(result.has_value());
            CHECK(result->HasError());
            CHECK(bytes == before);
        }
    }

    /** @brief Exercises the SAME public typed override parser on valid/malformed input through all allocation sites. */
    void RejectEveryOverrideAllocation(std::string bytes, bool valid) {
        const auto profile = Profile();
        {
            const auto warm = ParsePrefabValueOverrides(bytes, profile);
            REQUIRE(warm.HasValue() == valid);
        }
        AllocationProbe::Measurement measured;
        {
            AllocationProbe::ScopedMeasurement measurement;
            {
                const auto parsed = ParsePrefabValueOverrides(bytes, profile);
            }
            measured = measurement.Snapshot();
        }
        REQUIRE(measured.requests > 0);
        const auto before = bytes;
        for (std::size_t index = 0; index < measured.requests; ++index) {
            INFO(index);
            std::optional<Result<PrefabValueOverrideSet>> result;
            {
                AllocationProbe::ScopedFailure failure{index};
                result.emplace(ParsePrefabValueOverrides(bytes, profile));
            }
            REQUIRE(result.has_value());
            CHECK(result->HasError());
            CHECK(bytes == before);
        }
    }
}  // namespace

TEST_CASE("Dynamic prefab metadata round trips canonical stable identity and full initialization IDs", "[prefab][save][codec]") {
    const auto profile = Profile();
    const auto state = State();
    auto encoded = EncodePrefabPersistentState(state, {}, profile);
    REQUIRE(encoded.HasValue());
    auto decoded = DecodePrefabPersistentState(encoded.Value().Bytes(), {}, profile);
    REQUIRE(decoded.HasValue());
    REQUIRE(decoded.Value().instances.size() == 1);
    const auto &recipe = decoded.Value().instances.front();
    CHECK(recipe.identity == state.instances[0].identity);
    CHECK(recipe.members[0].record.identity == state.instances[0].members[0].record.identity);
    CHECK(recipe.rootSourceRevision == state.instances[0].rootSourceRevision);
    CHECK(recipe.source.artifactDigest == state.instances[0].source.artifactDigest);
    REQUIRE(recipe.initialization.size() == 2);
    CHECK(recipe.initialization.back().id.value == std::numeric_limits<std::uint64_t>::max());
    auto reversed = state;
    std::ranges::reverse(reversed.instances[0].initialization);
    const auto permuted = EncodePrefabPersistentState(reversed, {}, profile);
    REQUIRE(permuted.HasValue());
    CHECK(std::ranges::equal(encoded.Value().Bytes(), permuted.Value().Bytes()));
}

TEST_CASE("Dynamic prefab metadata preserves explicit destroyed occurrence tombstones", "[prefab][save][codec]") {
    auto state = State();
    state.instances[0].disposition = PersistentEntityDisposition::Tombstone;
    state.instances[0].members[0].record.disposition = PersistentEntityDisposition::Tombstone;
    const auto encoded = EncodePrefabPersistentState(state, {}, Profile());
    REQUIRE(encoded.HasValue());
    const auto decoded = DecodePrefabPersistentState(encoded.Value().Bytes(), {}, Profile());
    REQUIRE(decoded.HasValue());
    CHECK(decoded.Value().instances[0].disposition == PersistentEntityDisposition::Tombstone);
    CHECK(decoded.Value().instances[0].members[0].record.disposition == PersistentEntityDisposition::Tombstone);
    state.instances[0].members[0].record.disposition = PersistentEntityDisposition::Live;
    CHECK(EncodePrefabPersistentState(state, {}, Profile()).HasError());
}

TEST_CASE("Dynamic prefab record decoding rejects hostile counts trailing data and contradictory IDs", "[prefab][save][codec]") {
    const auto encoded = EncodePrefabPersistentState(State(), {}, Profile());
    REQUIRE(encoded.HasValue());
    auto bytes = std::vector<std::byte>{encoded.Value().Bytes().begin(), encoded.Value().Bytes().end()};
    bytes.push_back(std::byte{});
    CHECK(DecodePrefabPersistentState(bytes, {}, Profile()).HasError());
    CHECK(DecodePrefabPersistentState(encoded.Value().Bytes(), {1, 1, 1}, Profile()).HasError());
    auto state = State();
    state.instances.push_back(state.instances[0]);
    CHECK(EncodePrefabPersistentState(state, {}, Profile()).HasError());
    state = State();
    state.instances[0].initialization.push_back(state.instances[0].initialization[0]);
    CHECK(EncodePrefabPersistentState(state, {}, Profile()).HasError());
    const PrefabPersistenceLimits hostile{1, 1, std::numeric_limits<std::size_t>::max()};
    CHECK(DecodePrefabPersistentState(encoded.Value().Bytes(), hostile, Profile()).HasError());
}

TEST_CASE("Dynamic prefab public codec preserves input at every actual decode allocation failure", "[prefab][save][allocation]") {
    Horo::Tests::RequireSupportedSceneAllocationInjection();
    const auto encoded = EncodePrefabPersistentState(State(), {}, Profile());
    REQUIRE(encoded.HasValue());
    RejectEveryDecodeAllocation({encoded.Value().Bytes().begin(), encoded.Value().Bytes().end()});
}

TEST_CASE("Canonical typed override parser safely retires successful and malformed DOMs under every allocation fault",
          "[prefab][override][allocation]") {
    Horo::Tests::RequireSupportedSceneAllocationInjection();
    const auto encoded = SerializePrefabValueOverrides(State().instances[0].overrides, Profile());
    REQUIRE(encoded.HasValue());
    RejectEveryOverrideAllocation(encoded.Value(), true);
    auto malformed = encoded.Value();
    malformed.pop_back();
    RejectEveryOverrideAllocation(std::move(malformed), false);
}

TEST_CASE("Dynamic prefab metadata honors exact byte boundary and rejects oversized encoded counts before allocation",
          "[prefab][save][bounds]") {
    const auto state = State();
    const auto encoded = EncodePrefabPersistentState(state, {}, Profile());
    REQUIRE(encoded.HasValue());
    PrefabPersistenceLimits exact;
    exact.maximumBytes = encoded.Value().Bytes().size();
    CHECK(EncodePrefabPersistentState(state, exact, Profile()).HasValue());
    CHECK(DecodePrefabPersistentState(encoded.Value().Bytes(), exact, Profile()).HasValue());
    --exact.maximumBytes;
    CHECK(EncodePrefabPersistentState(state, exact, Profile()).HasError());
    CHECK(DecodePrefabPersistentState(encoded.Value().Bytes(), exact, Profile()).HasError());
    CanonicalValueWriter writer;
    REQUIRE(writer.WriteUInt32(1).HasValue());
    REQUIRE(writer.WriteBytes(std::as_bytes(std::span{state.world.Bytes()})).HasValue());
    REQUIRE(writer.WriteUInt32(std::numeric_limits<std::uint32_t>::max()).HasValue());
    const auto hostile = std::move(writer).Finalize();
    REQUIRE(hostile.HasValue());
    const auto rejected = DecodePrefabPersistentState(hostile.Value().Bytes(), {}, Profile());
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == SaveErrors::CanonicalCodecLimitExceeded.code.Value());
}
