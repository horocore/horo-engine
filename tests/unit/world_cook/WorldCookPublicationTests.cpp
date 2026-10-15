#include "../runtime/world_streaming/WorldArtifactCodecTestSupport.h"
#include "AllocationProbe.h"
#include "Horo/WorldCook/WorldCookPublication.h"
#include "assets/AssetCookOutputFixture.h"

#include <fstream>

namespace Horo::WorldCook {
    namespace {
        namespace W = WorldStreaming;
        namespace S = Runtime;
        namespace A = Assets;
        namespace T = A::OutputTestSupport;
        using W::TestSupport::Asset;
        using W::TestSupport::IdentityFrom;

        /** @brief Independent finite host ceilings for the complete actual two-cell publication. */
        PublicationLimits Limits() {
            return {{65536, 65536, {8, 8, 65536}},
                    {1, 65536, 65536},
                    {65536, 128, 4, {2, 8, 32}, {8, 8, 32, 65536, 65536}},
                    {65536, 65536, 8, 8, 16, 16, 16, 16, 32, 512},
                    {8, 8, 32, 64},
                    {8, 8, 16, 16, 4096, 65536, 131072},
                    {65536, 131072, 32, 1},
                    1048576,
                    16};
        }

        /** @brief Exact page/object publication independent from cell-local Scene identity. */
        W::WorldDependencyEndpoint Endpoint(const std::uint64_t object) {
            return {{Asset(1), object}, IdentityFrom<W::WorldAuthoringRevision>(1)};
        }

        /** @brief Actual typed source, owning release, native writer lease and observable durable files. */
        struct Fixture final {
            W::ArtifactCodecTestSupport::Fixture world;
            W::WorldSpatialAssignment assignments{Assignments(world.manifest.Descriptor())};
            W::WorldSpanningObjectPlan placements{Placements(assignments)};
            W::WorldDependencyPlan dependencies{Dependencies(assignments)};
            std::array<S::RuntimeEntityDefinition, 2> entities{{{.object = {11}}, {.object = {22}}}};
            std::array<S::SceneCellCookInput, 2> inputs{Inputs(entities)};
            std::vector<ObjectBinding> objects{SpatialObjectBinding{Endpoint(1), W::CandidateTestSupport::Cell(-1), {11}},
                                               SpatialObjectBinding{Endpoint(2), W::CandidateTestSupport::Cell(), {22}}};
            A::AssetChunkPlan packages{Packages()};
            S::IncrementalSceneCellCook cache{{8, 8, 16, 8, 65536, 131072}};
            T::TempDir directory;
            T::PublicationFiles files;
            std::vector<std::vector<std::uint8_t>> baseArtifacts;
            std::uint64_t attempt{};

            explicit Fixture(const bool nonspatial = false)
                : assignments(Assignments(world.manifest.Descriptor(), nonspatial)), placements(Placements(assignments)),
                  dependencies(Dependencies(assignments)), packages(Packages(nonspatial)) {
                files.root = directory.path;
                if (nonspatial) {
                    baseArtifacts.push_back(T::MakePayload(Asset(8), 16));
                    objects.push_back(BaseObjectBinding{Endpoint(3), Asset(8), T::DigestOf(baseArtifacts.front())});
                }
            }

