#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "NativePublicationFiles.h"
#include "OwnedTestDirectory.h"

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <nlohmann/json.hpp>
#include <stdexcept>

using namespace Horo;
using namespace Horo::Assets;

namespace {
    class PublicationFiles final : public TestSupport::NativePublicationFiles {
    public:
        std::size_t writes{}, promotions{}, failWrite{}, failPromotion{};
        std::function<void()> beforePromotion;

        Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                         std::filesystem::perms permissions) override {
            if (++writes == failWrite)
                return Result<void>::Failure(MakeError(AssetSourceErrors::ReadFailed));
            return native.WritePrivateDurable(path, bytes, created, permissions);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &stage, const std::filesystem::path &path,
                                          AtomicFileReplacementReceipt &receipt) override {
            if (beforePromotion)
                beforePromotion();
            if (++promotions == failPromotion)
                return Result<void>::Failure(MakeError(AssetSourceErrors::ReadFailed));
            return native.AtomicReplaceTracked(stage, path, receipt);
        }
    };

    AssetId Identity(std::string_view text = "00000000-0000-0000-0000-000000000071") {
        return AssetId::Parse(text).Value();
    }

    struct Project final {
        Tests::OwnedTestDirectory directory{"authored-asset-publication"};
        PublicationFiles files;
        Editor::ProjectMutationCoordinator mutations{files};
        AssetRegistry registry;
        Application::ProjectAssetImportCommitter committer{mutations, files, &registry};
        Application::PreparedAuthoredProjectAssetBatch batch{.operationId = "authored-asset-test",
                                                             .projectRoot = directory.Path(),
                                                             .items = {{Identity(),
                                                                        AssetTypeId::Parse("test.authored").Value(),
                                                                        ProjectPath::Parse("assets/layers/test.horoasset").Value(),
                                                                        {std::byte{0x12}, std::byte{0x34}}}}};

        std::filesystem::path Payload() const {
            return directory.Path() / batch.items.front().destination.String();
        }

        std::filesystem::path Sidecar() const {
            auto path = Payload();
            path += ".horo";
            return path;
        }

        void CheckUnpublished() const {
            CHECK_FALSE(std::filesystem::exists(Payload()));
            CHECK_FALSE(std::filesystem::exists(Sidecar()));
            CHECK(registry.Snapshot().Records().empty());
        }
    };
}  // namespace

TEST_CASE("Authored assets publish ordinary identity without invented import provenance", "[terrain][authored-publication]") {
    Project project;
    auto second = project.batch.items.front();
    second.asset = Identity("00000000-0000-0000-0000-000000000072");
    second.destination = ProjectPath::Parse("assets/layers/second.horoasset").Value();
    project.batch.items.push_back(second);
    bool observedStage{}, hiddenUntilComplete = true;
    project.files.beforePromotion = [&] {
        observedStage = true;
        hiddenUntilComplete = hiddenUntilComplete && project.registry.Snapshot().Records().empty();
    };
    REQUIRE(project.committer.CommitAuthored(project.batch).HasValue());
    REQUIRE(observedStage);
    REQUIRE(hiddenUntilComplete);
    const auto payload = CaptureAssetSource(project.directory.Path(), project.Payload(), 4096);
    REQUIRE(payload.HasValue());
    CHECK(payload.Value().bytes == project.batch.items.front().payload);
    const auto sidecar = project.files.ReadFileBounded(project.Sidecar(), 4096);
    REQUIRE(sidecar.HasValue());
    const std::string text(sidecar.Value().begin(), sidecar.Value().end());
    const auto metadata = nlohmann::json::parse(text);
    CHECK(metadata.size() == 3);
    CHECK(metadata.at("schemaVersion") == 1);
    CHECK(metadata.at("assetId") == Identity().ToString());
    CHECK(metadata.at("assetType") == project.batch.items.front().type.Value());
    const auto identity = DecodeAssetIdentitySidecar(project.batch.items.front().destination.String(), text);
    REQUIRE(identity.HasValue());
    CHECK(identity.Value().id == Identity());
    const auto provenance = ReadAssetImportMetadata(project.Sidecar());
    REQUIRE(provenance.HasValue());
    CHECK(provenance.Value().importerContributionId.empty());
    CHECK(provenance.Value().absoluteSourcePath.empty());
    CHECK(provenance.Value().sourceHash.empty());
    CHECK(provenance.Value().sourceByteSize == 0);
    REQUIRE(project.registry.Snapshot().Find(Identity()) != nullptr);
    REQUIRE(project.registry.Snapshot().Find(second.asset) != nullptr);
    CHECK(project.registry.Snapshot().Records().size() == 2);
    const auto secondSource = CaptureAssetSource(project.directory.Path(), project.directory.Path() / second.destination.String(), 4096);
    REQUIRE(secondSource.HasValue());
    CHECK(secondSource.Value().bytes == second.payload);
    CHECK(project.committer.LastReceipt().registryPublished);
    CHECK(project.committer.LastReceipt().status == Application::ProjectAssetImportStatus::Published);
}

