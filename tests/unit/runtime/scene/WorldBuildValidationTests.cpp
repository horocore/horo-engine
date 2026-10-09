#include "../world_streaming/StreamingCellCandidateTestSupport.h"
#include "Horo/Runtime/Scene/WorldBuildValidation.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Runtime {
    namespace {
        namespace W = WorldStreaming;

        struct Fixture final {
            W::CookedWorldIndexManifest manifest = W::CandidateTestSupport::Manifest();
            std::vector<WorldBuildCell> cells;
            std::vector<std::shared_ptr<const RuntimeSceneCellPayload>> payloads;
            WorldBuildValidationLimits limits{8, 8, 32, 16, 1024, 1024 * 1024, 4 * 1024 * 1024};

            Fixture() {
                for (int x = 0; x < 3; ++x) {
                    const SceneCellPayloadIdentity identity{W::TestSupport::World(),
                                                            W::CandidateTestSupport::Cell(x),
                                                            {static_cast<std::uint64_t>(77 + x)},
                                                            {1}};
                    cells.push_back({identity, DiagnosticSourceLocation{"/world/source.scene", 7, 4}});
                    const std::array entities{RuntimeEntityDefinition{.object = {1}}};
                    auto payload = CookRuntimeSceneCellPayload(manifest.Descriptor(), {identity, entities}, identity, {8, 8, 1024 * 1024});
                    REQUIRE(payload.HasValue());
                    payloads.push_back(std::make_shared<const RuntimeSceneCellPayload>(std::move(payload).Value()));
                }
            }

            Result<WorldBuildValidationReport> Run(std::span<const WorldBuildReference> references = {},
                                                   const CancellationToken &cancellation = {}) {
                return ValidateWorldBuild(manifest.Descriptor(), 9, cells, payloads, references, limits, cancellation);
            }
        };

        bool Has(const WorldBuildValidationReport &report, std::string_view code) {
            return std::ranges::any_of(report.diagnostics, [&](const auto &record) {
                return record.code.Value() == code;
            });
        }
    }  // namespace

    TEST_CASE("World build validates complete payload coverage and exact object references", "[scene][world_build]") {
        Fixture fixture;
        const std::array refs{WorldBuildReference{{fixture.cells[0].expected, {1}}, {fixture.cells[1].expected, {1}}, {}}};
        auto valid = fixture.Run(refs);
        REQUIRE(valid.HasValue());
        CHECK(valid.Value().CanPublish());
        CHECK(valid.Value().revision == 9);
        CHECK(valid.Value().estimatedBytes == 3 * fixture.payloads[0]->RetainedBytes());
        BuildOutputStore store{16};
        fixture.payloads[1].reset();
        auto missing = fixture.Run(refs);
        REQUIRE(missing.HasValue());
        CHECK_FALSE(missing.Value().CanPublish());
        CHECK(Has(missing.Value(), "world.build.missing_payload"));
        CHECK(Has(missing.Value(), "world.build.invalid_reference"));
        for (const auto &record : missing.Value().diagnostics)
            store.Append(record);
        auto output = store.SnapshotIfChanged(0);
        REQUIRE(output.has_value());
        REQUIRE(output->records[0].source.has_value());
        CHECK(output->records[0].source->absolutePath == "/world/source.scene");
        CHECK(output->records[0].source->line == 7);
        CHECK(output->records[0].source->column == 4);
    }

    TEST_CASE("World build diagnoses overlap stale output and exact budget boundaries", "[scene][world_build]") {
        Fixture fixture;
        SECTION("duplicate payload owner") {
            fixture.payloads.push_back(fixture.payloads[0]);
            REQUIRE(Has(fixture.Run().Value(), "world.build.overlap"));
        }
        SECTION("source publication changed") {
            fixture.cells[0].expected.revision.value = 2;
            REQUIRE(Has(fixture.Run().Value(), "world.build.stale_payload"));
        }
        SECTION("missing authored publication") {
            fixture.cells.pop_back();
            REQUIRE(Has(fixture.Run().Value(), "world.build.missing_source"));
        }
        SECTION("estimates admit exact equality") {
            const auto bytes = fixture.payloads[0]->RetainedBytes();
            fixture.limits.maximumCellBytes = bytes;
            fixture.limits.maximumWorldBytes = 3 * bytes;
            REQUIRE(fixture.Run().Value().CanPublish());
            --fixture.limits.maximumCellBytes;
            --fixture.limits.maximumWorldBytes;
            auto result = fixture.Run();
            REQUIRE(result.HasValue());
            CHECK(Has(result.Value(), "world.build.cell_budget"));
            CHECK(Has(result.Value(), "world.build.world_budget"));
        }
        SECTION("stale payload accounting precedes cell and aggregate budget findings") {
            ++fixture.cells[0].expected.revision.value;
            const auto bytes = fixture.payloads[0]->RetainedBytes();
            fixture.limits.maximumCellBytes = bytes - 1;
            fixture.limits.maximumWorldBytes = 3 * bytes - 1;
            auto result = fixture.Run();
            REQUIRE(result.HasValue());
            CHECK(result.Value().estimatedBytes == 3 * bytes);
            REQUIRE(result.Value().diagnostics.size() == 5);
            CHECK(result.Value().diagnostics[0].code.Value() == "world.build.stale_payload");
            CHECK(result.Value().diagnostics[1].code.Value() == "world.build.cell_budget");
            CHECK(result.Value().diagnostics[4].code.Value() == "world.build.world_budget");
            CHECK(fixture.limits.maximumCellBytes == bytes - 1);
            CHECK(fixture.limits.maximumWorldBytes == 3 * bytes - 1);
        }
        SECTION("reference generation is exact") {
            auto target = fixture.cells[1].expected;
            ++target.revision.value;
            const std::array refs{WorldBuildReference{{fixture.cells[0].expected, {1}}, {target, {1}}, {}}};
            REQUIRE(Has(fixture.Run(refs).Value(), "world.build.invalid_reference"));
        }
    }

    TEST_CASE("World build report ordering is independent of captured reference order", "[scene][world_build]") {
        Fixture fixture;
        std::array refs{WorldBuildReference{{fixture.cells[1].expected, {1}},
                                            {fixture.cells[2].expected, {99}},
                                            DiagnosticSourceLocation{"/world/source.scene", 20, 1}},
                        WorldBuildReference{{fixture.cells[0].expected, {1}},
                                            {fixture.cells[1].expected, {99}},
                                            DiagnosticSourceLocation{"/world/source.scene", 10, 1}}};
        auto first = fixture.Run(refs);
        REQUIRE(first.HasValue());
        std::ranges::reverse(refs);
        std::ranges::reverse(fixture.cells);
        std::ranges::reverse(fixture.payloads);
        auto second = fixture.Run(refs);
        REQUIRE(second.HasValue());
        REQUIRE(first.Value().diagnostics.size() == 2);
        REQUIRE(second.Value().diagnostics.size() == 2);
        for (std::size_t i = 0; i < 2; ++i) {
            REQUIRE(first.Value().diagnostics[i].source.has_value());
            REQUIRE(second.Value().diagnostics[i].source.has_value());
            CHECK(first.Value().diagnostics[i].source->line == second.Value().diagnostics[i].source->line);
        }
    }

    TEST_CASE("World build refuses partial diagnostics and malformed or cancelled capture", "[scene][world_build]") {
        Fixture fixture;
        SECTION("diagnostic overflow") {
            fixture.payloads.clear();
            fixture.limits.maximumDiagnostics = 1;
            auto result = fixture.Run();
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == SceneCellPayloadErrors::CapacityExceeded.code.Value());
        }
        SECTION("object traversal ceiling") {
            fixture.limits.maximumObjects = 2;
            REQUIRE(fixture.Run().HasError());
        }
        SECTION("duplicate authored owner") {
            fixture.cells.push_back(fixture.cells[0]);
            REQUIRE(fixture.Run().HasError());
        }
        SECTION("source path storage ceiling") {
            fixture.limits.maximumSourceBytes = 1;
            REQUIRE(fixture.Run().HasError());
        }
        SECTION("source navigation requires an absolute path") {
            fixture.cells[0].source->absolutePath = "relative.scene";
            REQUIRE(fixture.Run().HasError());
        }
        SECTION("cancellation leaves input ownership unchanged") {
            CancellationSource cancellation;
            cancellation.RequestCancellation();
            const auto previous = fixture.payloads[0];
            auto result = fixture.Run({}, cancellation.Token());
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == SceneCellPayloadErrors::Cancelled.code.Value());
            CHECK(fixture.payloads[0] == previous);
        }
    }
}  // namespace Horo::Runtime
