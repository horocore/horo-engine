#include "Horo/Release/ReleaseArtifactManifest.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>

using namespace Horo;
using namespace Horo::Release;

namespace {
    class TemporaryDirectory final {
    public:
        TemporaryDirectory()
            : path(std::filesystem::temp_directory_path() /
                   ("horo-release-manifest-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(path);
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
    };

    void WriteFile(const std::filesystem::path &path, const std::string_view bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    ReleaseArtifactManifestData Fixture() {
        ReleaseArtifactManifestData data;
        data.candidate = ReleaseCandidateId{42U};
        data.product = {DistributionProductKind::GameRuntime, {}};
        data.version = GameProductVersion{{1U, 2U, 3U, {}, {}}};
        data.sourceRevision = {"abc123"};
        data.platform = DistributionPlatform::Linux;
        data.architecture = DistributionArchitecture::X64;
        data.configuration = ReleaseBuildConfiguration::Shipping;
        data.build = {"build_42"};
        data.toolchainId = "clang_20";
        data.runtimeFeatures = {"network", "audio"};
        data.artifacts = {{"bin/game", ReleaseArtifactRole::Binary, 12U, {}}, {"assets.horo", ReleaseArtifactRole::AssetArchive, 28U, {}}};
        return data;
    }
}  // namespace

TEST_CASE("Final release manifest canonicalizes inventory and round-trips exact bytes", "[release][manifest]") {
    auto first = ReleaseArtifactManifest::Create(Fixture());
    REQUIRE(first.HasValue());
    auto second = ReleaseArtifactManifest::ParseCanonical(first.Value().CanonicalJson());
    REQUIRE(second.HasValue());
    CHECK(first.Value().CanonicalJson() == second.Value().CanonicalJson());
    CHECK(first.Value().Digest() == second.Value().Digest());
    CHECK(first.Value().Artifacts().front().path == "assets.horo");
}

TEST_CASE("Final release manifest rejects duplicate files and noncanonical or unknown schema", "[release][manifest]") {
    auto duplicate = Fixture();
    duplicate.artifacts.push_back(duplicate.artifacts.front());
    CHECK(ReleaseArtifactManifest::Create(std::move(duplicate)).HasError());

    auto valid = ReleaseArtifactManifest::Create(Fixture());
    REQUIRE(valid.HasValue());
    auto modified = valid.Value().CanonicalJson();
    modified.insert(1U, "\"unexpected\":true,");
    CHECK(ReleaseArtifactManifest::ParseCanonical(modified).HasError());
    modified = valid.Value().CanonicalJson();
    modified.replace(modified.find("assets.horo"), 11U, "../bad-file");
    CHECK(ReleaseArtifactManifest::ParseCanonical(modified).HasError());
}

TEST_CASE("Final release manifest preserves signed evidence and namespaced extensions", "[release][manifest]") {
    auto data = Fixture();
    data.signing = ReleaseManifestSigning{"ed25519", "horo", "release_key", {}};
    data.extensions.push_back({"com.horo.compat", 1U, R"({"networkProtocolVersion":3})"});
    data.artifacts.push_back({"notices/empty.txt", ReleaseArtifactRole::Notice, 0U, ComputeSha256({})});

    auto signedManifest = ReleaseArtifactManifest::Create(std::move(data));
    REQUIRE(signedManifest.HasValue());
    CHECK(signedManifest.Value().CanonicalJson().find("networkProtocolVersion") != std::string::npos);
    CHECK(ReleaseArtifactManifest::ParseCanonical(signedManifest.Value().CanonicalJson()).HasValue());
}

TEST_CASE("Final release manifest rejects traversal, duplicate identities, and invalid extension bytes", "[release][manifest]") {
    auto traversal = Fixture();
    traversal.artifacts.front().path = "bin/../game";
    CHECK(ReleaseArtifactManifest::Create(std::move(traversal)).HasError());

    auto duplicateFeature = Fixture();
    duplicateFeature.runtimeFeatures.push_back("audio");
    CHECK(ReleaseArtifactManifest::Create(std::move(duplicateFeature)).HasError());

    auto portableCollision = Fixture();
    portableCollision.artifacts.push_back({"BIN/GAME", ReleaseArtifactRole::Binary, 12U, {}});
    CHECK(ReleaseArtifactManifest::Create(std::move(portableCollision)).HasError());

    auto reservedPath = Fixture();
    reservedPath.artifacts.front().path = "bin/CON.txt";
    CHECK(ReleaseArtifactManifest::Create(std::move(reservedPath)).HasError());

    auto invalidExtension = Fixture();
    invalidExtension.extensions.push_back({"com.horo.invalid", 1U, R"({ "x": 1 })"});
    CHECK(ReleaseArtifactManifest::Create(std::move(invalidExtension)).HasError());

    auto invalidSigning = Fixture();
    invalidSigning.signing = ReleaseManifestSigning{"bad algorithm", "horo", "release_key", {}};
    CHECK(ReleaseArtifactManifest::Create(std::move(invalidSigning)).HasError());
}

TEST_CASE("Final release tree verification accounts for every published byte", "[release][manifest]") {
    TemporaryDirectory directory;
    auto data = Fixture();
    data.artifacts = {{"bin/game", ReleaseArtifactRole::Binary, 4U, ComputeSha256(std::as_bytes(std::span{"game", 4U}))},
                      {"assets.horo", ReleaseArtifactRole::AssetArchive, 6U, ComputeSha256(std::as_bytes(std::span{"assets", 6U}))}};
    auto manifest = ReleaseArtifactManifest::Create(std::move(data));
    REQUIRE(manifest.HasValue());
    WriteFile(directory.path / "manifest.json", manifest.Value().CanonicalJson());
    WriteFile(directory.path / "bin/game", "game");
    WriteFile(directory.path / "assets.horo", "assets");
    CHECK(VerifyReleaseArtifactTree(directory.path, manifest.Value()).HasValue());

    WriteFile(directory.path / "bin/game", "bad!");
    CHECK(VerifyReleaseArtifactTree(directory.path, manifest.Value()).HasError());
    WriteFile(directory.path / "bin/game", "game");
    WriteFile(directory.path / "unlisted.txt", "unexpected");
    CHECK(VerifyReleaseArtifactTree(directory.path, manifest.Value()).HasError());
    std::filesystem::remove(directory.path / "unlisted.txt");
    std::filesystem::remove(directory.path / "assets.horo");
    CHECK(VerifyReleaseArtifactTree(directory.path, manifest.Value()).HasError());
}

TEST_CASE("Pre-sign inventory remains separate from the post-sign manifest", "[release][manifest][staging]") {
    std::vector<ReleaseArtifactRecord> unsignedFiles{{"bin/game", ReleaseArtifactRole::Binary, 4U,
                                                      ComputeSha256(std::as_bytes(std::span{"game", 4U}))},
                                                     {"assets.horo", ReleaseArtifactRole::AssetArchive, 6U,
                                                      ComputeSha256(std::as_bytes(std::span{"assets", 6U}))}};
    auto inventory = ReleasePreSignInventory::Create(ReleaseCandidateId{42U}, unsignedFiles);
    REQUIRE(inventory.HasValue());
    CHECK(inventory.Value().Artifacts().front().path == "assets.horo");
    auto parsed = ReleasePreSignInventory::ParseCanonical(inventory.Value().CanonicalJson());
    REQUIRE(parsed.HasValue());
    CHECK(parsed.Value().Digest() == inventory.Value().Digest());
    CHECK(ReleaseArtifactManifest::ParseCanonical(inventory.Value().CanonicalJson()).HasError());

    auto noncanonical = inventory.Value().CanonicalJson();
    noncanonical.insert(1U, "\"unexpected\":true,");
    CHECK(ReleasePreSignInventory::ParseCanonical(noncanonical).HasError());
    unsignedFiles.push_back({"BIN/GAME", ReleaseArtifactRole::Binary, 4U, unsignedFiles.front().digest});
    CHECK(ReleasePreSignInventory::Create(ReleaseCandidateId{42U}, std::move(unsignedFiles)).HasError());
}

TEST_CASE("Pre-sign verification rejects changed or undeclared staged bytes", "[release][manifest][staging]") {
    TemporaryDirectory directory;
    auto inventory = ReleasePreSignInventory::Create(ReleaseCandidateId{42U}, {{"bin/game", ReleaseArtifactRole::Binary, 4U,
                                                                                ComputeSha256(std::as_bytes(std::span{"game", 4U}))}});
    REQUIRE(inventory.HasValue());
    WriteFile(directory.path / "bin/game", "game");
    CHECK(VerifyReleaseStagedTree(directory.path, inventory.Value()).HasValue());

    WriteFile(directory.path / "bin/game", "other");
    CHECK(VerifyReleaseStagedTree(directory.path, inventory.Value()).HasError());
    WriteFile(directory.path / "bin/game", "game");
    WriteFile(directory.path / "manifest.json", "premature final metadata");
    CHECK(VerifyReleaseStagedTree(directory.path, inventory.Value()).HasError());
    std::filesystem::remove(directory.path / "manifest.json");
    WriteFile(directory.path / "unlisted.txt", "unexpected");
    CHECK(VerifyReleaseStagedTree(directory.path, inventory.Value()).HasError());
}
