#include "Horo/Release/UpdateFileDelta.h"

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

    [[nodiscard]] UpdateStagedFile File(std::string_view path, std::string_view contents) {
        const bool entrypoint = path == "bin/game";
        return {std::string{path}, contents.size(), Horo::ComputeSha256(std::as_bytes(std::span{contents})),
                entrypoint ? UpdateFileMode::Executable : UpdateFileMode::Regular,
                entrypoint ? UpdateFileRole::Entrypoint : UpdateFileRole::Content};
    }

    [[nodiscard]] Horo::Sha256Digest InventoryDigest(std::span<const UpdateStagedFile> files) {
        auto canonical = BuildCanonicalUpdateFileInventory(files, Limits);
        REQUIRE(canonical.HasValue());
        return Horo::ComputeSha256(std::as_bytes(std::span{canonical.Value()}));
    }
}  // namespace

TEST_CASE("File delta reconstructs only changed files and binds the full target inventory", "[release][update][delta]") {
    const std::array base{File("bin/game", "old"), File("assets/keep", "same"), File("assets/removed", "gone")};
    const std::array target{File("bin/game", "new"), File("assets/keep", "same"), File("assets/added", "fresh")};
    const std::array patch{target[0], target[2]};
    auto plan = PlanUpdateFileDelta(base, target, patch, InventoryDigest(base), InventoryDigest(target), InventoryDigest(patch), Limits);
    REQUIRE(plan.HasValue());
    REQUIRE(plan.Value().targetFiles.size() == 3U);
    CHECK(plan.Value().targetFiles[0].target.path == "assets/added");
    CHECK(plan.Value().targetFiles[0].source == UpdateDeltaFileSource::VerifiedDelta);
    CHECK(plan.Value().targetFiles[1].source == UpdateDeltaFileSource::VerifiedBase);
    CHECK(plan.Value().targetFiles[2].source == UpdateDeltaFileSource::VerifiedDelta);
}

TEST_CASE("Delta ZIP carries an unchanged entrypoint while updating content", "[release][update][delta]") {
    const std::array base{File("bin/game", "same"), File("assets/level", "old")};
    const std::array target{base[0], File("assets/level", "new")};
    const std::array patch{target[0], target[1]};
    auto plan = PlanUpdateFileDelta(base, target, patch, InventoryDigest(base), InventoryDigest(target), InventoryDigest(patch), Limits);
    REQUIRE(plan.HasValue());
    CHECK(plan.Value().targetFiles[0].target.path == "assets/level");
    CHECK(plan.Value().targetFiles[0].source == UpdateDeltaFileSource::VerifiedDelta);
    CHECK(plan.Value().targetFiles[1].target.path == "bin/game");
    CHECK(plan.Value().targetFiles[1].source == UpdateDeltaFileSource::VerifiedDelta);

    const std::array unchangedExtra{target[0], target[1], File("assets/unchanged", "extra")};
    CHECK(PlanUpdateFileDelta(base, target, unchangedExtra, InventoryDigest(base), InventoryDigest(target), InventoryDigest(unchangedExtra),
                              Limits)
              .HasError());
}

TEST_CASE("Delta planning treats executable intent as part of file identity", "[release][update][delta]") {
    const std::array base{File("bin/game", "same"), File("bin/tool", "bytes")};
    auto target = base;
    target[1].mode = UpdateFileMode::Executable;
    const std::array patch{target[0], target[1]};
    auto plan = PlanUpdateFileDelta(base, target, patch, InventoryDigest(base), InventoryDigest(target), InventoryDigest(patch), Limits);
    REQUIRE(plan.HasValue());
    CHECK(plan.Value().targetFiles[1].target.path == "bin/tool");
    CHECK(plan.Value().targetFiles[1].source == UpdateDeltaFileSource::VerifiedDelta);
    const std::array missingModeChange{target[0]};
    CHECK(PlanUpdateFileDelta(base, target, missingModeChange, InventoryDigest(base), InventoryDigest(target),
                              InventoryDigest(missingModeChange), Limits)
              .HasError());
}

TEST_CASE("File delta rejects stale bases, altered patches, extra files, and unsafe names", "[release][update][delta]") {
    const std::array base{File("bin/game", "old"), File("assets/keep", "same")};
    const std::array target{File("bin/game", "new"), File("assets/keep", "same")};
    const std::array patch{target[0]};
    const auto baseDigest = InventoryDigest(base);
    const auto targetDigest = InventoryDigest(target);
    const auto deltaDigest = InventoryDigest(patch);
    REQUIRE(PlanUpdateFileDelta(base, target, patch, baseDigest, targetDigest, deltaDigest, Limits).HasValue());

    CHECK(PlanUpdateFileDelta(base, target, patch, targetDigest, targetDigest, deltaDigest, Limits).HasError());
    CHECK(PlanUpdateFileDelta(base, target, patch, baseDigest, baseDigest, deltaDigest, Limits).HasError());
    CHECK(PlanUpdateFileDelta(base, target, patch, baseDigest, targetDigest, baseDigest, Limits).HasError());
    const std::array altered{File("bin/game", "tampered")};
    CHECK(PlanUpdateFileDelta(base, target, altered, baseDigest, targetDigest, deltaDigest, Limits).HasError());
    const std::array extra{target[0], File("assets/extra", "unexpected")};
    CHECK(PlanUpdateFileDelta(base, target, extra, baseDigest, targetDigest, InventoryDigest(extra), Limits).HasError());
    const std::array unsafe{File("../escape", "new")};
    CHECK(PlanUpdateFileDelta(base, target, unsafe, baseDigest, targetDigest, deltaDigest, Limits).HasError());
}

TEST_CASE("Reconstructed delta stage must exactly match the full target inventory", "[release][update][delta]") {
    const std::array base{File("bin/game", "old")};
    const std::array target{File("bin/game", "new")};
    auto plan = PlanUpdateFileDelta(base, target, target, InventoryDigest(base), InventoryDigest(target), InventoryDigest(target), Limits);
    REQUIRE(plan.HasValue());
    const auto root = std::filesystem::temp_directory_path() /
                      ("horo-delta-stage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "bin");
    const auto write = [&root](std::string_view path, std::string_view contents) {
        std::ofstream output(root / path, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    };
    write("bin/game", "new");
    std::filesystem::permissions(root / "bin/game", std::filesystem::perms{0755});
    CHECK(VerifyUpdateFileDeltaStage(root, plan.Value(), Limits).HasValue());
    write("bin/game", "old");
    CHECK(VerifyUpdateFileDeltaStage(root, plan.Value(), Limits).HasError());
    write("bin/game", "new");
    write("bin/extra", "unexpected");
    CHECK(VerifyUpdateFileDeltaStage(root, plan.Value(), Limits).HasError());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