            static W::WorldSpatialAssignment Assignments(const W::WorldPartitionDescriptor &partition, const bool nonspatial = false) {
                const auto bounds = [](const std::int64_t x) {
                    const auto point = Math::WorldCoordinate64::FromMillimeters(x, 0, 0);
                    return W::WorldPartitionBounds{point, point};
                };
                std::vector candidates{W::WorldSpatialAssignmentCandidate{Endpoint(2).address, Endpoint(2).revision, bounds(0),
                                                                          W::TestSupport::Layer(), 0},
                                       W::WorldSpatialAssignmentCandidate{Endpoint(1).address, Endpoint(1).revision, bounds(-1000),
                                                                          W::TestSupport::Layer(), 0}};
                if (nonspatial)
                    candidates.push_back({Endpoint(3).address, Endpoint(3).revision, partition.Bounds(), W::TestSupport::Layer(), 0});
                auto result = W::WorldSpatialAssignment::Create(partition, candidates, {8, 8, 16});
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            static W::WorldSpanningObjectPlan Placements(const W::WorldSpatialAssignment &assignments) {
                std::vector<W::WorldSpanningObjectDirective> directives;
                if (assignments.Objects().size() == 3)
                    directives.push_back({Endpoint(3).address, Endpoint(3).revision, W::WorldSpanningObjectPolicy::NonSpatial, {}});
                auto result = W::WorldSpanningObjectPlan::Create(assignments, directives, {8, 1, 16});
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            static W::WorldDependencyPlan Dependencies(const W::WorldSpatialAssignment &assignments) {
                const std::array edges{W::WorldDependencyCandidate{Endpoint(1), Endpoint(2), W::WorldDependencyKind::Hard},
                                       W::WorldDependencyCandidate{Endpoint(2), Endpoint(9), W::WorldDependencyKind::Soft}};
                auto result = W::WorldDependencyPlan::Create(assignments, edges, {8, 8, 8, 8});
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            static std::array<S::SceneCellCookInput, 2> Inputs(const std::array<S::RuntimeEntityDefinition, 2> &entities) {
                const S::SceneCellPayloadIdentity left{W::TestSupport::World(), W::CandidateTestSupport::Cell(-1), {71}, {1}};
                const S::SceneCellPayloadIdentity right{W::TestSupport::World(), W::CandidateTestSupport::Cell(), {72}, {1}};
                return {S::SceneCellCookInput{{left, std::span{entities}.first(1)}, left},
                        S::SceneCellCookInput{{right, std::span{entities}.last(1)}, right}};
            }

            static A::AssetChunkPlan Packages(const bool nonspatial = false) {
                auto id = A::AssetChunkId::Parse("base");
                REQUIRE(id.HasValue());
                std::array definitions{
                    A::AssetChunkDefinition{std::move(id).Value(), A::AssetChunkKind::Base, {Asset(4), Asset(5), Asset(6), Asset(7)}}};
                if (nonspatial)
                    definitions.front().assets.push_back(Asset(8));
                auto result = A::AssetChunkPlan::Create(definitions);
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            SourceCapture Source() const {
                return {world.manifest.Descriptor(),     assignments, placements,       dependencies,      inputs, {}, objects, {},
                        W::CandidateTestSupport::Hash(), 1,           cache.Revision(), "Canonical world", {},     {}};
            }

            ReleaseCapture Release() const {
                return {packages,
                        {IdentityFrom<W::WorldPackageAssignmentId>(1), IdentityFrom<W::WorldPackageAssignmentRevision>(1),
                         IdentityFrom<W::PartitionEpoch>(1)},
                        W::CandidateTestSupport::Hash(),
                        baseArtifacts,
                        Asset(6),
                        Asset(7),
                        {T::Type("world.index"), T::Type("world.dependencies"), T::Type("world.cell")},
                        directory.path,
                        T::Target("headless-null")};
            }

            Result<PublicationReport> Publish(const PublicationLimits &limits = Limits(),
                                              const std::function<Result<void>()> &freshness = [] {
                                                  return Result<void>::Success();
                                              }, const CancellationToken &cancellation = {}) {
                auto lease = files.native.TryAcquireExclusive(directory.path / ".cook-writer.lock", "world publication fixture");
                REQUIRE(lease.HasValue());
                auto policy = T::Policy(files, "10000000-0000-0000-0000-000000000001");
                policy.newOperationId = [&] {
                    return Result<A::AssetId>::Success(Asset(++attempt + 100));
                };
                policy.operationId.clear();
                policy.writerLease = &lease.Value();
                policy.beforeCommit = freshness;
                return PublishWorldCook(cache, Source(), Release(), limits, policy, cancellation);
            }
        };

        /** @brief Reads exact committed owner artifacts rather than reconstructing expected bytes in the assertion. */
        A::AssetCookGenerationContents Contents(const PublicationReport &report) {
            REQUIRE(report.publication);
            auto contents = A::ReadCookGenerationContents(*report.publication, Limits().maximumTotalArtifactBytes);
            REQUIRE(contents.HasValue());
            return std::move(contents).Value();
        }

        /** @brief Detaches actual published provider bytes for independent malformed/missing-artifact admission. */
        struct Detached final {
            std::vector<std::byte> index;
            std::vector<std::byte> dependencies;
            std::vector<std::pair<A::AssetId, std::vector<std::byte>>> cells;

            explicit Detached(const A::AssetCookGenerationContents &contents) {
                for (const auto &bytes : contents.artifacts) {
                    auto decoded = A::DecodeCookedArtifact(bytes);
                    REQUIRE(decoded.HasValue());
                    const auto raw = std::as_bytes(std::span{decoded.Value().payload});
                    std::vector<std::byte> copied(raw.begin(), raw.end());
                    if (decoded.Value().id == Asset(6))
                        index = std::move(copied);
                    else if (decoded.Value().id == Asset(7))
                        dependencies = std::move(copied);
                    else
                        cells.emplace_back(decoded.Value().id, std::move(copied));
                }
            }

            Result<ValidationReport> Validate(const Fixture &fixture) const {
                std::vector<CellArtifactView> views;
                for (const auto &[id, bytes] : cells)
                    views.push_back({id, bytes});
                return ValidateWorldCookArtifacts(fixture.Source(), fixture.Release(), {index, dependencies, views}, Limits());
            }
        };
    }  // namespace

    TEST_CASE("Cook test directory collisions preserve existing roots and bound exclusive admission", "[world_cook][native]") {
        T::TempDir parent;
        const auto existing = parent.path / "existing";
        REQUIRE(std::filesystem::create_directory(existing));
        const auto sentinel = existing / "sentinel.txt";
        {
            std::ofstream stream(sentinel);
            stream << "existing owner";
            REQUIRE(stream.good());
        }
        std::size_t attempts{};
        SECTION("Collision retries into a distinct exclusively created child") {
            const auto fresh = parent.path / "fresh";
            const auto created = A::CookTestValues::CreateCookTestDirectoryFromCandidates([&](const std::size_t attempt) {
                ++attempts;
                return attempt == 0 ? existing : fresh;
            }, true);
            CHECK(attempts == 2);
            CHECK(created == std::filesystem::canonical(fresh));
            CHECK(T::ReadText(sentinel) == "existing owner");
            // This enclosing exclusively owned parent also owns the newly admitted child.
            CHECK(std::filesystem::remove(created));
            CHECK(std::filesystem::is_directory(existing));
            CHECK(T::ReadText(sentinel) == "existing owner");
        }
        SECTION("Repeated collisions exhaust the finite bound without adopting an existing root") {
            REQUIRE_THROWS_AS(A::CookTestValues::CreateCookTestDirectoryFromCandidates(
                                  [&](const std::size_t) {
                ++attempts;
                return existing;
            }, false),
                              std::filesystem::filesystem_error);
            CHECK(attempts == A::CookTestValues::MaximumCookTestDirectoryAttempts);
            CHECK(T::ReadText(sentinel) == "existing owner");
        }
        SECTION("Non-collision filesystem errors surface without retry") {
            REQUIRE_THROWS_AS(A::CookTestValues::CreateCookTestDirectoryFromCandidates(
                                  [&](const std::size_t) {
                ++attempts;
                return parent.path / "missing" / "child";
            }, false),
                              std::filesystem::filesystem_error);
            CHECK(attempts == 1);
            CHECK(T::ReadText(sentinel) == "existing owner");
        }
    }

    TEST_CASE("World committed Scene adoption and report lease transport allocate no new storage", "[world_cook][native]") {
        Fixture fixture;
        const auto source = fixture.Source();
        auto prepared = fixture.cache.Prepare(source.partition, source.cells, {}, source.settings, 0, Limits().scene.semantic);
        REQUIRE(prepared.HasValue());
        auto receipt = std::move(prepared).Value();
        const auto reportLease = receipt.ReportLease();
        const auto validation = std::make_shared<const ValidationReport>();
        const std::vector<std::vector<std::uint8_t>> payloads{T::MakePayload(Asset(8), 16)};
        const std::array entries{T::Entry(Asset(8), payloads.front())};
        auto writer = fixture.files.native.TryAcquireExclusive(fixture.directory.path / ".cook-writer.lock", "adoption fixture");
        REQUIRE(writer.HasValue());
        auto policy = T::Policy(fixture.files, "10000000-0000-0000-0000-000000000002");
        policy.writerLease = &writer.Value();
        policy.beforeCommit = [&] {
            return receipt.Reserve();
        };
        Tests::AllocationProbe::Measurement measured{};
        bool committed{};
        policy.afterCommit = [&](const A::AssetCookGeneration &) {
            Tests::AllocationProbe::ScopedMeasurement scope;
            receipt.Adopt();
            const auto copiedLease = receipt.ReportLease();
            auto result = Result<PublicationReport>::Success({validation, copiedLease, {}});
            auto copied = result;
            auto moved = std::move(copied);
            committed = moved.Value().scene == reportLease;
            measured = scope.Snapshot();
        };
        auto published =
            A::PublishCookGeneration(fixture.directory.path, T::Target("headless-null"), entries, payloads, Limits().assets, policy);
        REQUIRE(published.HasValue());
        CHECK(committed);
        CHECK(measured.requests == 0);
        CHECK(fixture.cache.Revision() == 1);
        CHECK(reportLease->cooked == 2);
        CHECK(receipt.Report().cells.size() == 2);
    }

    TEST_CASE("World cook publishes actual Scene cells hard dependencies and complete release bytes", "[world_cook][native]") {
        Fixture fixture;
        auto published = fixture.Publish();
        if (published.HasError())
            INFO(published.ErrorValue().message);
        REQUIRE(published.HasValue());
        REQUIRE(published.Value().validation->CanPublish());
        REQUIRE(published.Value().publication);
        CHECK(published.Value().scene->cooked == 2);
        CHECK(fixture.cache.Revision() == 1);
        const auto contents = Contents(published.Value());
        REQUIRE(contents.entries.size() == 4);
        const auto indexEntry = std::ranges::find(contents.entries, Asset(6), &A::AssetCookManifestEntry::assetId);
        REQUIRE(indexEntry != contents.entries.end());
        const auto position = static_cast<std::size_t>(indexEntry - contents.entries.begin());
        auto envelope = A::DecodeCookedArtifact(contents.artifacts[position]);
        REQUIRE(envelope.HasValue());
        auto decoded = W::DecodeWorldIndexArtifact(std::as_bytes(std::span{envelope.Value().payload}), Limits().index);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().descriptor.Cells()[0].id == W::CandidateTestSupport::Cell(-1));
        CHECK(decoded.Value().cells.size() == 2);
    }

    TEST_CASE("World clean and incremental complete inventories are byte stable under input membership order", "[world_cook][native]") {
        Fixture fixture;
        auto first = fixture.Publish();
        REQUIRE(first.HasValue());
        const auto clean = Contents(first.Value());
        std::ranges::reverse(fixture.inputs);
        std::ranges::reverse(fixture.objects);
        auto second = fixture.Publish();
        REQUIRE(second.HasValue());
        CHECK(second.Value().scene->cooked == 0);
        CHECK(second.Value().scene->reused == 2);
        const auto incremental = Contents(second.Value());
        CHECK(clean.artifacts == incremental.artifacts);
        CHECK(first.Value().publication->manifestDigest == second.Value().publication->manifestDigest);
        S::IncrementalSceneCellCook fresh{{8, 8, 16, 8, 65536, 131072}};
        // A separate owner cooks the same reordered capture without any cache history.
        const auto source = fixture.Source();
        auto cold = fresh.Cook(source.partition, source.cells, {}, source.settings, 0, Limits().scene.semantic);
        REQUIRE(cold.HasValue());
        CHECK(cold.Value().cells[0].key == second.Value().scene->cells[0].key);
    }

    TEST_CASE("World precommit encoding freshness and filesystem failures preserve exact previous cache and pointer",
              "[world_cook][native]") {
        Fixture fixture;
        auto first = fixture.Publish();
        REQUIRE(first.HasValue());
        const auto pointer = T::ReadText(fixture.directory.path / "current.json");
        const auto oldLease = first.Value().scene->cells[0].payload;
        fixture.entities[0].localTransform.translation.x = 4;
        SECTION("Encoding bound") {
            auto limits = Limits();
            limits.scene.maximumEncodedBytes = 1;
            CHECK(fixture.Publish(limits).HasError());
        }
        SECTION("Source replacement at final freshness") {
            CHECK(fixture
                      .Publish(Limits(), [] {
                return Result<void>::Failure(MakeError(Errors::Stale));
            }).HasError());
        }
        SECTION("Cancellation after staging") {
            CancellationSource cancel;
            CHECK(fixture
                      .Publish(Limits(),
                               [&] {
                cancel.RequestCancellation();
                return Result<void>::Success();
            }, cancel.Token())
                      .HasError());
        }
        SECTION("Shutdown before cache reservation") {
            CHECK(fixture
                      .Publish(Limits(), [&] {
                fixture.cache.Shutdown();
                return Result<void>::Success();
            }).HasError());
        }
        SECTION("Durable generation write") {
            fixture.files.fault = T::PublicationFault::ArtifactWrite;
            CHECK(fixture.Publish().HasError());
        }
        CHECK(fixture.cache.Revision() == 1);
        CHECK(T::ReadText(fixture.directory.path / "current.json") == pointer);
        CHECK(oldLease->Definition().Entities()[0].localTransform.translation.x == 0);
    }

    TEST_CASE("World committed durability failure still adopts the exact prepared Scene state", "[world_cook][native]") {
        Fixture fixture;
        REQUIRE(fixture.Publish().HasValue());
        fixture.entities[0].localTransform.translation.x = 7;
        fixture.files.fault = T::PublicationFault::PointerSync;
        auto result = fixture.Publish();
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().publication);
        CHECK(result.Value().publication->durabilityError.has_value());
        CHECK(fixture.cache.Revision() == 2);
        CHECK(result.Value().scene->cells[0].payload->Definition().Entities()[0].localTransform.translation.x == 7);
        auto current = A::ResolveCurrentCookGeneration(fixture.directory.path);
        REQUIRE(current.HasValue());
        CHECK(current.Value().manifestDigest == result.Value().publication->manifestDigest);
    }

