#include "navigation/NavigationProjectBakeSourceFixture.h"

#include <algorithm>

namespace SourceTest = Horo::Application::NavigationSourceTestSupport;
using namespace Horo;
using namespace Horo::Application;

namespace {
    /** @brief Records query deadlines while forwarding every read to the same real retained native root. */
    class ObservedFiles final : public Platform::IProjectReadFiles {
    public:
        explicit ObservedFiles(std::shared_ptr<const Platform::IProjectReadFiles> files) : files_(std::move(files)) {}

        /** @brief Returns recorded forwarded query deadlines without exposing mutable spy storage. */
        std::span<const std::chrono::steady_clock::time_point> Deadlines() const noexcept {
            return deadlines_;
        }

        /** @brief Starts a separate verification observation while retaining the same native root owner. */
        void ClearDeadlines() noexcept {
            deadlines_.clear();
        }

        /** @brief Arms cancellation for the next synchronous native source read, after initial capture. */
        void StopDuringRead(std::shared_ptr<const CancellationSource> stop) {
            stopDuringRead_ = std::move(stop);
        }

        Result<Platform::ProjectReadManifest> Files(std::string_view path, const Platform::ProjectReadContext &context,
                                                    const Platform::ProjectReadLimits &limits) const override {
            deadlines_.push_back(context.deadline);
            return files_->Files(path, context, limits);
        }

        Result<Platform::ProjectReadTextSnapshot> Text(std::string_view path, const Platform::ProjectReadContext &context,
                                                       const Platform::ProjectReadLimits &limits) const override {
            deadlines_.push_back(context.deadline);
            return files_->Text(path, context, limits);
        }

        Result<Platform::ProjectInventory> Inventory(std::string_view path, const Platform::ProjectInventoryDepth depth,
                                                     const Platform::ProjectReadContext &context,
                                                     const Platform::ProjectInventoryLimits &limits) const override {
            deadlines_.push_back(context.deadline);
            return files_->Inventory(path, depth, context, limits);
        }

        Result<Platform::ProjectReadTextSnapshot> InventoryText(std::string_view path,
                                                                const Platform::ProjectReadContext &context) const override {
            deadlines_.push_back(context.deadline);
            return files_->InventoryText(path, context);
        }

        Result<Platform::ProjectReadTextSnapshot> SourceText(std::string_view path, const Platform::ProjectReadContext &context,
                                                             const Platform::ProjectSourceReadLimits &limits) const override {
            deadlines_.push_back(context.deadline);
            if (stopDuringRead_)
                stopDuringRead_->RequestCancellation();
            return files_->SourceText(path, context, limits);
        }

    private:
        mutable std::vector<std::chrono::steady_clock::time_point> deadlines_;
        std::shared_ptr<const Platform::IProjectReadFiles> files_;
        std::shared_ptr<const CancellationSource> stopDuringRead_;
    };
}  // namespace

TEST_CASE("Navigation automation captures complete registered project Scene and NAVDEF01 source without editor or writes",
          "[navigation][source][automation]") {
    SourceTest::Project project;
    const auto source = project.Capture();
    CHECK(source->Project().metadata.projectId == "navigation-capture");
    CHECK(source->Settings().Input().definition == SourceTest::Asset(1));
    CHECK(source->Registry().Records().size() == 3);
    REQUIRE(source->Scenes().size() == 2);
    CHECK(source->Scenes()[0].asset == SourceTest::Asset(2));
    CHECK(source->Scenes()[1].asset == SourceTest::Asset(3));
    REQUIRE(source->Definitions().size() == 1);
    CHECK(source->Definitions().front().definition->Profiles().size() == 2);
    CHECK(source->Definitions().front().digest != Sha256Digest{});
    CHECK_FALSE(source->Packages().intent);
    CHECK_FALSE(source->Packages().lock);
    CHECK(source->VerifyCurrent().HasValue());
    for (const auto path : {".horo/cache", ".horo/cooked", ".horo/asset-index.json"})
        CHECK_FALSE(std::filesystem::exists(project.directory.Path() / path));
}

