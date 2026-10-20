#include "Horo/Foundation/ImmutableFileStorage.h"
#include "navigation/NavigationBakeServiceFixture.h"

#include <atomic>

namespace Horo::Application {
    namespace {
        using namespace Horo::Navigation;
        using namespace Horo::Navigation::TestSupport;
        using namespace BakeTestSupport;

        /** @brief Submits a rebuild selection while retaining the complete definition capture. */
        Result<OperationId> Selected(const BakeHarness &harness, std::vector<SurfaceId> surfaces) {
            return harness.service->Submit(Request(harness, NavigationBakeExecutionMode::BuildAndPublish, std::move(surfaces)));
        }

        /** @brief The worker observes only the injected selector result; the test reads its counter after terminal completion. */
        class SelectorObservationFiles final : public Horo::TestSupport::NativePublicationFiles {
        public:
            explicit SelectorObservationFiles(const bool reject) : reject_(reject) {}

            const Error failure{WrapError(FileStorageErrors::IoFailed, MakeError(FileStorageErrors::UnsafeObject))};
            mutable std::atomic<std::size_t> observations{};

            Result<std::optional<FileEntryInfo>> InspectEntry(const std::filesystem::path &path) const override {
                if (path.filename() != "current.json")
                    return native.InspectEntry(path);
                ++observations;
                return reject_ ? Result<std::optional<FileEntryInfo>>::Failure(failure)
                               : Result<std::optional<FileEntryInfo>>::Success(std::nullopt);
            }

        private:
            const bool reject_;
        };
    }  // namespace

