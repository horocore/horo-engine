#include "Horo/Assets/AssetCookService.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Physics/PhysicsCollisionCooker.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsTestUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>

namespace {
    using namespace Horo;
    using namespace Horo::Physics;

    const Assets::AssetId Asset = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000854").Value();
    const Assets::AssetTypeId Type = Assets::AssetTypeId::Parse("core.physics_shape").Value();
    const AssetCookTargetId Target = AssetCookTargetId::Parse("headless-null").Value();
    const PhysicsShapeCookTargetDigest PhysicsTarget{ComputeSha256(std::as_bytes(std::span{"qualified-test-target", 21}))};
    const std::array<std::uint8_t, 1> SourceBytes{1};

    /** @brief Bounded fixture importer; deliberately separates format decoding from shape cooking. */
    class SourceImporter final : public IPhysicsCollisionSourceImporter {
    public:
        explicit SourceImporter(const unsigned kind = 0, const std::uint8_t setting = 0) : kind_(kind), setting_(setting) {}

        mutable std::atomic<unsigned> calls{};

        Assets::CookerCacheIdentity CacheIdentity() const noexcept override {
            const std::array<std::byte, 2> bytes{static_cast<std::byte>(kind_), static_cast<std::byte>(setting_)};
            return {.version = "fixture.1", .settingsDigest = ComputeSha256(bytes), .settingsSchemaVersion = 1};
        }

        Result<PhysicsCollisionSource> Import(const Assets::CookSourceView &source, const CancellationToken &) const override {
            ++calls;
            ThrowConfiguredException();
            if (kind_ == 4)
                return Result<PhysicsCollisionSource>::Failure(MakeError(PhysicsErrors::ShapeCookCancelled));
            if (kind_ == 6) {
                auto error = MakeError(PhysicsErrors::ShapeCookSourceInvalid, "Collision vertex field is invalid");
                error.diagnostics.push_back({DiagnosticCode{error.code.Value()},
                                             DiagnosticSeverity::Error,
                                             "Expected finite vertex coordinates",
                                             {std::string{source.sourceContext}, 7, 3},
                                             "vertices[2]"});
                return Result<PhysicsCollisionSource>::Failure(std::move(error));
            }
            if (source.bytes.empty())
                return Result<PhysicsCollisionSource>::Failure(MakeError(PhysicsErrors::ShapeCookSourceInvalid));
            PhysicsCollisionSource result{.subresource = PhysicsShapeSubresourceId::FromValue(9)};
            if (kind_ == 1) {
                result.geometry = PhysicsCollisionMeshSource{
                    .vertices = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}},
                    .triangles = {{{0, 2, 1}, PhysicsShapeSubresourceId::FromValue(21), PhysicsMaterialSlotId::FromValue(1)}},
                    .materialSlots = {PhysicsMaterialSlotId::FromValue(1)},
                };
            } else if (kind_ == 2) {
                result.geometry = PhysicsCollisionHeightFieldSource{
                    .width = 2,
                    .height = 2,
                    .spacingX = 1,
                    .spacingZ = 1,
                    .samples = {0, 0, 0, 0},
                    .cellHoles = {0},
                    .cellMaterials = {PhysicsMaterialSlotId::FromValue(1)},
                    .materialSlots = {PhysicsMaterialSlotId::FromValue(1)},
                };
            } else {
                result.geometry = PhysicsCollisionConvexSource{.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
                if (source.bytes.front() == 0)
                    std::get<PhysicsCollisionConvexSource>(result.geometry).vertices.clear();
                if (source.bytes.front() == 2)
                    std::get<PhysicsCollisionConvexSource>(result.geometry).settings.limits.maxSourceVertices = 3;
            }
            return Result<PhysicsCollisionSource>::Success(std::move(result));
        }

    private:
        void ThrowConfiguredException() const {
            if (kind_ == 3)
                throw std::runtime_error("private source text");
            if (kind_ == 7)
                throw 7;
            if (kind_ == 8)
                throw std::bad_alloc{};
            if (kind_ == 9)
                throw std::logic_error("private adapter invariant text");
        }

