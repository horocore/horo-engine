#include "Horo/Release/ReleaseBuildProvenance.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string_view>
#include <vector>

using namespace Horo;
using namespace Horo::Release;

namespace {
    class TemporaryDirectory final {
    public:
        TemporaryDirectory()
            : path(std::filesystem::temp_directory_path() /
                   ("horo-release-provenance-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
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

    ReleaseBuildProvenanceData Fixture() {
        ReleaseBuildProvenanceData data;
        data.sourceEpochSeconds = 1'700'000'000U;
        data.features = {"renderer", "audio"};
        data.environment = {{"SDK_VERSION", {}}};
        data.files = {{"bin/game", 4U, {}}, {"assets/archive.bin", 6U, {}}};
        return data;
    }

    ReleaseBuildProvenanceData Inputs() {
        auto data = Fixture();
        data.files.clear();
        return data;
    }
}  // namespace

TEST_CASE("Unsigned provenance capture hashes real files and explains changed bytes", "[release][provenance]") {
    TemporaryDirectory directory;
    WriteFile(directory.path / "a/bin/game", "game");
    WriteFile(directory.path / "a/assets/data", "assets");
    WriteFile(directory.path / "b/assets/data", "assets");
    WriteFile(directory.path / "b/bin/game", "game");

    auto first = CaptureReleaseBuildProvenance(directory.path / "a", Inputs());
    auto second = CaptureReleaseBuildProvenance(directory.path / "b", Inputs());
    REQUIRE(first.HasValue());
    REQUIRE(second.HasValue());
    CHECK(first.Value().Digest() == second.Value().Digest());
    CHECK(first.Value().CanonicalJson().find(directory.path.string()) == std::string::npos);

    WriteFile(directory.path / "b/bin/game", "new!");
    auto changed = CaptureReleaseBuildProvenance(directory.path / "b", Inputs());
    REQUIRE(changed.HasValue());
    CHECK(first.Value().Compare(changed.Value()) == std::vector<ReleaseBuildVariance>{{"files.bin/game"}});
}

TEST_CASE("Unsigned provenance capture rejects unsafe tree entries", "[release][provenance]") {
    TemporaryDirectory directory;
    WriteFile(directory.path / "payload", "ok");
    CHECK(CaptureReleaseBuildProvenance(directory.path, Fixture()).HasError());

    std::error_code error;
    std::filesystem::create_symlink(directory.path / "payload", directory.path / "link", error);
    if (!error)
        CHECK(CaptureReleaseBuildProvenance(directory.path, Inputs()).HasError());
}

TEST_CASE("Unsigned provenance canonicalizes independent of inventory order", "[release][provenance]") {
    auto first = ReleaseBuildProvenance::Create(Fixture());
    REQUIRE(first.HasValue());
    auto secondData = Fixture();
    std::ranges::reverse(secondData.files);
    std::ranges::reverse(secondData.features);
    auto second = ReleaseBuildProvenance::Create(std::move(secondData));
    REQUIRE(second.HasValue());
    CHECK(first.Value().Digest() == second.Value().Digest());
    auto parsed = ReleaseBuildProvenance::ParseCanonical(first.Value().CanonicalJson());
    REQUIRE(parsed.HasValue());
    CHECK(parsed.Value().Compare(first.Value()).empty());
}

TEST_CASE("Unsigned provenance reports precise input and file variance", "[release][provenance]") {
    auto first = ReleaseBuildProvenance::Create(Fixture());
    REQUIRE(first.HasValue());
    auto changed = Fixture();
    changed.sourceEpochSeconds += 1U;
    changed.environment.front().digest.bytes[0] = 1U;
    changed.files.front().size += 1U;
    changed.files.push_back({"new.txt", 0U, {}});
    auto second = ReleaseBuildProvenance::Create(std::move(changed));
    REQUIRE(second.HasValue());
    const auto variance = first.Value().Compare(second.Value());
    CHECK(variance == std::vector<ReleaseBuildVariance>{{"normalization.sourceEpochSeconds"},
                                                        {"environment.SDK_VERSION"},
                                                        {"files.bin/game"},
                                                        {"files.new.txt"}});
}

TEST_CASE("Unsigned provenance rejects unsafe paths and noncanonical bytes", "[release][provenance]") {
    auto data = Fixture();
    data.files.front().path = "../secret";
    CHECK(ReleaseBuildProvenance::Create(std::move(data)).HasError());
    data = Fixture();
    data.files.push_back({"BIN/GAME", 4U, {}});
    CHECK(ReleaseBuildProvenance::Create(std::move(data)).HasError());
    auto valid = ReleaseBuildProvenance::Create(Fixture());
    REQUIRE(valid.HasValue());
    auto noncanonical = valid.Value().CanonicalJson();
    noncanonical.insert(1U, " ");
    CHECK(ReleaseBuildProvenance::ParseCanonical(noncanonical).HasError());
    auto secret = Fixture();
    secret.environment.front().name = "PATH=/home/user";
    CHECK(ReleaseBuildProvenance::Create(std::move(secret)).HasError());
}
