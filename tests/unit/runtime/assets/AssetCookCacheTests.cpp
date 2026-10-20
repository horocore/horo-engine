#include "../../../support/NativeCacheFiles.h"
#include "../../../support/OwnedTestDirectory.h"
#include "../../../support/RecordingImmutableFiles.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetCookCache.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Sha256.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Assets;

    AssetId Id(const std::string_view value) {
        auto parsed = AssetId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    AssetTypeId Type(const std::string_view value) {
        auto parsed = AssetTypeId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    AssetCookTargetId Target(const std::string_view value) {
        auto parsed = AssetCookTargetId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    Sha256Digest DigestOf(std::span<const std::uint8_t> bytes) {
        return ComputeSha256(std::as_bytes(bytes));
    }

    AssetCookCacheKeyInputs MakeInputs(std::string_view sourceData, std::string_view cookerId = "test.cooker",
                                       std::string_view cookerVer = "1.0.0") {
        auto srcBytes = std::vector<std::uint8_t>(sourceData.begin(), sourceData.end());
        return AssetCookCacheKeyInputs{
            .assetId = Id("00000000-0000-0000-0000-000000000001"),
            .assetType = Type("core.mesh"),
            .sourceDigest = DigestOf(srcBytes),
            .metadataDigest = DigestOf(srcBytes),  // simplified: using source as metadata
            .metadataSchemaVersion = 1,
            .settingsDigest = Sha256Digest{},  // zero digest = no settings
            .settingsSchemaVersion = 0,
            .cookerContributionId = cookerId,
            .cookerVersion = cookerVer,
            .target = Target("headless-null"),
            .artifactFormatVersion = AssetCookArtifact::CurrentFormatVersion,
        };
    }

}  // namespace

TEST_CASE("BuildAssetCookCacheKey produces different keys for different inputs", "[native]") {
    auto inputs1 = MakeInputs("hello");
    auto inputs2 = MakeInputs("world");

    auto key1 = BuildAssetCookCacheKey(inputs1);
    auto key2 = BuildAssetCookCacheKey(inputs2);

    REQUIRE((key1.digest.bytes != key2.digest.bytes));
}

TEST_CASE("BuildAssetCookCacheKey is deterministic", "[native]") {
    auto inputs = MakeInputs("deterministic");

    auto key1 = BuildAssetCookCacheKey(inputs);
    auto key2 = BuildAssetCookCacheKey(inputs);

    REQUIRE((key1.digest.bytes == key2.digest.bytes));
    REQUIRE((key1 == key2));
}

TEST_CASE("Cooker version change invalidates cache key", "[native]") {
    auto inputs1 = MakeInputs("data", "test.cooker", "1.0.0");
    auto inputs2 = MakeInputs("data", "test.cooker", "1.1.0");

    auto key1 = BuildAssetCookCacheKey(inputs1);
    auto key2 = BuildAssetCookCacheKey(inputs2);

    REQUIRE((key1.digest.bytes != key2.digest.bytes));
}

TEST_CASE("Cooker contribution ID change invalidates cache key", "[native]") {
    auto inputs1 = MakeInputs("data", "cooker.a", "1.0.0");
    auto inputs2 = MakeInputs("data", "cooker.b", "1.0.0");

    auto key1 = BuildAssetCookCacheKey(inputs1);
    auto key2 = BuildAssetCookCacheKey(inputs2);

    REQUIRE((key1.digest.bytes != key2.digest.bytes));
}

TEST_CASE("Target change invalidates cache key", "[native]") {
    auto inputs1 = MakeInputs("data");
    auto inputs2 = MakeInputs("data");
    inputs2.target = Target("desktop-opengl");

    auto key1 = BuildAssetCookCacheKey(inputs1);
    auto key2 = BuildAssetCookCacheKey(inputs2);

    REQUIRE((key1.digest.bytes != key2.digest.bytes));
}

TEST_CASE("AssetCookCache miss returns nullopt", "[native]") {
    Tests::OwnedTestDirectory tmp{"horo-cache-test"};
    const auto cacheFiles = Horo::TestSupport::NativeCacheFiles(tmp.Path());
    AssetCookCache cache(*cacheFiles);
    CancellationToken cancellation;

    auto inputs = MakeInputs("miss-test");
    auto key = BuildAssetCookCacheKey(inputs);

    auto result = cache.Load(key, cancellation);
    REQUIRE((result.HasValue()));
    REQUIRE((!result.Value().has_value()));
}

TEST_CASE("AssetCookCache write then hit returns bytes", "[native]") {
    Tests::OwnedTestDirectory tmp{"horo-cache-test"};
    const auto cacheFiles = Horo::TestSupport::NativeCacheFiles(tmp.Path());
    AssetCookCache cache(*cacheFiles);
    CancellationToken cancellation;

    std::vector<std::uint8_t> artifact = {'H', 'O', 'R', 'O', 0x01, 0x02, 0x03, 0x04};
    auto inputs = MakeInputs("write-hit");
    auto key = BuildAssetCookCacheKey(inputs);

    // Store
    auto storeResult = cache.Store(key, artifact, cancellation);
    REQUIRE((storeResult.HasValue()));

    // Load
    auto loadResult = cache.Load(key, cancellation);
    REQUIRE((loadResult.HasValue()));
    REQUIRE((loadResult.Value().has_value()));
    REQUIRE((loadResult.Value().value() == artifact));
}

TEST_CASE("AssetCookCache creates missing directories only when storing an entry", "[native]") {
    Tests::OwnedTestDirectory tmp{"horo-cache-test"};
    const auto root = tmp.Path() / "missing cache ü" / "nested";
    const auto cacheFiles = Horo::TestSupport::NativeCacheFiles(root);
    const AssetCookCache cache(*cacheFiles);
    REQUIRE_FALSE(std::filesystem::exists(root.parent_path()));
    const auto key = BuildAssetCookCacheKey(MakeInputs("lazy-cache"));
    const auto miss = cache.Load(key, {});
    REQUIRE(miss.HasValue());
    CHECK_FALSE(miss.Value().has_value());
    CHECK_FALSE(std::filesystem::exists(root.parent_path()));

    CancellationSource cancelled;
    cancelled.RequestCancellation();
    const std::vector<std::uint8_t> bytes{1, 2, 3};
    CHECK(cache.Store(key, bytes, cancelled.Token()).HasError());
    CHECK_FALSE(std::filesystem::exists(root.parent_path()));

    REQUIRE(cache.Store(key, bytes, {}).HasValue());
    const auto hit = cache.Load(key, {});
    REQUIRE(hit.HasValue());
    REQUIRE(hit.Value().has_value());
    CHECK(*hit.Value() == bytes);
}

TEST_CASE("AssetCookCache double store is idempotent", "[native]") {
    Tests::OwnedTestDirectory tmp{"horo-cache-test"};
    const auto cacheFiles = Horo::TestSupport::NativeCacheFiles(tmp.Path());
    AssetCookCache cache(*cacheFiles);
    CancellationToken cancellation;

    std::vector<std::uint8_t> artifact = {0xAA, 0xBB, 0xCC};
    auto inputs = MakeInputs("idempotent");
    auto key = BuildAssetCookCacheKey(inputs);

    auto store1 = cache.Store(key, artifact, cancellation);
    REQUIRE((store1.HasValue()));

    auto store2 = cache.Store(key, artifact, cancellation);
    REQUIRE((store2.HasValue()));

    // Loaded bytes still match
    auto loadResult = cache.Load(key, cancellation);
    REQUIRE((loadResult.HasValue()));
    REQUIRE((loadResult.Value().has_value()));
    REQUIRE((loadResult.Value().value() == artifact));
}

TEST_CASE("AssetCookCache honours cancellation on load", "[native]") {
    Tests::OwnedTestDirectory tmp{"horo-cache-test"};
    const auto cacheFiles = Horo::TestSupport::NativeCacheFiles(tmp.Path());
    AssetCookCache cache(*cacheFiles);

    CancellationSource cancelSource;
    cancelSource.RequestCancellation();
    auto cancellation = cancelSource.Token();

    auto inputs = MakeInputs("cancel-load");
    auto key = BuildAssetCookCacheKey(inputs);

    auto result = cache.Load(key, cancellation);
    REQUIRE((result.HasError()));
}

TEST_CASE("AssetCookCache honours cancellation on store", "[native]") {
    Tests::OwnedTestDirectory tmp{"horo-cache-test"};
    const auto cacheFiles = Horo::TestSupport::NativeCacheFiles(tmp.Path());
    AssetCookCache cache(*cacheFiles);

    CancellationSource cancelSource;
    cancelSource.RequestCancellation();
    auto cancellation = cancelSource.Token();

    std::vector<std::uint8_t> artifact = {0x01, 0x02};
    auto inputs = MakeInputs("cancel-store");
    auto key = BuildAssetCookCacheKey(inputs);

    auto result = cache.Store(key, artifact, cancellation);
    REQUIRE((result.HasError()));
}

TEST_CASE("BuildAssetCookCacheKey schema version change invalidates key", "[native]") {
    auto inputs1 = MakeInputs("schema-test");
    inputs1.metadataSchemaVersion = 1;

    auto inputs2 = MakeInputs("schema-test");
    inputs2.metadataSchemaVersion = 2;

    auto key1 = BuildAssetCookCacheKey(inputs1);
    auto key2 = BuildAssetCookCacheKey(inputs2);

    REQUIRE((key1.digest.bytes != key2.digest.bytes));
}

TEST_CASE("BuildAssetCookCacheKey artifact format version change invalidates key", "[native]") {
    auto inputs1 = MakeInputs("format-test");
    inputs1.artifactFormatVersion = 1;

    auto inputs2 = MakeInputs("format-test");
    inputs2.artifactFormatVersion = 2;

    auto key1 = BuildAssetCookCacheKey(inputs1);
    auto key2 = BuildAssetCookCacheKey(inputs2);

    REQUIRE((key1.digest.bytes != key2.digest.bytes));
}

TEST_CASE("Dependency cache identity is canonical and separately versioned", "[native][prefab-cook]") {
    const auto inputs = MakeInputs("scene");
    const auto &semantic = inputs.sourceDigest;
    std::vector<AssetCookDependencyIdentity> dependencies{{Id("00000000-0000-0000-0000-000000000002"), Type("core.mesh"), semantic},
                                                          {Id("00000000-0000-0000-0000-000000000003"), Type("core.mesh"), semantic}};
    const auto key = BuildAssetCookCacheKeyV2(inputs, dependencies, semantic, 2);
    REQUIRE(key.HasValue());
    REQUIRE(key.Value() != BuildAssetCookCacheKey(inputs));
    std::swap(dependencies[0], dependencies[1]);
    const auto reordered = BuildAssetCookCacheKeyV2(inputs, dependencies, semantic, 2);
    REQUIRE(reordered.HasValue());
    REQUIRE(reordered.Value() == key.Value());

    SECTION("dependency bytes") {
        dependencies[0].artifactDigest.bytes[0] = static_cast<std::uint8_t>(dependencies[0].artifactDigest.bytes[0] + 1U);
    }
    SECTION("dependency type") {
        dependencies[0].type = Type("core.material");
    }
    const auto changed = BuildAssetCookCacheKeyV2(inputs, dependencies, semantic, 2);
    REQUIRE(changed.HasValue());
    REQUIRE(changed.Value() != key.Value());
}

TEST_CASE("Dependency cache identity binds semantic context and rejects ambiguous or oversized input", "[native][prefab-cook]") {
    const auto inputs = MakeInputs("scene");
    auto semantic = inputs.sourceDigest;
    const AssetCookDependencyIdentity dependency{Id("00000000-0000-0000-0000-000000000002"), Type("core.mesh"), semantic};
    const std::vector<AssetCookDependencyIdentity> dependencies{dependency};
    const auto key = BuildAssetCookCacheKeyV2(inputs, dependencies, semantic, 1);
    REQUIRE(key.HasValue());
    semantic.bytes[0] = static_cast<std::uint8_t>(semantic.bytes[0] + 1U);
    const auto changed = BuildAssetCookCacheKeyV2(inputs, dependencies, semantic, 1);
    REQUIRE(changed.HasValue());
    REQUIRE(changed.Value() != key.Value());
    const std::vector<AssetCookDependencyIdentity> duplicates{dependency, dependency};
    REQUIRE(BuildAssetCookCacheKeyV2(inputs, duplicates, semantic, 2).HasError());
    REQUIRE(BuildAssetCookCacheKeyV2(inputs, duplicates, semantic, 1).HasError());
    REQUIRE(BuildAssetCookCacheKeyV2(inputs, dependencies, semantic, 0).HasError());
}

TEST_CASE("Cache delegates exact digest paths and byte bounds to the supplied authority", "[cache][authority]") {
    TestSupport::RecordingImmutableFiles files;
    const AssetCookCache cache{files, {.maximumArtifactBytes = 4}};
    const AssetCookCacheKey key{};
    const std::string expected = "00/" + std::string(62, '0') + ".cooked";
    const auto miss = cache.Load(key, {});
    REQUIRE(miss.HasValue());
    CHECK_FALSE(miss.Value());
    CHECK(files.loadedPath == expected);
    CHECK(files.bound == 4);
    CHECK(files.stores == 0);
    files.response = std::vector<std::uint8_t>{3, 2, 1};
    const auto hit = cache.Load(key, {});
    REQUIRE(hit.HasValue());
    CHECK(hit.Value() == files.response);
    REQUIRE(cache.Store(key, *files.response, {}).HasValue());
    CHECK(files.storedPath == expected);
    CHECK(files.stored == *files.response);
}

TEST_CASE("Cache preserves provider failures and nested causes without another storage route", "[cache][authority]") {
    TestSupport::RecordingImmutableFiles files;
    const auto cause = MakeError(FileStorageErrors::UnsafeObject);
    files.loadError = WrapError(FileStorageErrors::IoFailed, cause);
    files.storeError = MakeError(FileStorageErrors::Conflict);
    const AssetCookCache cache{files};
    const auto loaded = cache.Load({}, {});
    REQUIRE(loaded.HasError());
    CHECK(loaded.ErrorValue().code.Value() == FileStorageErrors::IoFailed.code.Value());
    REQUIRE(loaded.ErrorValue().cause.Get());
    CHECK(loaded.ErrorValue().cause.Get()->code.Value() == cause.code.Value());
    const auto stored = cache.Store({}, {}, {});
    REQUIRE(stored.HasError());
    CHECK(stored.ErrorValue().domain.Value() == FileStorageErrors::Conflict.domain.Value());
    CHECK(stored.ErrorValue().code.Value() == FileStorageErrors::Conflict.code.Value());
    CHECK(files.loads == 1);
    CHECK(files.stores == 1);
}

TEST_CASE("Cache rejects cancelled or oversized requests before invoking storage", "[cache][authority]") {
    TestSupport::RecordingImmutableFiles files;
    const AssetCookCache cache{files, {.maximumArtifactBytes = 2}};
    CancellationSource stop;
    stop.RequestCancellation();
    CHECK(cache.Load({}, stop.Token()).HasError());
    CHECK(cache.Store({}, {}, stop.Token()).HasError());
    const std::vector<std::uint8_t> oversized{1, 2, 3};
    CHECK(cache.Store({}, oversized, {}).HasError());
    const AssetCookCache disabled{files, {.maximumArtifactBytes = 0}};
    CHECK(disabled.Load({}, {}).HasError());
    CHECK(disabled.Store({}, {}, {}).HasError());
    CHECK(files.loads == 0);
    CHECK(files.stores == 0);
    files.response = oversized;
    CHECK(cache.Load({}, {}).HasError());
    CHECK(files.loads == 1);
}