TEST_CASE("Authored publication never overwrites preexisting source or identity sidecar", "[terrain][authored-publication]") {
    Project project;
    SECTION("Payload already exists") {
        std::filesystem::create_directories(project.Payload().parent_path());
        REQUIRE(project.files.WriteDurable(project.Payload(), project.batch.items.front().payload).HasValue());
    }
    SECTION("Sidecar already exists") {
        std::filesystem::create_directories(project.Sidecar().parent_path());
        REQUIRE(project.files.WriteDurable(project.Sidecar(), project.batch.items.front().payload).HasValue());
    }
    const auto payload = CaptureAssetSource(project.directory.Path(), project.Payload(), 4096);
    const auto sidecar = CaptureAssetSource(project.directory.Path(), project.Sidecar(), 4096);
    REQUIRE(payload.HasValue());
    REQUIRE(sidecar.HasValue());
    REQUIRE(project.committer.CommitAuthored(project.batch).HasError());
    CHECK(CaptureAssetSource(project.directory.Path(), project.Payload(), 4096).Value().identity == payload.Value().identity);
    CHECK(CaptureAssetSource(project.directory.Path(), project.Sidecar(), 4096).Value().identity == sidecar.Value().identity);
    CHECK(project.files.writes == 0);
    CHECK(project.registry.Snapshot().Records().empty());
}

TEST_CASE("Authored source failure compensates only owned files before registry publication", "[terrain][authored-publication]") {
    Project project;
    SECTION("Complete sidecar cannot be privately staged") {
        project.files.failWrite = 2;
    }
    SECTION("Sidecar native promotion fails after payload commits") {
        project.files.failPromotion = 2;
    }
    SECTION("Native publication callback throws before its commit") {
        project.files.beforePromotion = [] {
            throw std::runtime_error("native publication failure");
        };
    }
    REQUIRE(project.committer.CommitAuthored(project.batch).HasError());
    project.CheckUnpublished();
    CHECK(project.committer.LastReceipt().promotedFiles == project.committer.LastReceipt().rolledBackFiles);
}

TEST_CASE("Authored late index failure retains the complete published registry generation", "[terrain][authored-publication]") {
    Project project;
    REQUIRE(std::filesystem::create_directories(project.directory.Path() / ".horo/asset_index.json"));
    const auto result = project.committer.CommitAuthored(project.batch);
    REQUIRE(result.HasError());
    CHECK(project.committer.LastReceipt().registryPublished);
    REQUIRE(project.registry.Snapshot().Find(Identity()) != nullptr);
    CHECK(std::filesystem::exists(project.Payload()));
    CHECK(std::filesystem::exists(project.Sidecar()));
    CHECK(project.committer.LastReceipt().rolledBackFiles == 0);
}

TEST_CASE("Authored identity ambiguity preserves previous sources and registry", "[terrain][authored-publication]") {
    Project project;
    SECTION("Duplicate identity inside a batch") {
        auto duplicate = project.batch.items.front();
        duplicate.destination = ProjectPath::Parse("assets/layers/duplicate.horoasset").Value();
        project.batch.items.push_back(std::move(duplicate));
        REQUIRE(project.committer.CommitAuthored(project.batch).HasError());
        project.CheckUnpublished();
        CHECK(project.files.writes == 0);
    }
    SECTION("Identity already belongs to a different published source") {
        REQUIRE(project.committer.CommitAuthored(project.batch).HasValue());
        const auto original = project.Payload();
        const auto source = CaptureAssetSource(project.directory.Path(), original, 4096);
        REQUIRE(source.HasValue());
        const auto revision = project.registry.Snapshot().Revision();
        project.batch.items.front().destination = ProjectPath::Parse("assets/layers/duplicate.horoasset").Value();
        REQUIRE(project.committer.CommitAuthored(project.batch).HasError());
        CHECK_FALSE(std::filesystem::exists(project.Payload()));
        CHECK_FALSE(std::filesystem::exists(project.Sidecar()));
        CHECK(CaptureAssetSource(project.directory.Path(), original, 4096).Value().identity == source.Value().identity);
        CHECK(project.registry.Snapshot().Revision() == revision);
        REQUIRE(project.registry.Snapshot().Find(Identity()) != nullptr);
    }
}

TEST_CASE("Authored cancellation and reentrancy keep publication ownership explicit", "[terrain][authored-publication]") {
    Project project;
    CancellationSource cancellation;
    SECTION("Precancelled admission performs no native writes") {
        cancellation.RequestCancellation();
        REQUIRE(project.committer.CommitAuthored(project.batch, cancellation.Token()).HasError());
        project.CheckUnpublished();
        CHECK(project.files.writes == 0);
    }
    SECTION("Cancellation at a native boundary compensates committed owned payload") {
        project.files.beforePromotion = [&] {
            cancellation.RequestCancellation();
        };
        REQUIRE(project.committer.CommitAuthored(project.batch, cancellation.Token()).HasError());
        project.CheckUnpublished();
        CHECK(project.committer.LastReceipt().promotedFiles == project.committer.LastReceipt().rolledBackFiles);
    }
    SECTION("Nested publication is rejected without clobbering outer receipt") {
        bool attempted{}, rejected{};
        project.files.beforePromotion = [&] {
            attempted = true;
            rejected = project.committer.CommitAuthored(project.batch).HasError();
        };
        REQUIRE(project.committer.CommitAuthored(project.batch).HasValue());
        CHECK(attempted);
        CHECK(rejected);
        CHECK(project.committer.LastReceipt().registryPublished);
        CHECK(project.committer.LastReceipt().status == Application::ProjectAssetImportStatus::Published);
    }
}
