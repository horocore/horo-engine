#include "Horo/Release/ReleasePackageProducer.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>

using namespace Horo;
using namespace Horo::Release;

namespace {
    class TemporaryDirectory final {
    public:
        TemporaryDirectory()
            : root(std::filesystem::temp_directory_path() /
                   ("horo-package-producer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(root / "source/bin");
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path root;
    };

    void WriteFile(const std::filesystem::path &path, const std::string_view bytes) {
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    ReleasePreSignInventory Inventory() {
        auto inventory = ReleasePreSignInventory::Create(ReleaseCandidateId{42U}, {{"bin/game", ReleaseArtifactRole::Binary, 4U,
                                                                                    ComputeSha256(std::as_bytes(std::span{"game", 4U}))}});
        REQUIRE(inventory.HasValue());
        return std::move(inventory).Value();
    }

    DistributionPackageSelection Selection() {
        DistributionArtifactIdentity artifact;
        artifact.product = {DistributionProductKind::GameRuntime, {}};
        artifact.version = GameProductVersion{{1U, 2U, 3U, {}, {}}};
        artifact.platform = DistributionPlatform::Linux;
        artifact.architecture = DistributionArchitecture::X64;
        artifact.build = {"build_42"};
        artifact.package = {"package_42"};
        artifact.installation = DistributionInstallationId{"game_42"};
        auto selection = ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::TarGzip);
        REQUIRE(selection.HasValue());
        return std::move(selection).Value();
    }

    class FakeProducer final : public IReleasePackageProducer {
    public:
        explicit FakeProducer(const DistributionPackageFormat format) : format_(format) {}

        [[nodiscard]] DistributionPackageFormat Format() const noexcept override {
            return format_;
        }

        [[nodiscard]] Result<ReleasePackageResult> Produce(const ReleasePackageRequest &request) override {
            ++calls;
            return Result<ReleasePackageResult>::Success(
                {request.selection.format, {{"package.tar.gz", ReleaseArtifactRole::NativePackage, 7U, {}}}});
        }

        int calls{};

    private:
        DistributionPackageFormat format_;
    };
}  // namespace

TEST_CASE("Release package dispatch selects one explicit backend after verifying frozen bytes", "[release][package]") {
    TemporaryDirectory directory;
    auto inventory = Inventory();
    WriteFile(directory.root / "source/bin/game", "game");
    ReleasePackageRequest request{Selection(), inventory, directory.root / "source", directory.root / "output"};
    FakeProducer other{DistributionPackageFormat::ZipArchive};
    FakeProducer selected{DistributionPackageFormat::TarGzip};
    IReleasePackageProducer *producers[]{&other, &selected};

    auto result = ProduceReleasePackage(request, producers);
    REQUIRE(result.HasValue());
    CHECK(result.Value().format == DistributionPackageFormat::TarGzip);
    CHECK(other.calls == 0);
    CHECK(selected.calls == 1);

    WriteFile(directory.root / "source/bin/game", "bad!");
    CHECK(ProduceReleasePackage(request, producers).HasError());
    CHECK(selected.calls == 1);
}

TEST_CASE("Release package dispatch rejects missing, duplicate, or conflicting producers", "[release][package]") {
    TemporaryDirectory directory;
    auto inventory = Inventory();
    WriteFile(directory.root / "source/bin/game", "game");
    ReleasePackageRequest request{Selection(), inventory, directory.root / "source", directory.root / "output"};
    FakeProducer first{DistributionPackageFormat::TarGzip};
    FakeProducer second{DistributionPackageFormat::TarGzip};
    IReleasePackageProducer *duplicate[]{&first, &second};
    CHECK(ProduceReleasePackage(request, duplicate).HasError());
    CHECK(first.calls == 0);
    CHECK(second.calls == 0);

    FakeProducer wrongFormat{DistributionPackageFormat::ZipArchive};
    IReleasePackageProducer *missing[]{&wrongFormat};
    CHECK(ProduceReleasePackage(request, missing).HasError());
    CHECK(wrongFormat.calls == 0);

    IReleasePackageProducer *available[]{&second};
    request.privateOutputRoot = request.sourceRoot / "nested";
    CHECK(ProduceReleasePackage(request, available).HasError());
    CHECK(second.calls == 0);

    request.privateOutputRoot = directory.root / "output";
    request.selection.format = DistributionPackageFormat::ZipArchive;
    CHECK(ProduceReleasePackage(request, available).HasError());
    CHECK(second.calls == 0);
}
