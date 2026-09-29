#include "Horo/Release/UpdateDeltaStaging.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>

using namespace Horo::Release;

namespace {
    constexpr UpdateArchiveLimits Limits{32U, 1024U, 4096U, 0U};

    struct PrivateTree final {
        std::filesystem::path root;

        PrivateTree()
            : root(std::filesystem::temp_directory_path() /
                   ("horo-delta-reconstruction-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directory(root);
        }

        ~PrivateTree() {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }

        PrivateTree(const PrivateTree &) = delete;
        PrivateTree &operator=(const PrivateTree &) = delete;
    };

    [[nodiscard]] UpdateStagedFile File(const std::string_view path, const std::string_view contents) {
        const bool entrypoint = path == "bin/game";
        return {std::string{path}, contents.size(), Horo::ComputeSha256(std::as_bytes(std::span{contents})),
                entrypoint ? UpdateFileMode::Executable : UpdateFileMode::Regular,
                entrypoint ? UpdateFileRole::Entrypoint : UpdateFileRole::Content};
    }

    [[nodiscard]] Horo::Sha256Digest InventoryDigest(const std::span<const UpdateStagedFile> files) {
        auto canonical = BuildCanonicalUpdateFileInventory(files, Limits);
        REQUIRE(canonical.HasValue());
        return Horo::ComputeSha256(std::as_bytes(std::span{canonical.Value()}));
    }

    void Write(const std::filesystem::path &root, const std::string_view relative, const std::string_view contents) {
        const auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        REQUIRE(output.good());
        output.close();
        std::filesystem::permissions(path, relative == "bin/game" ? std::filesystem::perms{0755} : std::filesystem::perms{0644});
    }
}  // namespace

TEST_CASE("Private delta reconstruction produces the exact full target without removed base files", "[release][update][delta]") {
    PrivateTree privateTree;
    const auto baseRoot = privateTree.root / "base";
    const auto deltaRoot = privateTree.root / "delta";
    const auto stageRoot = privateTree.root / "candidate";
    std::filesystem::create_directory(baseRoot);
    std::filesystem::create_directory(deltaRoot);
    Write(baseRoot, "bin/game", "old");
    Write(baseRoot, "assets/keep", "same");
    Write(baseRoot, "assets/removed", "gone");
    Write(deltaRoot, "bin/game", "new");
    Write(deltaRoot, "assets/added", "fresh");
    const std::array base{File("bin/game", "old"), File("assets/keep", "same"), File("assets/removed", "gone")};
    const std::array target{File("bin/game", "new"), File("assets/keep", "same"), File("assets/added", "fresh")};
    const std::array delta{target[0], target[2]};
    auto plan = PlanUpdateFileDelta(base, target, delta, InventoryDigest(base), InventoryDigest(target), InventoryDigest(delta), Limits);
    REQUIRE(plan.HasValue());
    Horo::NativeDurableFileSystem files;
    CHECK(VerifyUpdateStagedTree(baseRoot, base, Limits).HasValue());
    CHECK(VerifyUpdateStagedTree(deltaRoot, delta, Limits).HasValue());
    CHECK(ReconstructUpdateFileDeltaStage(plan.Value(), baseRoot, deltaRoot, stageRoot, Limits, files, {}).HasValue());
    CHECK(VerifyUpdateFileDeltaStage(stageRoot, plan.Value(), Limits).HasValue());
    CHECK(!std::filesystem::exists(stageRoot / "assets/removed"));
    CHECK(std::filesystem::exists(stageRoot / "assets/added"));
}

TEST_CASE("Private delta reconstruction leaves no partial destination after invalid bytes or cancellation", "[release][update][delta]") {
    PrivateTree privateTree;
    const auto baseRoot = privateTree.root / "base";
    const auto deltaRoot = privateTree.root / "delta";
    const auto stageRoot = privateTree.root / "candidate";
    std::filesystem::create_directory(baseRoot);
    std::filesystem::create_directory(deltaRoot);
    Write(baseRoot, "bin/game", "old");
    Write(deltaRoot, "bin/game", "tampered");
    const std::array base{File("bin/game", "old")};
    const std::array target{File("bin/game", "new")};
    auto plan = PlanUpdateFileDelta(base, target, target, InventoryDigest(base), InventoryDigest(target), InventoryDigest(target), Limits);
    REQUIRE(plan.HasValue());
    Horo::NativeDurableFileSystem files;
    CHECK(ReconstructUpdateFileDeltaStage(plan.Value(), baseRoot, deltaRoot, stageRoot, Limits, files, {}).HasError());
    CHECK(!std::filesystem::exists(stageRoot));

    Write(deltaRoot, "bin/game", "new");
    Horo::CancellationSource cancellation;
    cancellation.RequestCancellation();
    CHECK(ReconstructUpdateFileDeltaStage(plan.Value(), baseRoot, deltaRoot, stageRoot, Limits, files, cancellation.Token()).HasError());
    CHECK(!std::filesystem::exists(stageRoot));

    Write(stageRoot, "sentinel", "do not remove");
    CHECK(ReconstructUpdateFileDeltaStage(plan.Value(), baseRoot, deltaRoot, stageRoot, Limits, files, {}).HasError());
    CHECK(std::filesystem::exists(stageRoot / "sentinel"));
    CHECK(ReconstructUpdateFileDeltaStage(plan.Value(), baseRoot, deltaRoot, baseRoot / "nested", Limits, files, {}).HasError());

    const auto alias = privateTree.root / "base-alias";
    std::error_code linkError;
    std::filesystem::create_directory_symlink(baseRoot, alias, linkError);
    if (!linkError) {
        CHECK(
            ReconstructUpdateFileDeltaStage(plan.Value(), baseRoot, deltaRoot, alias / "bin" / "candidate", Limits, files, {}).HasError());
        CHECK_FALSE(std::filesystem::exists(baseRoot / "bin/candidate"));
        CHECK(VerifyUpdateStagedTree(baseRoot, base, Limits).HasValue());
    }
}