        unsigned kind_;
        std::uint8_t setting_;
    };

    Assets::CookSourceView SourceView() {
        return {Asset, Type, Target, ComputeSha256(std::as_bytes(std::span{SourceBytes})), SourceBytes, "assets/çarpışma shape.source"};
    }

    /** @brief Owns a disposable project with portable space/non-ASCII source navigation evidence. */
    struct Project final {
        std::filesystem::path root = std::filesystem::temp_directory_path() /
                                     ("horo collision " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        Project() {
            std::filesystem::create_directories(root / "assets");
            Write(1);
        }

        ~Project() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void Write(const unsigned char value) const {
            std::ofstream file(root / "assets/çarpışma shape.source", std::ios::binary | std::ios::trunc);
            file.put(static_cast<char>(value));
            REQUIRE(file.good());
        }

        Assets::AssetRecord Record() const {
            return {.id = Asset,
                    .type = Type,
                    .sourcePath = ProjectPath::Parse("assets/çarpışma shape.source").Value(),
                    .metadataPath = ProjectPath::Parse("assets/çarpışma shape.source.horo").Value()};
        }
    };

    std::shared_ptr<const Assets::CookerCatalogSnapshot> Catalog(std::shared_ptr<const SourceImporter> importer) {
        auto contribution = MakePhysicsCollisionCookerContribution(Type, Target, PhysicsTarget, std::move(importer));
        REQUIRE(contribution.HasValue());
        Assets::CookerCatalog catalog;
        REQUIRE(catalog.Register(std::move(contribution).Value()).HasValue());
        auto snapshot = catalog.Publish();
        REQUIRE(snapshot.HasValue());
        return snapshot.Value();
    }

    /** @brief Owns joined cook work and primes a verified last-good generation/cache for independent edge-case checks. */
    struct CookSession final {
        Project project;
        Assets::AssetRegistry registry;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
        std::shared_ptr<SourceImporter> importer = std::make_shared<SourceImporter>();
        Assets::AssetCookService service{jobs, Catalog(importer)};
        BuildOutputStore output{64};
        Assets::AssetCookRequest request;
        Assets::AssetCookReport first;
        BuildOutputSnapshot snapshot;
        Assets::AssetCookArtifact artifact;

        CookSession() {
            REQUIRE(registry.Publish({project.Record()}).status == Assets::AssetRegistryBuildStatus::Complete);
            request = {.sourceRoot = project.root,
                       .cacheRoot = project.root / "cache",
                       .cookedRoot = project.root / "cooked",
                       .registry = registry.Snapshot(),
                       .target = Target,
                       .buildOutputStore = &output};
            const auto fresh = service.Cook(request, {});
            REQUIRE(fresh.HasValue());
            REQUIRE(fresh.Value().cookedAssets == 1);
            const auto second = service.Cook(request, {});
            REQUIRE(second.HasValue());
            REQUIRE(second.Value().cacheHits == 1);
            REQUIRE(importer->calls == 1);
            REQUIRE(second.Value().generation.manifestDigest == fresh.Value().generation.manifestDigest);
            const auto queried = output.SnapshotIfChanged(0);
            REQUIRE(queried.has_value());
            REQUIRE(std::ranges::any_of(queried->records, [](const BuildOutputRecord &record) {
                return record.code.Value() == "asset.cook.cache_hit" && record.result == BuildOutputResult::Cached &&
                       record.source.has_value();
            }));
            auto contents = Assets::ReadCookGenerationContents(fresh.Value().generation, 1024 * 1024);
            REQUIRE(contents.HasValue());
            auto decoded = Assets::DecodeCookedArtifact(contents.Value().artifacts.front());
            REQUIRE(decoded.HasValue());
            REQUIRE(InspectPhysicsCollisionArtifact(Asset, decoded.Value().payload, PhysicsTarget).HasValue());

            first = fresh.Value();
            snapshot = *queried;
            artifact = std::move(decoded).Value();
        }
    };
}  // namespace

