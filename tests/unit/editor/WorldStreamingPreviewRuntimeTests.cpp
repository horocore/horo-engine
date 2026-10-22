#include "../runtime/world_streaming/WorldArtifactCodecTestSupport.h"
#include "Horo/Editor/WorldStreamingPreviewRuntime.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;

        struct MountedIndex {
            WS::ArtifactCodecTestSupport::Fixture artifact;
            WorldStreamingPreviewRuntimePolicy policy = MakeWorldStreamingPreviewRuntimePolicy().Value();
            PartitionedWorldWorkspace workspace{WS::TestSupport::Asset(9)};
            PartitionedWorldSnapshotLease source;
            WorldStreamingPreviewHost bridge;

            MountedIndex() {
                const auto &descriptor = artifact.manifest.Descriptor();
                PartitionedWorldDocument document{
                    .settings = {.partition = descriptor.Partition(), .bounds = descriptor.Bounds(), .grid = descriptor.Grid()}};
                document.layers.assign(descriptor.Layers().begin(), descriptor.Layers().end());
                document.sources.push_back({.id = WS::TestSupport::IdentityFrom<WS::StreamingSourceId>(1),
                                            .revision = WS::TestSupport::IdentityFrom<WS::StreamingSourceRevision>(1),
                                            .shape = WS::StreamingSourceBox{descriptor.Bounds().minimum, descriptor.Bounds().maximum},
                                            .target = WS::StreamingDesiredResidency::Activated,
                                            .priority = WS::StreamingSourcePriority::Create(1).Value()});
                source = workspace.Open(document).Value();
            }

            std::unique_ptr<WorldStreamingPreviewRuntime> Prepare() {
                const auto owner = bridge.IssueMountIdentity(source->document.settings.partition).Value();
                const auto encoded = artifact.Encode();
                auto decoded = WS::DecodeWorldIndexArtifact(encoded.bytes, policy.artifact).Value();
                auto runtime = WorldStreamingPreviewRuntime::Create(owner, source->fence, std::move(decoded.descriptor), policy).Value();
                for (unsigned quantum = 0; quantum < 256; ++quantum) {
                    const auto result = runtime->Service(source);
                    REQUIRE(result.HasValue());
                    if (result.Value())
                        return runtime;
                }
                FAIL("bounded native planner did not publish the two-cell fixture");
                return {};
            }
        };

        TEST_CASE("Actual cooked-index native mount binds a real planner and does not fabricate residency or budget observations",
                  "[unit][editor][streaming_preview][native_mount]") {
            MountedIndex fixture;
            auto runtime = fixture.Prepare();
            REQUIRE(runtime->Diagnostics());
            CHECK(runtime->Diagnostics()->BudgetPolicy() == nullptr);
            CHECK(runtime->Diagnostics()->BudgetSample() == nullptr);
            CHECK(runtime->Diagnostics()->Cells().empty());
            REQUIRE(runtime->Diagnostics()->Sources().size() == 1);
            CHECK(runtime->Diagnostics()->Sources().front().Source().owner.IsValid());
            REQUIRE(fixture.bridge.BindRuntime(runtime->Registry(), runtime->Planner(), *runtime->Diagnostics()).HasValue());
            const auto geometry = fixture.bridge.Refresh({fixture.source, {}, 1});
            REQUIRE(geometry.HasValue());
            REQUIRE(geometry.Value()->boxes.size() == 2);
            CHECK(geometry.Value()->preview->Cells().front().desired->residency == WS::StreamingDesiredResidency::Activated);
            CHECK(geometry.Value()->preview->Cells().front().attempts.empty());
            REQUIRE(fixture.bridge.UnbindRuntime(runtime->Owner()).HasValue());
            REQUIRE(runtime->Shutdown().HasValue());
            CHECK(runtime->Planner().Plan().HasError());
            CHECK(geometry.Value()->preview->Cells().front().desired.has_value());
        }

        TEST_CASE("Native replacement issues a fresh exact mounted epoch and stale unmount cannot revoke it",
                  "[unit][editor][streaming_preview][native_mount]") {
            MountedIndex fixture;
            auto first = fixture.Prepare();
            auto second = fixture.Prepare();
            CHECK(second->Owner().epoch.Value() > first->Owner().epoch.Value());
            REQUIRE(fixture.bridge.BindRuntime(first->Registry(), first->Planner(), *first->Diagnostics()).HasValue());
            CHECK(fixture.bridge.BindRuntime(second->Registry(), second->Planner(), *second->Diagnostics()).HasError());
            REQUIRE(fixture.bridge.UnbindRuntime(first->Owner()).HasValue());
            REQUIRE(fixture.bridge.BindRuntime(second->Registry(), second->Planner(), *second->Diagnostics()).HasValue());
            CHECK(fixture.bridge.UnbindRuntime(first->Owner()).HasError());
            REQUIRE(first->Shutdown().HasValue());
            CHECK(fixture.bridge.Refresh({fixture.source, {}, 1}).HasValue());
            REQUIRE(fixture.bridge.UnbindRuntime(second->Owner()).HasValue());
        }

        TEST_CASE("Native source capacity failure publishes no planner or diagnostic state",
                  "[unit][editor][streaming_preview][native_mount]") {
            MountedIndex fixture;
            auto source = fixture.source->document.sources.front();
            source.id = WS::TestSupport::IdentityFrom<WS::StreamingSourceId>(2);
            const auto added = fixture.workspace.EditSource({fixture.source->fence, source.id, source});
            REQUIRE(added.HasValue());
            fixture.policy.planner.limits.sources = 1;
            fixture.policy.planner.limits.sourceVisitsPerFrame = 1;
            const auto owner = fixture.bridge.IssueMountIdentity(fixture.source->document.settings.partition).Value();
            const auto encoded = fixture.artifact.Encode();
            auto decoded = WS::DecodeWorldIndexArtifact(encoded.bytes, fixture.policy.artifact).Value();
            auto runtime =
                WorldStreamingPreviewRuntime::Create(owner, added.Value()->fence, std::move(decoded.descriptor), fixture.policy).Value();
            const auto rejected = runtime->Service(added.Value());
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == WorldDocumentErrors::Capacity.code.Value());
            CHECK(runtime->Diagnostics() == nullptr);
            CHECK(runtime->Planner().Plan().HasError());
        }

        TEST_CASE("Canonical source withdrawal is reconciled through actual native planner registrations",
                  "[unit][editor][streaming_preview][native_mount]") {
            MountedIndex fixture;
            auto runtime = fixture.Prepare();
            const auto sourceId = fixture.source->document.sources.front().id;
            const auto withdrawn = fixture.workspace.EditSource({fixture.source->fence, sourceId, std::nullopt});
            REQUIRE(withdrawn.HasValue());
            bool published{};
            for (unsigned quantum = 0; quantum < 256 && !published; ++quantum) {
                const auto service = runtime->Service(withdrawn.Value());
                REQUIRE(service.HasValue());
                published = service.Value();
            }
            REQUIRE(published);
            CHECK(runtime->Diagnostics()->Sources().empty());
            CHECK(runtime->Diagnostics()->Cells().empty());
            CHECK(runtime->Service(fixture.source).HasError());
            REQUIRE(runtime->Shutdown().HasValue());
            CHECK(runtime->Service(withdrawn.Value()).HasError());
        }
    }  // namespace
}  // namespace Horo::Editor
