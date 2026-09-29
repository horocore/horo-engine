#include "Horo/Release/ReleaseCandidatePublisher.h"
#include "ReleaseTestFixtures.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

using namespace Horo;
using namespace Horo::Release;

namespace {
    class TemporaryDirectory final {
    public:
        TemporaryDirectory()
            : path(std::filesystem::temp_directory_path() /
                   ("horo-release-stage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(path);
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
    };

    [[nodiscard]] ReleaseExecutionPlan Plan(const std::filesystem::path &outputRoot) {
        auto request = ReleaseTestFixtures::Request();
        request.outputRoot = outputRoot;
        auto facts = ReleaseTestFixtures::Facts(request);
        auto prepared = PreflightRelease(request, facts);
        REQUIRE(prepared.issues.empty());
        REQUIRE(prepared.plan.has_value());
        return std::move(*prepared.plan);
    }

    [[nodiscard]] ReleaseArtifactManifest Manifest(const ReleaseExecutionPlan &plan, const ReleaseCandidateId candidate) {
        ReleaseArtifactManifestData data;
        data.candidate = candidate;
        data.product = plan.Request().profile.Product();
        data.version = plan.Request().version.productVersion;
        data.sourceRevision = plan.Request().version.sourceRevision;
        data.platform = plan.Request().profile.Platform();
        data.architecture = plan.Request().architecture;
        data.configuration = plan.Request().configuration;
        data.build = {"build_42"};
        data.toolchainId = plan.Request().toolchainId;
        data.frozen = plan.Identities();
        data.artifacts = {{"bin/editor", ReleaseArtifactRole::Binary, 6U, ReleaseTestFixtures::Digest("editor")}};
        auto manifest = ReleaseArtifactManifest::Create(std::move(data));
        REQUIRE(manifest.HasValue());
        return std::move(manifest).Value();
    }

    void WriteFile(const std::filesystem::path &path, const std::string_view content) {
        std::ofstream output(path, std::ios::binary);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
}  // namespace

TEST_CASE("Release candidate publisher promotes only a fully verified private stage", "[release][staging]") {
    TemporaryDirectory output;
    auto plan = Plan(output.path);
    NativeDurableFileSystem files;
    NativeReleaseCandidatePublisher publisher{files};
    auto stage = publisher.Begin(plan, ReleaseCandidateId{42U});
    REQUIRE(stage.HasValue());
    CHECK(std::filesystem::is_directory(stage.Value().StageRoot() / "bin"));
    CHECK_FALSE(std::filesystem::exists(stage.Value().FinalRoot()));
    CHECK(publisher.Begin(plan, ReleaseCandidateId{42U}).HasError());
    auto contender = publisher.Begin(plan, ReleaseCandidateId{43U});
    REQUIRE(contender.HasValue());

    WriteFile(stage.Value().StageRoot() / "bin/editor", "editor");
    auto manifest = Manifest(plan, stage.Value().Candidate());
    REQUIRE(publisher.Promote(plan, stage.Value(), manifest).HasValue());
    CHECK(std::filesystem::is_directory(stage.Value().FinalRoot()));
    CHECK_FALSE(std::filesystem::exists(stage.Value().StageRoot()));
    CHECK(VerifyReleaseArtifactTree(stage.Value().FinalRoot(), manifest).HasValue());
    CHECK(publisher.Promote(plan, stage.Value(), manifest).HasValue());
    CHECK(publisher.Promote(plan, contender.Value(), Manifest(plan, contender.Value().Candidate())).HasError());
    CHECK(std::filesystem::is_directory(contender.Value().StageRoot()));
    CHECK(publisher.Begin(plan, ReleaseCandidateId{44U}).HasError());
}

TEST_CASE("Release candidate publisher refuses a retry when published bytes changed", "[release][staging][recovery]") {
    TemporaryDirectory output;
    auto plan = Plan(output.path);
    NativeDurableFileSystem files;
    NativeReleaseCandidatePublisher publisher{files};
    auto stage = publisher.Begin(plan, ReleaseCandidateId{45U});
    REQUIRE(stage.HasValue());
    WriteFile(stage.Value().StageRoot() / "bin/editor", "editor");
    auto manifest = Manifest(plan, stage.Value().Candidate());
    REQUIRE(publisher.Promote(plan, stage.Value(), manifest).HasValue());

    WriteFile(stage.Value().FinalRoot() / "bin/editor", "change");
    CHECK(publisher.Promote(plan, stage.Value(), manifest).HasError());
    CHECK_FALSE(std::filesystem::exists(stage.Value().StageRoot()));
}

TEST_CASE("Release candidate publisher preserves the stage and final path on invalid inventory", "[release][staging]") {
    TemporaryDirectory output;
    auto plan = Plan(output.path);
    NativeDurableFileSystem files;
    NativeReleaseCandidatePublisher publisher{files};
    auto stage = publisher.Begin(plan, ReleaseCandidateId{43U});
    REQUIRE(stage.HasValue());
    WriteFile(stage.Value().StageRoot() / "bin/editor", "editor");
    WriteFile(stage.Value().StageRoot() / "unlisted.txt", "not declared");
    CHECK(publisher.Promote(plan, stage.Value(), Manifest(plan, ReleaseCandidateId{99U})).HasError());
    auto manifest = Manifest(plan, stage.Value().Candidate());
    CHECK(publisher.Promote(plan, stage.Value(), manifest).HasError());
    CHECK(std::filesystem::is_directory(stage.Value().StageRoot()));
    CHECK_FALSE(std::filesystem::exists(stage.Value().FinalRoot()));
}

TEST_CASE("Release candidate publisher never replaces an existing staged manifest", "[release][staging]") {
    TemporaryDirectory output;
    auto plan = Plan(output.path);
    NativeDurableFileSystem files;
    NativeReleaseCandidatePublisher publisher{files};
    auto stage = publisher.Begin(plan, ReleaseCandidateId{44U});
    REQUIRE(stage.HasValue());
    WriteFile(stage.Value().StageRoot() / "bin/editor", "editor");
    WriteFile(stage.Value().StageRoot() / "manifest.json", "existing metadata");
    auto manifest = Manifest(plan, stage.Value().Candidate());

    CHECK(publisher.Promote(plan, stage.Value(), manifest).HasError());
    std::ifstream existing(stage.Value().StageRoot() / "manifest.json", std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>{existing}, std::istreambuf_iterator<char>{}};
    CHECK(bytes == "existing metadata");
    CHECK_FALSE(std::filesystem::exists(stage.Value().FinalRoot()));
}