TEST_CASE("Collision contribution cooks each qualified source and validates without reimport", "[physics][collision-cook]") {
    for (unsigned kind = 0; kind < 3; ++kind) {
        auto importer = std::make_shared<SourceImporter>(kind);
        auto contribution = MakePhysicsCollisionCookerContribution(Type, Target, PhysicsTarget, importer);
        REQUIRE(contribution.HasValue());
        REQUIRE(importer->calls == 0);
        const auto &strategy = *contribution.Value().strategy;
        auto cooked = strategy.Cook(SourceView(), {});
        REQUIRE(cooked.HasValue());
        REQUIRE(strategy.ValidateCookedPayload(SourceView(), cooked.Value().payload).HasValue());
        REQUIRE(importer->calls == 1);
        auto inspected = InspectPhysicsCollisionArtifact(Asset, cooked.Value().payload, PhysicsTarget);
        REQUIRE(inspected.HasValue());
        REQUIRE(inspected.Value().descriptor.subresource.Value() == 9);
        REQUIRE(inspected.Value().descriptor.kind == static_cast<PhysicsCookedShapeKind>(kind));
        REQUIRE(inspected.Value().sourceDigest == SourceView().sourceDigest);
        auto renamed = SourceView();
        const std::string longContext(4096, 'x');
        renamed.sourceContext = longContext;
        auto renamedCook = strategy.Cook(renamed, {});
        REQUIRE(renamedCook.HasValue());
        REQUIRE(renamedCook.Value().payload == cooked.Value().payload);
        auto repeated = strategy.Cook(SourceView(), {});
        REQUIRE(repeated.HasValue());
        REQUIRE(repeated.Value().payload == cooked.Value().payload);

        auto wrongTarget = PhysicsTarget;
        wrongTarget.digest.bytes[0] ^= 1;
        REQUIRE(InspectPhysicsCollisionArtifact(Asset, cooked.Value().payload, wrongTarget).HasError());
        const auto wrongAsset = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000855").Value();
        REQUIRE(InspectPhysicsCollisionArtifact(wrongAsset, cooked.Value().payload, PhysicsTarget).HasError());
        auto changedSource = SourceView();
        changedSource.sourceDigest.bytes[0] ^= 1;
        Test::RequireError(strategy.ValidateCookedPayload(changedSource, cooked.Value().payload), PhysicsErrors::ShapeArtifactInvalid);
        Test::RequireError(strategy.Cook(changedSource, {}), PhysicsErrors::ShapeCookSourceInvalid);

        for (const std::size_t offset : {0U, 4U, 5U, 13U, 45U, 77U, 173U}) {
            auto corrupt = cooked.Value().payload;
            corrupt[offset] ^= 0xff;
            REQUIRE(InspectPhysicsCollisionArtifact(Asset, corrupt, PhysicsTarget).HasError());
        }
        const std::span<const std::uint8_t> truncated{cooked.Value().payload.data(), 12};
        Test::RequireError(InspectPhysicsCollisionArtifact(Asset, truncated, PhysicsTarget), PhysicsErrors::ShapeArtifactInvalid);
    }
}

TEST_CASE("Collision import admission isolates target and importer configuration", "[physics][collision-cook]") {
    Test::RequireError(MakePhysicsCollisionCookerContribution(Type, Target, PhysicsTarget, nullptr), PhysicsErrors::DescriptorInvalid);
    const auto first = MakePhysicsCollisionCookerContribution(Type, Target, PhysicsTarget, std::make_shared<SourceImporter>());
    const auto second = MakePhysicsCollisionCookerContribution(Type, Target, PhysicsTarget, std::make_shared<SourceImporter>(0, 1));
    REQUIRE(first.HasValue());
    REQUIRE(second.HasValue());
    REQUIRE(first.Value().strategy->CacheIdentity().settingsDigest != second.Value().strategy->CacheIdentity().settingsDigest);
    auto cooked = first.Value().strategy->Cook(SourceView(), {});
    REQUIRE(cooked.HasValue());
    Test::RequireError(second.Value().strategy->ValidateCookedPayload(SourceView(), cooked.Value().payload),
                       PhysicsErrors::ShapeArtifactInvalid);
    auto wrongSource = SourceView();
    wrongSource.target = AssetCookTargetId::Parse("other-target").Value();
    Test::RequireError(first.Value().strategy->Cook(wrongSource, {}), PhysicsErrors::ProfileUnsupported);
    Test::RequireError(first.Value().strategy->ValidateCookedPayload(wrongSource, cooked.Value().payload),
                       PhysicsErrors::ProfileUnsupported);
    const std::array<std::uint8_t, 1> limitedBytes{2};
    auto limitedSource = SourceView();
    limitedSource.bytes = limitedBytes;
    limitedSource.sourceDigest = ComputeSha256(std::as_bytes(std::span{limitedBytes}));
    Test::RequireError(first.Value().strategy->Cook(limitedSource, {}), PhysicsErrors::ShapeCookLimitExceeded);
    for (const std::size_t offset : {109U, 141U}) {
        auto corrupt = cooked.Value().payload;
        corrupt[offset] ^= 1;
        Test::RequireError(first.Value().strategy->ValidateCookedPayload(SourceView(), corrupt), PhysicsErrors::ShapeArtifactInvalid);
    }
    CancellationSource cancelled;
    cancelled.RequestCancellation();
    const auto result = first.Value().strategy->Cook(SourceView(), cancelled.Token());
    REQUIRE(result.HasError());
    REQUIRE(IsJobCancelled(result.ErrorValue()));
    REQUIRE(ErrorChainContains(result.ErrorValue(), PhysicsErrors::ShapeCookCancelled.domain, PhysicsErrors::ShapeCookCancelled.code));
    const auto throwing = MakePhysicsCollisionCookerContribution(Type, Target, PhysicsTarget, std::make_shared<SourceImporter>(3));
    REQUIRE(throwing.HasValue());
    Test::RequireError(throwing.Value().strategy->Cook(SourceView(), {}), PhysicsErrors::ShapeCookSourceInvalid);
}