TEST_CASE("Navigation source verification fences source bytes registry names and optional package presence",
          "[navigation][source][automation]") {
    SourceTest::Project project;
    const auto source = project.Capture();
    SECTION("semantically identical Scene bytes still changed") {
        project.Write("Assets/first.horo", SceneSource::EncodeSceneSource({{}, {}}) + "\n");
    }
    SECTION("definition bytes") {
        project.Write("Assets/navigation.horoasset", "not HNAV");
    }
    SECTION("package intent appeared") {
        project.Write(".horo/packages.json", "{}");
    }
    SECTION("package lock appeared") {
        project.Write(".horo/packages.lock", "{}");
    }
    SECTION("registered Scene appeared") {
        project.Scene("Assets/third.horo", SourceTest::Asset(4));
    }
    SECTION("registered Scene moved with stable identity") {
        const auto root = project.directory.Path();
        std::filesystem::rename(root / "Assets/first.horo", root / "Assets/moved.horo");
        std::filesystem::rename(root / "Assets/first.horo.horo", root / "Assets/moved.horo.horo");
    }
    CHECK(source->VerifyCurrent().HasError());
    CHECK(source->Scenes().size() == 2);
    CHECK(source->Definitions().front().definition->Profiles().size() == 2);
}

TEST_CASE("Navigation project capture fails whole rather than dropping source beyond selected limits", "[navigation][source][automation]") {
    SourceTest::Project project;
    NavigationProjectSourceLimits limits;
    SECTION("Scene count") {
        limits.maximumScenes = 1;
    }
    SECTION("raw input bytes") {
        limits.maximumCapturedBytes = 1;
    }
    SECTION("invalid duration") {
        limits.inventory.maximumDuration = std::chrono::milliseconds::max();
    }
    SECTION("zero definition limit") {
        limits.maximumDefinitions = 0;
    }
    const auto captured = NavigationProjectBakeSource::Capture(project.files, project.Context(), project.Compatibility(), limits);
    CHECK(captured.HasError());
    CHECK_FALSE(std::filesystem::exists(project.directory.Path() / ".horo/cooked"));
}

TEST_CASE("Navigation project capture refuses migration stale compatibility and unavailable default definition",
          "[navigation][source][automation]") {
    SourceTest::Project project;
    auto compatibility = project.Compatibility();
    SECTION("migration required") {
        compatibility.status = ProjectCompatibilityStatus::AutomaticMigrationRequired;
    }
    SECTION("marker required") {
        compatibility.markerUpdateRequired = true;
    }
    SECTION("inspection from another project") {
        compatibility.metadata->projectId = "different-project";
    }
    SECTION("malformed definition") {
        project.Write("Assets/navigation.horoasset", "{}");
    }
    SECTION("missing definition") {
        std::filesystem::remove(project.directory.Path() / "Assets/navigation.horoasset");
        std::filesystem::remove(project.directory.Path() / "Assets/navigation.horoasset.horo");
    }
    CHECK(NavigationProjectBakeSource::Capture(project.files, project.Context(), compatibility).HasError());
}

TEST_CASE("Navigation source retains cancellation and revocation until final source verification", "[navigation][source][automation]") {
    SourceTest::Project project;
    auto context = project.Context();
    CancellationSource cancel;
    context.cancellation = cancel.Token();
    const auto revoked = std::make_shared<bool>();
    context.authorityStopped = [revoked] {
        return *revoked;
    };
    const auto captured = NavigationProjectBakeSource::Capture(project.files, context, project.Compatibility());
    REQUIRE(captured.HasValue());
    SECTION("cancel") {
        cancel.RequestCancellation();
    }
    SECTION("opening revoked") {
        *revoked = true;
    }
    CHECK(captured.Value()->VerifyCurrent().HasError());
    CHECK(NavigationProjectBakeSource::Capture(project.files, context, project.Compatibility()).HasError());
}