    TEST_CASE("World package omission and object content mismatch publish no partial generation", "[world_cook][native]") {
        Fixture fixture;
        SECTION("Missing actual authored binding") {
            fixture.objects[0] = SpatialObjectBinding{Endpoint(1), W::CandidateTestSupport::Cell(-1), {99}};
        }
        SECTION("Missing release inventory asset") {
            auto id = A::AssetChunkId::Parse("base");
            REQUIRE(id.HasValue());
            const std::array chunks{A::AssetChunkDefinition{std::move(id).Value(),
                                                            A::AssetChunkKind::Base,
                                                            {Asset(4), Asset(5), Asset(6), Asset(7), Asset(99)}}};
            auto plan = A::AssetChunkPlan::Create(chunks);
            REQUIRE(plan.HasValue());
            fixture.packages = std::move(plan).Value();
        }
        auto result = fixture.Publish();
        REQUIRE(result.HasValue());
        CHECK_FALSE(result.Value().validation->CanPublish());
        CHECK_FALSE(result.Value().publication);
        CHECK(fixture.cache.Revision() == 0);
        CHECK_FALSE(std::filesystem::exists(fixture.directory.path / "current.json"));
    }

    TEST_CASE("World artifact admission emits owned BuildOutput causes for actual missing corrupt and version bytes",
              "[world_cook][native]") {
        Fixture fixture;
        auto first = fixture.Publish();
        REQUIRE(first.HasValue());
        Detached outputs{Contents(first.Value())};
        SECTION("Missing cooked cell") {
            outputs.cells.pop_back();
        }
        SECTION("Corrupt actual cell bytes") {
            outputs.cells.front().second.back() ^= std::byte{1};
        }
        SECTION("Unsupported index wire version") {
            outputs.index[8] = std::byte{2};
        }
        SECTION("Corrupt dependency sibling") {
            outputs.dependencies.back() ^= std::byte{1};
        }
        SECTION("Captured authored source changed") {
            fixture.entities.front().localTransform.translation.y = 3;
        }
        auto validation = outputs.Validate(fixture);
        REQUIRE(validation.HasValue());
        REQUIRE_FALSE(validation.Value().CanPublish());
        REQUIRE_FALSE(validation.Value().findings.empty());
        CHECK_FALSE(validation.Value().findings.front().cause.code.Value().empty());
        CHECK(validation.Value().findings.front().diagnostic.result == BuildOutputResult::Failed);
        CHECK(fixture.cache.Revision() == 1);
        CHECK(A::ResolveCurrentCookGeneration(fixture.directory.path).Value().manifestDigest == first.Value().publication->manifestDigest);
    }