TEST_CASE("Collision Assets cook publishes verified cache hits and source-free artifacts", "[physics][collision-cook]") {
    CookSession session;
    REQUIRE(session.first.cookedAssets == 1);
    REQUIRE(session.snapshot.records.back().result == BuildOutputResult::Succeeded);
}

TEST_CASE("Collision domain-invalid cache fails with source context before reuse or reimport", "[physics][collision-cook]") {
    CookSession session;
    auto invalidArtifact = session.artifact;
    invalidArtifact.payload[141] ^= 1;  // Configuration digest; geometry itself remains intact.
    invalidArtifact.payloadDigest = ComputeSha256(std::as_bytes(std::span{invalidArtifact.payload}));
    auto encoded = Assets::EncodeCookedArtifact(invalidArtifact);
    REQUIRE(encoded.HasValue());
    session.request.cacheRoot = session.project.root / "invalid cache";
    Assets::AssetCookCache cache{session.request.cacheRoot};
    REQUIRE(cache.Store({invalidArtifact.cacheKeyDigest}, encoded.Value(), {}).HasValue());
    const auto invalid = session.service.Cook(session.request, {});
    Test::RequireError(invalid, PhysicsErrors::ShapeArtifactInvalid);
    REQUIRE(session.importer->calls == 1);
    const auto invalidOutput = session.output.SnapshotIfChanged(session.snapshot.revision);
    REQUIRE(invalidOutput.has_value());
    REQUIRE(std::ranges::any_of(invalidOutput->records, [&session](const BuildOutputRecord &record) {
        return record.stage == "cache_check" && record.code.Value() == PhysicsErrors::ShapeArtifactInvalid.code.Value() &&
               record.result == BuildOutputResult::Failed && record.source.has_value() &&
               record.source->absolutePath == (session.project.root / session.project.Record().sourcePath.String()).string();
    }));
    REQUIRE(Assets::ResolveCurrentCookGeneration(session.request.cookedRoot).Value().manifestDigest ==
            session.first.generation.manifestDigest);
}

TEST_CASE("Collision importer findings preserve source line and diagnostic detail", "[physics][collision-cook]") {
    CookSession session;
    Assets::AssetCookService diagnosticService(session.jobs, Catalog(std::make_shared<SourceImporter>(6)));
    const auto failed = diagnosticService.Cook(session.request, {});
    Test::RequireError(failed, PhysicsErrors::ShapeCookSourceInvalid);
    const auto diagnosticOutput = session.output.SnapshotIfChanged(session.snapshot.revision);
    REQUIRE(diagnosticOutput.has_value());
    const auto finding = std::ranges::find_if(diagnosticOutput->records, [](const BuildOutputRecord &record) {
        return record.message == "Expected finite vertex coordinates";
    });
    REQUIRE(finding != diagnosticOutput->records.end());
    REQUIRE(finding->result == BuildOutputResult::None);
    REQUIRE(finding->severity == DiagnosticSeverity::Error);
    REQUIRE(finding->source.has_value());
    REQUIRE(finding->source->line == 7);
    REQUIRE(finding->source->column == 3);
    REQUIRE(finding->source->absolutePath == (session.project.root / session.project.Record().sourcePath.String()).string());
}