    TEST_CASE("Selected-surface admission rejects ambiguous empty and absent surface selections", "[navigation][cook][selection]") {
        BakeHarness harness;
        auto surfaces = std::vector{Id<SurfaceId>(1)};
        SECTION("empty") {
            surfaces.clear();
        }
        SECTION("invalid identity") {
            surfaces = {SurfaceId{}};
        }
        SECTION("duplicate") {
            surfaces.push_back(surfaces.front());
        }
        SECTION("absent from complete closure") {
            surfaces = {Id<SurfaceId>(2)};
        }
        SECTION("selection count exceeds capture ceiling") {
            surfaces.assign(NavigationBakeInputLimits::MaximumSurfaceBindings + 1U, Id<SurfaceId>(1));
        }
        const auto rejected = Selected(harness, std::move(surfaces));
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == NavigationErrors::BakeInputInvalid.code.Value());
        CHECK(harness.builder->builds.load() == 0);
        CHECK_FALSE(harness.service->Published());
    }

    TEST_CASE("Selected-surface cold admission never builds an unselected surface or publishes a subset", "[navigation][cook][selection]") {
        BakeHarness harness;
        harness.fixture.surfaces.push_back(Id<SurfaceId>(2));
        const auto request = Selected(harness, {Id<SurfaceId>(1)});
        REQUIRE(request.HasValue());
        const auto result = Terminal(*harness.service, harness.operations, request.Value());
        REQUIRE(result.state == OperationState::Failed);
        REQUIRE(result.error.has_value());
        CHECK(result.error->code.Value() == NavigationErrors::BakeSelectionIncomplete.code.Value());
        CHECK(harness.builder->builds.load() == 0);
        CHECK_FALSE(harness.service->Published());
        CHECK_FALSE(std::filesystem::exists(harness.config.targetRoot / "current.json"));
    }

    TEST_CASE("Selected-surface rebuild preserves complete verified unselected topology and rejects stale output",
              "[navigation][cook][selection]") {
        BakeHarness harness;
        harness.fixture.surfaces.push_back(Id<SurfaceId>(2));
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto original = harness.service->Published();
        REQUIRE(original);
        REQUIRE(original->tiles.tiles.size() == 8);
        harness.fixture.ExcludeBorder();
        const auto selected = Selected(harness, {Id<SurfaceId>(1)});
        REQUIRE(selected.HasValue());
        REQUIRE(Terminal(*harness.service, harness.operations, selected.Value()).state == OperationState::Succeeded);
        const auto updated = harness.service->Published();
        REQUIRE(updated);
        REQUIRE(updated->tiles.tiles.size() == 8);
        CHECK(updated->rebuiltTiles == 2);
        CHECK(updated->reusedTiles == 6);
        for (std::size_t index = 4; index < 8; ++index)
            CHECK(updated->tiles.tiles[index] == original->tiles.tiles[index]);
        const auto builds = harness.builder->builds.load();
        const auto previous = Assets::ResolveCurrentCookGeneration(Horo::NativeDurableFileSystem{}, harness.config.targetRoot);
        REQUIRE(previous.HasValue());
        harness.fixture.ExcludeBorder();
        harness.fixture.modifiers.front().surface = Id<SurfaceId>(2);
        const auto stale = Selected(harness, {Id<SurfaceId>(1)});
        REQUIRE(stale.HasValue());
        const auto rejected = Terminal(*harness.service, harness.operations, stale.Value());
        REQUIRE(rejected.state == OperationState::Failed);
        REQUIRE(rejected.error.has_value());
        CHECK(rejected.error->code.Value() == NavigationErrors::BakeSelectionIncomplete.code.Value());
        CHECK(harness.builder->builds.load() == builds);
        CHECK(harness.service->Published() == updated);
        const auto retained = Assets::ResolveCurrentCookGeneration(Horo::NativeDurableFileSystem{}, harness.config.targetRoot);
        REQUIRE(retained.HasValue());
        CHECK(retained.Value().manifestDigest == previous.Value().manifestDigest);
    }

    TEST_CASE("Canonical selected-surface order joins one native operation", "[navigation][cook][selection]") {
        BakeHarness harness;
        harness.fixture.surfaces.push_back(Id<SurfaceId>(2));
        harness.builder->pause.store(true);
        const auto first = Selected(harness, {Id<SurfaceId>(2), Id<SurfaceId>(1)});
        REQUIRE(first.HasValue());
        const auto joined = Selected(harness, {Id<SurfaceId>(1), Id<SurfaceId>(2)});
        REQUIRE(joined.HasValue());
        CHECK(joined.Value() == first.Value());
        harness.builder->pause.store(false);
        REQUIRE(Terminal(*harness.service, harness.operations, first.Value()).state == OperationState::Succeeded);
        REQUIRE(harness.service->Published());
        CHECK(harness.service->Published()->tiles.tiles.size() == 8);
    }

    TEST_CASE("Selected validation preserves injected selector failure even when the ambient selector is absent",
              "[navigation][cook][selection][authority]") {
        auto files = std::make_shared<SelectorObservationFiles>(true);
        BakeHarness harness{files};
        REQUIRE_FALSE(std::filesystem::exists(harness.config.targetRoot / "current.json"));
        const auto submitted =
            harness.service->Submit(Request(harness, NavigationBakeExecutionMode::ValidateOnly, std::vector{Id<SurfaceId>(1)}));
        REQUIRE(submitted.HasValue());
        const auto result = Terminal(*harness.service, harness.operations, submitted.Value());
        REQUIRE(result.state == OperationState::Failed);
        REQUIRE(result.error);
        CHECK(result.error->code.Value() == files->failure.code.Value());
        REQUIRE(result.error->cause.Get());
        CHECK(result.error->cause.Get()->code.Value() == FileStorageErrors::UnsafeObject.code.Value());
        CHECK(files->observations.load() > 0);
        CHECK(harness.builder->builds.load() == 0);
        CHECK_FALSE(std::filesystem::exists(harness.config.targetRoot));
    }

    TEST_CASE("Selected validation uses injected selector absence even when an ambient selector exists",
              "[navigation][cook][selection][authority]") {
        auto files = std::make_shared<SelectorObservationFiles>(false);
        BakeHarness harness{files};
        std::filesystem::create_directories(harness.config.targetRoot);
        const auto selector = harness.config.targetRoot / "current.json";
        std::ofstream(selector) << "ambient bytes outside the injected selector view";
        const auto submitted =
            harness.service->Submit(Request(harness, NavigationBakeExecutionMode::ValidateOnly, std::vector{Id<SurfaceId>(1)}));
        REQUIRE(submitted.HasValue());
        CHECK(Terminal(*harness.service, harness.operations, submitted.Value()).state == OperationState::Succeeded);
        CHECK(files->observations.load() > 0);
        CHECK(harness.builder->builds.load() == 0);
        CHECK_FALSE(harness.service->Published());
        CHECK(std::filesystem::file_size(selector) == std::string_view{"ambient bytes outside the injected selector view"}.size());
    }
}  // namespace Horo::Application
