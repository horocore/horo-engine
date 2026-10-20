#include "Horo/Editor/PCGPreviewSession.h"
#include "PCGDocumentTestSupport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Horo::Editor {
    namespace {
        using namespace PCGDocumentTestSupport;
        using namespace PCG;

        struct Fixture final {
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 8}};
            PCGNodeCatalogSnapshot catalog;
            std::optional<PCGGraphDocument> document;
            PCGSpatialSnapshot spatial{Spatial()};
            PCGPreviewSession preview{jobs, Id<PCGWorldId>(90), 4};

            static PCGSpatialSnapshot Spatial(std::uint64_t revision = 1) {
                PCGSpatialSnapshotCandidate value;
                value.snapshot = Id<SpatialSnapshotId>(revision);
                value.provenance = {Id<SpatialProviderId>(1), Id<SpatialSourceId>(2), Id<SpatialRevision>(revision)};
                value.bounds = {{-10, -10, -10}, {10, 10, 10}};
                value.coverage = PCGSpatialCoverage::Complete;
                value.grids = {{Id<SpatialElementId>(8), {}, {1, 1, 1}, {2, 2, 2}}};
                return CapturePCGSpatialSnapshot(std::move(value)).Value();
            }

            Fixture() {
                const std::array grants{PCGCapability::Validation, PCGCapability::OfflineBake, PCGCapability::EditorPreview};
                auto created = PCGNodeCatalog::Create(
                    ProjectPCGCapabilities(PCGHostProfile::Interactive, PCGCapabilitySet::Create(grants).Value()).Value());
                REQUIRE(created.HasValue());
                auto owner = std::move(created).Value();
                REQUIRE(owner.Register(PCGCpuNodeKind::SnapshotGrid).HasValue());
                catalog = owner.Snapshot().Value();
                PCGGraphSourceData graph;
                graph.generation = {Id<GraphId>(77), Id<GraphRevision>(1)};
                graph.nodes = {{Id<NodeId>(10),
                                PCGCpuNodeType(PCGCpuNodeKind::SnapshotGrid).Value(),
                                {1, 0},
                                {{Id<PinId>(101), PCGPinDirection::Output, PCGPinType::PointSet, PCGPinCardinality::Multiple, {}}},
                                {0, 0, 0, 0, 0, 0, 0, 8}}};
                PCGDocumentCatalog support{.revision = 1};
                for (const auto &node : catalog.Nodes())
                    support.types.push_back({node.type, node.version, node.version});
                auto source = PCGGraphAsset::Create(graph, {.supportedNodeTypes = support.types});
                REQUIRE(source.HasValue());
                auto bytes = SerializePCGGraphAsset(source.Value()).Value();
                const auto raw = std::as_bytes(std::span(bytes));
                Assets::AssetSourceCapture capture{{true, raw.size(), ComputeSha256(raw)}, {raw.begin(), raw.end()}};
                auto opened = PCGGraphDocument::Open(Asset(), Identity(), {Asset(9), 1}, capture, support);
                REQUIRE(opened.HasValue());
                document.emplace(std::move(opened).Value());
            }

            PCGDocumentSnapshot Snapshot() {
                return document->Snapshot().Value();
            }

            PCGPreviewRequest Request() {
                const std::array attributes{
                    PCGAttributeDescriptor{PCGAttributeKey::Create("pcg.weight").Value(), PCGAttributeType::Scalar}};
                auto schema = PCGPointSchema::Capture({PCGOperationalTier::Baseline, attributes});
                REQUIRE(schema.HasValue());
                const auto tier = LimitsForTier(PCGOperationalTier::Baseline).Value();
                PCGCpuEvaluationLimits limits{tier.maximumScratchBytes, tier.maximumCandidateBytes};
                limits.numericProfile.bytes[0] = 1;
                limits.providerContent.bytes[0] = 1;
                return {Snapshot(), catalog, spatial, std::make_shared<const PCGPointSchema>(std::move(schema).Value()), {}, limits, 8};
            }

            PCGPreviewSnapshot Advance() {
                const auto source = Snapshot();
                return preview.Advance(&source, &spatial).Value();
            }

            PCGPreviewSnapshot Complete() {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                auto view = Advance();
                while (view.phase == PCGPreviewPhase::Evaluating && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::yield();
                    view = Advance();
                }
                REQUIRE(view.phase != PCGPreviewPhase::Evaluating);
                return view;
            }
        };

        /** @brief Holds the only CPU lane to exercise queued cancellation/replacement without timing races. */
        struct Blocker final {
            std::mutex mutex;
            std::condition_variable changed;
            bool release{};
            JobHandle job;

            explicit Blocker(JobSystem &jobs) {
                auto submitted = jobs.SubmitResult({}, [this](const CancellationToken &) {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [this] {
                        return release;
                    });
                    return Result<void>::Success();
                });
                REQUIRE(submitted.HasValue());
                job = std::move(submitted).Value();
            }

            void Release() {
                {
                    std::lock_guard lock(mutex);
                    release = true;
                }
                changed.notify_all();
            }

            ~Blocker() {
                Release();
                static_cast<void>(job.Wait());
            }
        };
    }  // namespace

    TEST_CASE("PCG preview runs canonical cook and CPU evaluation without changing document authority", "[unit][editor][pcg][preview]") {
        Fixture fixture;
        const auto before = fixture.Snapshot();
        REQUIRE(fixture.preview.Request(fixture.Request()).HasValue());
        const auto view = fixture.Complete();
        REQUIRE(view.phase == PCGPreviewPhase::Ready);
        REQUIRE(view.overlay);
        REQUIRE(view.overlay->candidate.Outputs().size() == 1);
        CHECK(view.overlay->candidate.Outputs()[0].points->PointCount() == 8);
        CHECK(view.overlay->candidate.Outputs()[0].points->Transforms()[1].translation != Math::Vec3{});
        CHECK(view.overlay->fence.documentRevision == before.revision);
        CHECK(fixture.Snapshot().source == before.source);
        CHECK(fixture.Snapshot().state == before.state);
        CHECK_FALSE(fixture.Snapshot().dirty);
        CHECK(fixture.document->UndoCount() == 0);
        CHECK(fixture.preview.MaximumDrawPoints() == 4);
    }

    TEST_CASE("PCG preview coalesces requests and cancelled completed work cannot replace newest output", "[unit][editor][pcg][preview]") {
        Fixture fixture;
        Blocker blocker{fixture.jobs};
        REQUIRE(fixture.preview.Request(fixture.Request()).Value() == 1);
        REQUIRE(fixture.Advance().phase == PCGPreviewPhase::Evaluating);
        REQUIRE(fixture.preview.Request(fixture.Request()).Value() == 2);
        REQUIRE(fixture.preview.Request(fixture.Request()).Value() == 3);
        blocker.Release();
        const auto view = fixture.Complete();
        REQUIRE(view.phase == PCGPreviewPhase::Ready);
        REQUIRE(view.overlay);
        CHECK(view.overlay->request == 3);
        CHECK(view.request == 3);
    }

    TEST_CASE("PCG preview invalidates both running and ready results on document/provider/close changes", "[unit][editor][pcg][preview]") {
        Fixture fixture;
        REQUIRE(fixture.preview.Request(fixture.Request()).HasValue());
        REQUIRE(fixture.Complete().overlay);
        SECTION("document edit") {
            REQUIRE(fixture.document->Execute(Edit(fixture.Snapshot(), 19)).HasValue());
            CHECK(fixture.Advance().phase == PCGPreviewPhase::Cancelled);
        }
        SECTION("catalog replacement") {
            auto snapshot = fixture.Snapshot();
            ++snapshot.catalogRevision;
            CHECK(fixture.preview.Advance(&snapshot, &fixture.spatial).Value().phase == PCGPreviewPhase::Cancelled);
        }
        SECTION("provider replacement") {
            fixture.spatial = Fixture::Spatial(2);
            CHECK(fixture.Advance().phase == PCGPreviewPhase::Cancelled);
        }
        SECTION("close") {
            REQUIRE(fixture.document->Close().HasValue());
            CHECK(fixture.preview.Advance(nullptr, &fixture.spatial).Value().phase == PCGPreviewPhase::Cancelled);
        }
        SECTION("reopen same source in new session") {
            auto snapshot = fixture.Snapshot();
            ++snapshot.session.generation;
            CHECK(fixture.preview.Advance(&snapshot, &fixture.spatial).Value().phase == PCGPreviewPhase::Cancelled);
        }
        CHECK_FALSE(fixture.preview.Advance(nullptr, nullptr).Value().overlay);
    }

    TEST_CASE("PCG preview cancellation fences already completed results and pending parents", "[unit][editor][pcg][preview]") {
        Fixture fixture;
        CancellationSource parent;
        auto request = fixture.Request();
        request.cancellation = parent.Token();
        REQUIRE(fixture.preview.Request(std::move(request)).HasValue());
        REQUIRE(fixture.Complete().overlay);
        parent.RequestCancellation();
        CHECK_FALSE(fixture.Advance().overlay);
        REQUIRE(fixture.preview.Request(fixture.Request()).HasValue());
        REQUIRE(fixture.preview.Cancel().HasValue());
        CHECK_FALSE(fixture.Advance().overlay);
        CHECK(fixture.Advance().phase == PCGPreviewPhase::Cancelled);
    }

    TEST_CASE("PCG preview rejects invalid inputs and exposes cooker/evaluator/queue failures", "[unit][editor][pcg][preview]") {
        Fixture fixture;
        auto request = fixture.Request();
        SECTION("zero points") {
            request.maximumPointsPerOutput = 0;
        }
        SECTION("unbounded points") {
            request.maximumPointsPerOutput = 65'537;
        }
        SECTION("missing source") {
            request.document.source.reset();
        }
        SECTION("missing schema") {
            request.schema.reset();
        }
        SECTION("missing catalog") {
            request.catalog = {};
        }
        SECTION("cancelled admission") {
            CancellationSource cancelled;
            cancelled.RequestCancellation();
            request.cancellation = cancelled.Token();
        }
        SECTION("evaluator budget failure") {
            request.evaluation.maximumCandidateBytes = 1;
            REQUIRE(fixture.preview.Request(std::move(request)).HasValue());
            const auto view = fixture.Complete();
            CHECK(view.phase == PCGPreviewPhase::Failed);
            CHECK(view.error.has_value());
            CHECK_FALSE(view.overlay);
            return;
        }
        SECTION("closed scheduler") {
            fixture.jobs.StopAccepting();
            REQUIRE(fixture.preview.Request(std::move(request)).HasValue());
            const auto view = fixture.Complete();
            CHECK(view.phase == PCGPreviewPhase::Failed);
            CHECK(view.error.has_value());
            return;
        }
        CHECK(fixture.preview.Request(std::move(request)).HasError());
        CHECK_FALSE(fixture.Advance().overlay);
    }

    TEST_CASE("PCG preview shutdown cancels queued capture and closes admission without scene work", "[unit][editor][pcg][preview]") {
        Fixture fixture;
        Blocker blocker{fixture.jobs};
        REQUIRE(fixture.preview.Request(fixture.Request()).HasValue());
        REQUIRE(fixture.Advance().phase == PCGPreviewPhase::Evaluating);
        REQUIRE(fixture.preview.Shutdown(Duration::FromMilliseconds(100)).HasValue());
        CHECK(fixture.preview.Request(fixture.Request()).HasError());
        CHECK(fixture.Advance().phase == PCGPreviewPhase::Closed);
        CHECK_FALSE(fixture.Advance().overlay);
        REQUIRE(fixture.preview.Shutdown(Duration::FromMilliseconds(100)).HasValue());
    }

    TEST_CASE("PCG preview rejects mutation from another owner thread", "[unit][editor][pcg][preview]") {
        Fixture fixture;
        auto request = fixture.Request();
        std::atomic<bool> rejected{};
        std::thread other([&] {
            rejected = fixture.preview.Request(std::move(request)).HasError();
        });
        other.join();
        CHECK(rejected.load());
    }

    TEST_CASE("PCG preview fences queued work after an edit and bounds retained overlay leases", "[unit][editor][pcg][preview]") {
        Fixture fixture;
        SECTION("edit before worker completion") {
            Blocker blocker{fixture.jobs};
            REQUIRE(fixture.preview.Request(fixture.Request()).HasValue());
            REQUIRE(fixture.Advance().phase == PCGPreviewPhase::Evaluating);
            REQUIRE(fixture.document->Execute(Edit(fixture.Snapshot(), 98)).HasValue());
            REQUIRE(fixture.Advance().phase == PCGPreviewPhase::Cancelled);
            blocker.Release();
            CHECK_FALSE(fixture.Advance().overlay);
        }
        SECTION("retained readers have finite admission") {
            std::vector<std::shared_ptr<const PCGPreviewOverlay>> leases;
            for (std::size_t index = 0; index < 16; ++index) {
                REQUIRE(fixture.preview.Request(fixture.Request()).HasValue());
                auto view = fixture.Complete();
                REQUIRE(view.overlay);
                leases.push_back(view.overlay);
            }
            CHECK(fixture.preview.Request(fixture.Request()).HasError());
            leases.clear();
            REQUIRE(fixture.preview.Request(fixture.Request()).HasValue());
            CHECK(fixture.Complete().phase == PCGPreviewPhase::Ready);
        }
        SECTION("explicit shutdown validates deadline before closing admission") {
            CHECK(fixture.preview.Shutdown(Duration{}).HasError());
            REQUIRE(fixture.preview.Request(fixture.Request()).HasValue());
            CHECK(fixture.Complete().phase == PCGPreviewPhase::Ready);
        }
    }
}  // namespace Horo::Editor