TEST_CASE("Collision invalid source and cancellation retain the last-good generation", "[physics][collision-cook]") {
    CookSession session;
    session.project.Write(0);
    const auto failed = session.service.Cook(session.request, {});
    Test::RequireError(failed, PhysicsErrors::ShapeCookSourceInvalid);
    const auto current = Assets::ResolveCurrentCookGeneration(session.request.cookedRoot);
    REQUIRE(current.HasValue());
    REQUIRE(current.Value().manifestDigest == session.first.generation.manifestDigest);
    const auto failedOutput = session.output.SnapshotIfChanged(session.snapshot.revision);
    REQUIRE(failedOutput.has_value());
    const auto finding = std::ranges::find_if(failedOutput->records, [](const BuildOutputRecord &record) {
        return record.code.Value() == PhysicsErrors::ShapeCookSourceInvalid.code.Value();
    });
    REQUIRE(finding != failedOutput->records.end());
    REQUIRE(finding->result == BuildOutputResult::Failed);
    REQUIRE(finding->message.find("subresource 9") != std::string::npos);
    REQUIRE(finding->source->absolutePath == (session.project.root / session.project.Record().sourcePath.String()).string());

    Assets::AssetCookService cancellingService(session.jobs, Catalog(std::make_shared<SourceImporter>(4)));
    const auto cancelled = cancellingService.Cook(session.request, {});
    REQUIRE(cancelled.HasError());
    REQUIRE(ErrorChainContains(cancelled.ErrorValue(), PhysicsErrors::ShapeCookCancelled.domain, PhysicsErrors::ShapeCookCancelled.code));
    const auto cancelledOutput = session.output.SnapshotIfChanged(failedOutput->revision);
    REQUIRE(cancelledOutput.has_value());
    REQUIRE(cancelledOutput->records.back().result == BuildOutputResult::Cancelled);
    REQUIRE(std::ranges::any_of(cancelledOutput->records, [](const BuildOutputRecord &record) {
        return record.code.Value() == PhysicsErrors::ShapeCookCancelled.code.Value() && record.result == BuildOutputResult::None;
    }));
    REQUIRE(Assets::ResolveCurrentCookGeneration(session.request.cookedRoot).Value().manifestDigest ==
            session.first.generation.manifestDigest);
}

TEST_CASE("Collision unexpected importer exceptions fail joined work without replacing the last-good generation",
          "[physics][collision-cook]") {
    CookSession session;
    const unsigned kind = GENERATE(7U, 9U);
    auto foreignImporter = std::make_shared<SourceImporter>(kind);
    Assets::AssetCookService foreignService(session.jobs, Catalog(foreignImporter));
    const auto failed = foreignService.Cook(session.request, {});
    REQUIRE(failed.HasError());
    REQUIRE_FALSE(IsJobCancelled(failed.ErrorValue()));
    REQUIRE(foreignImporter->calls == 1);
    const auto failedOutput = session.output.SnapshotIfChanged(session.snapshot.revision);
    REQUIRE(failedOutput.has_value());
    REQUIRE(failedOutput->records.back().result == BuildOutputResult::Failed);
    const auto finding = std::ranges::find_if(failedOutput->records, [](const BuildOutputRecord &record) {
        return record.code.Value() == "asset.cook.cooker_failed";
    });
    REQUIRE(finding != failedOutput->records.end());
    REQUIRE(finding->result == BuildOutputResult::Failed);
    REQUIRE(finding->severity == DiagnosticSeverity::Error);
    REQUIRE(finding->source.has_value());
    REQUIRE(finding->source->absolutePath == (session.project.root / session.project.Record().sourcePath.String()).string());
    REQUIRE(finding->message == (kind == 9 ? "private adapter invariant text" : "The asset cooker threw before publication."));
    REQUIRE(Assets::ResolveCurrentCookGeneration(session.request.cookedRoot).Value().manifestDigest ==
            session.first.generation.manifestDigest);
    REQUIRE(session.service.Cook(session.request, {}).HasValue());
}

TEST_CASE("Collision importer allocation failures retain the stable Physics limit category", "[physics][collision-cook]") {
    const auto contribution = MakePhysicsCollisionCookerContribution(Type, Target, PhysicsTarget, std::make_shared<SourceImporter>(8));
    REQUIRE(contribution.HasValue());
    Test::RequireError(contribution.Value().strategy->Cook(SourceView(), {}), PhysicsErrors::ShapeCookLimitExceeded);
}

TEST_CASE("Collision contribution retains its immutable importer for the strategy lifetime", "[physics][collision-cook]") {
    auto importer = std::make_shared<SourceImporter>();
    const std::weak_ptr<const IPhysicsCollisionSourceImporter> lifetime = importer;
    {
        auto contribution = MakePhysicsCollisionCookerContribution(Type, Target, PhysicsTarget, importer);
        REQUIRE(contribution.HasValue());
        importer.reset();
        REQUIRE_FALSE(lifetime.expired());
        REQUIRE(contribution.Value().strategy->Cook(SourceView(), {}).HasValue());
    }
    REQUIRE(lifetime.expired());
}