TEST_CASE("Navigation source cannot follow replacement symbolic source or metadata aliases", "[navigation][source][automation]") {
    SourceTest::Project project;
    const auto captured = project.Capture();
    Tests::OwnedTestDirectory outside{"navigation external source"};
    const auto path = project.directory.Path() / "Assets/first.horo";
    std::filesystem::remove(path);
    std::error_code error;
    std::filesystem::create_symlink(outside.Path() / "first.horo", path, error);
    if (error)
        SKIP("The test filesystem cannot create symbolic links.");
    CHECK(captured->VerifyCurrent().HasError());
    CHECK(NavigationProjectBakeSource::Capture(project.files, project.Context(), project.Compatibility()).HasError());
}

TEST_CASE("Navigation capture never renews its phase deadline during initial complete revalidation", "[navigation][source][automation]") {
    SourceTest::Project project;
    const auto observed = std::make_shared<ObservedFiles>(project.files);
    auto context = project.Context();
    context.deadline += std::chrono::minutes{1};
    const auto source = NavigationProjectBakeSource::Capture(observed, context, project.Compatibility());
    REQUIRE(source.HasValue());
    REQUIRE(observed->Deadlines().size() > 10);
    const auto first = observed->Deadlines().front();
    CHECK(first < context.deadline);
    CHECK(std::ranges::all_of(observed->Deadlines(), [first](const auto deadline) {
        return deadline == first;
    }));
    observed->ClearDeadlines();
    const auto verificationStarted = std::chrono::steady_clock::now();
    REQUIRE(source.Value()->VerifyCurrent().HasValue());
    REQUIRE_FALSE(observed->Deadlines().empty());
    CHECK(observed->Deadlines().front() >= verificationStarted);
    CHECK(observed->Deadlines().back() <= context.deadline);
}

TEST_CASE("Source verification preserves captured cancellation as well as additional operation cancellation",
          "[navigation][source][automation][cancellation]") {
    SourceTest::Project project;
    CancellationSource capturedStop;
    CancellationSource operationStop;
    auto context = project.Context();
    context.cancellation = capturedStop.Token();
    const auto source = NavigationProjectBakeSource::Capture(project.files, context, project.Compatibility());
    REQUIRE(source.HasValue());
    SECTION("captured opening cancellation") {
        capturedStop.RequestCancellation();
    }
    SECTION("operation cancellation") {
        operationStop.RequestCancellation();
    }
    const auto result = source.Value()->ValidateCurrent(operationStop.Token());
    REQUIRE(result.HasError());
    CHECK(ErrorChainContains(result.ErrorValue(), Navigation::NavigationErrors::BakeInputCancelled.domain,
                             Navigation::NavigationErrors::BakeInputCancelled.code));
    CHECK(ErrorChainContains(result.ErrorValue(), Platform::ProjectReadErrors::Cancelled.domain,
                             Platform::ProjectReadErrors::Cancelled.code));
}

TEST_CASE("Operation cancellation during actual source reads is observed without renewing captured authority",
          "[navigation][source][automation][cancellation]") {
    SourceTest::Project project;
    const auto files = std::make_shared<ObservedFiles>(project.files);
    const auto context = project.Context();
    const auto source = NavigationProjectBakeSource::Capture(files, context, project.Compatibility());
    REQUIRE(source.HasValue());
    const auto stop = std::make_shared<CancellationSource>();
    files->ClearDeadlines();
    files->StopDuringRead(stop);
    const auto result = source.Value()->ValidateCurrent(stop->Token());
    REQUIRE(result.HasError());
    CHECK(ErrorChainContains(result.ErrorValue(), Navigation::NavigationErrors::BakeInputCancelled.domain,
                             Navigation::NavigationErrors::BakeInputCancelled.code));
    CHECK(ErrorChainContains(result.ErrorValue(), Platform::ProjectReadErrors::Cancelled.domain,
                             Platform::ProjectReadErrors::Cancelled.code));
    CHECK(stop->Token().IsCancellationRequested());
    REQUIRE_FALSE(files->Deadlines().empty());
    for (const auto deadline : files->Deadlines())
        CHECK(deadline <= context.deadline);
}