    TEST_CASE("World source shutdown after final reservation cannot roll back committed publication", "[world_cook][native]") {
        Fixture fixture;
        auto lease = fixture.files.native.TryAcquireExclusive(fixture.directory.path / ".cook-writer.lock", "adoption fixture");
        REQUIRE(lease.HasValue());
        auto policy = T::Policy(fixture.files, "10000000-0000-0000-0000-000000000001");
        policy.writerLease = &lease.Value();
        policy.beforeCommit = [] {
            return Result<void>::Success();
        };
        bool adopted{};
        policy.afterCommit = [&](const A::AssetCookGeneration &) {
            adopted = true;
            fixture.cache.Shutdown();
        };
        auto result = PublishWorldCook(fixture.cache, fixture.Source(), fixture.Release(), Limits(), policy);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().publication);
        CHECK(adopted);
        CHECK(fixture.cache.Revision() == 1);
        CHECK(result.Value().scene->cells.size() == 2);
        const auto source = fixture.Source();
        CHECK(fixture.cache.Cook(source.partition, source.cells, {}, source.settings, 1, Limits().scene.semantic).HasError());
    }

    TEST_CASE("World nonspatial placement requires actual verified provider content in the complete Base inventory",
              "[world_cook][native]") {
        Fixture fixture{true};
        SECTION("Actual provider bytes retained") {
            auto result = fixture.Publish();
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().validation->CanPublish());
            const auto contents = Contents(result.Value());
            REQUIRE(contents.entries.size() == 5);
            const auto base = std::ranges::find(contents.entries, Asset(8), &A::AssetCookManifestEntry::assetId);
            REQUIRE(base != contents.entries.end());
            CHECK(contents.artifacts[static_cast<std::size_t>(base - contents.entries.begin())] == fixture.baseArtifacts.front());
        }
        SECTION("Missing base bytes cannot become metadata-only success") {
            fixture.baseArtifacts.clear();
            auto result = fixture.Publish();
            REQUIRE(result.HasValue());
            CHECK_FALSE(result.Value().validation->CanPublish());
            CHECK_FALSE(result.Value().publication);
            CHECK(fixture.cache.Revision() == 0);
        }
        SECTION("Replaced provider envelope fails independent captured hash") {
            fixture.baseArtifacts.front() = T::MakePayload(Asset(8), 16, 0x99);
            auto result = fixture.Publish();
            REQUIRE(result.HasValue());
            CHECK_FALSE(result.Value().validation->CanPublish());
            CHECK_FALSE(result.Value().publication);
            CHECK(fixture.cache.Revision() == 0);
        }
    }

}  // namespace Horo::WorldCook
