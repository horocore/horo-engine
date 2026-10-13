#include "PCGPointTransformationTestSupport.h"

namespace Horo::PCG {
    using namespace PointTransformationTestSupport;

    TEST_CASE("Point transformation catalog preserves sparse and legacy identities", "[unit][pcg][transformation]") {
        using enum PCGCpuNodeKind;
        auto created = PCGNodeCatalog::Create(ProjectPCGCapabilities(PCGHostProfile::Interactive, Grant()).Value());
        REQUIRE(created.HasValue());
        auto owner = std::move(created).Value();
        for (auto kind : {SnapshotGrid, DensityFilter, Merge, Forward, GridJitter, Stratified, PoissonScatter, Transform, SurfaceAlignment,
                          RandomOffset, PerlinNoise, SimplexNoise})
            REQUIRE(owner.Register(kind).HasValue());
        const auto root = owner.Snapshot().Value();
        CHECK(root.Nodes().size() == 12);
        CHECK(PCGCpuNodeType(SnapshotGrid).Value().Value() == 0x5043470206000001ULL);
        CHECK(PCGCpuNodeType(Transform).Value().Value() == 0x504347020600000dULL);
        for (unsigned value = 7; value < 12; ++value) {
            CHECK(PCGCpuNodeType(static_cast<PCGCpuNodeKind>(value)).HasError());
            CHECK(owner.Register(static_cast<PCGCpuNodeKind>(value)).HasError());
        }
        CHECK(root.Find(PCGCpuNodeType(PerlinNoise).Value()).Value()->settings == PCGNodeSettingsSchema::PerlinNoiseV1);
        REQUIRE(owner.Close().HasValue());
        CHECK(root.Find(PCGCpuNodeType(Transform).Value()).HasValue());
    }

    TEST_CASE("Point transformation settings reject malformed versions lengths frames and finite values", "[unit][pcg][transformation]") {
        PCGPointTransformSettings settings;
        const auto bytes = EncodePCGPointTransformationSettings(settings);
        REQUIRE(bytes.HasValue());
        PCGCookedNode node;
        node.type = PCGCpuNodeType(PCGCpuNodeKind::Transform).Value();
        node.payload = bytes.Value();
        REQUIRE(detail::DecodePointTransformation(node).HasValue());
        node.payload.push_back(0);
        CHECK(detail::DecodePointTransformation(node).HasError());
        node.payload = bytes.Value();
        node.payload[3] = 2;
        CHECK(detail::DecodePointTransformation(node).HasError());
        node.payload = bytes.Value();
        node.payload[4] = 7;
        CHECK(detail::DecodePointTransformation(node).HasError());
        settings.delta.translation.x = std::numeric_limits<float>::infinity();
        CHECK(EncodePCGPointTransformationSettings(settings).HasError());
        settings = {};
        settings.delta.rotation = {0, 0, 0, 0};
        CHECK(EncodePCGPointTransformationSettings(settings).HasError());
        CHECK(EncodePCGPointTransformationSettings(PCGPointAlignmentSettings{-1}).HasError());
        CHECK(EncodePCGPointTransformationSettings(PCGPointRandomOffsetSettings{{-1, 0, 0}}).HasError());
    }

    TEST_CASE("Component transformation preserves complete records and historical catalog lifetime", "[unit][pcg][transformation]") {
        const auto spatial = Spatial();
        const auto baseline = Evaluate(Plan(PCGCpuNodeKind::Transform, PCGPointTransformSettings{}), spatial);
        PCGPointTransformSettings settings;
        settings.delta.translation = {1, 2, 3};
        settings.delta.scale = {2, 3, 4};
        const auto candidate = Evaluate(Plan(PCGCpuNodeKind::Transform, settings), spatial);
        CHECK(std::ranges::equal(Points(baseline).Bounds(), Points(candidate).Bounds(), SameBounds));
        CHECK(std::ranges::equal(Points(baseline).Densities(), Points(candidate).Densities()));
        REQUIRE(Points(candidate).PointCount() == 4);
        ValidIdentity(Points(candidate));
        for (std::size_t i = 0; i < 4; ++i) {
            CHECK(Points(candidate).Transforms()[i].translation ==
                  Points(baseline).Transforms()[i].translation + settings.delta.translation);
            CHECK(Points(candidate).Transforms()[i].scale == settings.delta.scale);
        }
    }

    TEST_CASE("Local component translation and quaternion composition have explicit reference order", "[unit][pcg][transformation]") {
        PCGPointTransformSettings settings;
        settings.space = PCGPointCoordinateSpace::PointLocal;
        settings.delta.translation = {1, 0, 0};
        Math::Transform source;
        source.rotation = Math::Quaternion::FromAxisAngle({0, 0, 1}, 1.5707963267948966F);
        const auto local = detail::TransformPoint(settings, Spatial(), source, 1, {});
        REQUIRE(local.HasValue());
        CHECK(local.Value().translation.x == Catch::Approx(0).margin(1e-6));
        CHECK(local.Value().translation.y == Catch::Approx(1).margin(1e-6));
        settings.space = PCGPointCoordinateSpace::Snapshot;
        const auto snapshot = detail::TransformPoint(settings, Spatial(), source, 1, {});
        REQUIRE(snapshot.HasValue());
        CHECK(snapshot.Value().translation == Math::Vec3{1, 0, 0});
    }

    TEST_CASE("Surface alignment covers boundaries antiparallel normals and missing coverage", "[unit][pcg][transformation]") {
        using enum PCGCpuNodeKind;
        const auto aligned = Evaluate(Plan(SurfaceAlignment, PCGPointAlignmentSettings{0}), Spatial({}, true, true));
        for (const auto &transform : Points(aligned).Transforms()) {
            const auto up = transform.rotation.Rotate({0, 1, 0});
            CHECK(up.y == Catch::Approx(-1));
            CHECK(transform.translation.y == 0);
        }
        const auto plan = Plan(SurfaceAlignment, PCGPointAlignmentSettings{0});
        const auto bounds = Bounds();
        CHECK(EvaluatePCGCpu(plan, Spatial({}, false), bounds, {}, Limits(plan)).HasError());
        CHECK(EvaluatePCGCpu(plan, Spatial({0, 0.1F, 0}), bounds, {}, Limits(plan)).HasError());
        const auto tilted = Evaluate(Plan(SurfaceAlignment, PCGPointAlignmentSettings{1}), Spatial({}, true, false, true));
        for (const auto &transform : Points(tilted).Transforms()) {
            const auto up = transform.rotation.Rotate({0, 1, 0});
            CHECK(up.y == Catch::Approx(std::sqrt(0.5)).margin(1e-6));
            CHECK(up.z == Catch::Approx(-std::sqrt(0.5)).margin(1e-6));
        }
    }

    TEST_CASE("Random displacement is semantic-seed stable bounded and worker invariant", "[unit][pcg][transformation]") {
        const auto plan =
            Plan(PCGCpuNodeKind::RandomOffset, PCGPointRandomOffsetSettings{{1, 2, 3}, PCGPointCoordinateSpace::Snapshot, 83});
        const auto spatial = Spatial();
        const auto one = Evaluate(plan, spatial);
        JobSystem jobs{JobSystemConfig{.workerCount = 2, .maxQueuedJobs = 8}};
        const auto two = Evaluate(plan, spatial, nullptr, 2, &jobs);
        CHECK(std::ranges::equal(Points(one).Transforms(), Points(two).Transforms()));
        SameIdentity(Points(one), Points(two));
        const auto baseline = Evaluate(Plan(PCGCpuNodeKind::Transform, PCGPointTransformSettings{}), spatial);
        for (std::size_t i = 0; i < 4; ++i) {
            const auto delta = Points(one).Transforms()[i].translation - Points(baseline).Transforms()[i].translation;
            CHECK(std::abs(delta.x) <= 1);
            CHECK(std::abs(delta.y) <= 2);
            CHECK(std::abs(delta.z) <= 3);
        }
        Math::Transform source;
        const detail::PointTransformationSettings settings = PCGPointRandomOffsetSettings{{1, 2, 3}, PCGPointCoordinateSpace::Snapshot, 83};
        CHECK(detail::TransformPoint(settings, spatial, source, 7, {}).Value() ==
              detail::TransformPoint(settings, spatial, source, 7, {}).Value());
        CHECK(detail::TransformPoint(settings, spatial, source, 7, {}).Value() !=
              detail::TransformPoint(settings, spatial, source, 8, {}).Value());
    }

    TEST_CASE("Transformation work admission bounds surfaces and every octave before allocation", "[unit][pcg][transformation]") {
        const auto charge = detail::ChargePointTransformation(PCGPointAlignmentSettings{}, 4, 2);
        REQUIRE(charge.HasValue());
        CHECK(charge.Value().snapshotElementVisits == 8);
        CHECK(detail::ChargePointTransformation(PCGPointAlignmentSettings{}, std::numeric_limits<std::size_t>::max(), 2).HasError());
        PCGPointNoiseSettings s{PCGAttributeKey::Create("test.noise").Value()};
        s.attribute = PCGAttributeKey::Create("test.noise").Value();
        s.octaves = 16;
        const auto noise = detail::ChargePointTransformation(s, 4, 0);
        REQUIRE(noise.HasValue());
        CHECK(noise.Value().pointVisits >= 4 * 16 * 4 * 8);
        const auto plan = Plan(PCGCpuNodeKind::PerlinNoise, s);
        auto limits = Limits(plan);
        limits.maximumPointVisits = 1;
        const auto bounds = Bounds(&s);
        CHECK(EvaluatePCGCpu(plan, Spatial(), bounds, {}, limits).HasError());
    }

    TEST_CASE("Transformation finite failure and cancellation retain the last complete candidate", "[unit][pcg][transformation]") {
        using enum PCGCpuNodeKind;
        PCGPointTransformSettings s;
        s.delta.translation = {std::numeric_limits<float>::max(), 0, 0};
        const auto plan = Plan(Transform, s);
        const auto retained = Evaluate(Plan(Transform, PCGPointTransformSettings{}), Spatial());
        const auto bounds = Bounds();
        Math::Transform overflowing;
        overflowing.translation = {std::numeric_limits<float>::max(), 0, 0};
        CHECK(detail::TransformPoint(s, Spatial(), overflowing, 1, {}).HasError());
        CHECK(Points(retained).PointCount() == 4);
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        auto limits = Limits(plan);
        limits.retainedBytes = retained.ReservedBytes();
        CHECK(EvaluatePCGCpu(plan, Spatial(), bounds, {}, limits, PCGCpuAdmission::Accepting, cancellation.Token()).HasError());
        const auto recovered = Evaluate(Plan(RandomOffset, PCGPointRandomOffsetSettings{}), Spatial());
        CHECK(Points(recovered).PointCount() == 4);
    }

    TEST_CASE("Transformation dispatch preserves nonzero values of every exact attribute type", "[unit][pcg][transformation]") {
        const auto plan = Plan(PCGCpuNodeKind::Transform, PCGPointTransformSettings{});
        const auto spatial = Spatial();
        const auto reference = Evaluate(plan, spatial);
        const auto bounds = Bounds();
        const auto limits = Limits(plan);
        const auto workspace = PCGPointCloudWorkspace::Create(plan, bounds, limits.maximumScratchBytes);
        REQUIRE(workspace.HasValue());
        const auto writer = workspace.Value()->BeginOutput(0, Id<PinId>(101), 1);
        REQUIRE(writer.HasValue());
        const auto seed = reference.Provenance()[0].Seed(Id<SourceSampleId>(1)).Value();
        REQUIRE(writer.Value().SetPoint(0, {}, {{-1, -2, -3}, {1, 2, 3}}, 0.75F, seed).HasValue());
        PopulateColumns(writer.Value(), reference.Provenance()[0]);
        REQUIRE(workspace.Value()->SealOutput(0, Id<PinId>(101)).HasValue());
        REQUIRE(workspace.Value()->FinishNode(0).HasValue());
        const detail::NodeExecutionContext context{plan, spatial, *workspace.Value(), bounds, {}, 1, nullptr, {}, reference.Provenance()};
        REQUIRE(detail::ExecuteNode(context, 1).HasValue());
        const auto output = workspace.Value()->ReadFinal(1, Id<PinId>(202));
        REQUIRE(output.HasValue());
        CHECK(output.Value().PointCount() == 1);
        CHECK(output.Value().Seeds()[0] == seed);
        CHECK(output.Value().Densities()[0] == 0.75F);
        CHECK(output.Value().FindColumn<PCGBoolColumn>("test.boolean")[0] == 1);
        CHECK(output.Value().FindColumn<PCGSignedIntegerColumn>("test.signed")[0] == -7);
        CHECK(output.Value().FindColumn<PCGUnsignedIntegerColumn>("test.unsigned")[0] == std::numeric_limits<std::uint64_t>::max());
        CHECK(output.Value().FindColumn<PCGScalarColumn>("test.scalar")[0] == 0.125);
        CHECK(output.Value().FindColumn<PCGVector2Column>("test.vector2")[0] == Math::Vec2{1, 2});
        CHECK(output.Value().FindColumn<PCGVector3Column>("test.vector3")[0] == Math::Vec3{3, 4, 5});
        CHECK(output.Value().FindColumn<PCGVector4Column>("test.vector4")[0] == Math::Vec4{6, 7, 8, 9});
    }

    TEST_CASE("Transformation worker failure joins children before candidate revocation", "[unit][pcg][transformation]") {
        PCGPointNoiseSettings noise{PCGAttributeKey::Create("test.noise").Value()};
        noise.frequency = 1e100;
        const auto plan = Plan(PCGCpuNodeKind::SimplexNoise, noise);
        const auto bounds = Bounds(&noise);
        JobSystem jobs{JobSystemConfig{.workerCount = 2, .maxQueuedJobs = 8}};
        auto limits = Limits(plan);
        limits.workers = 2;
        limits.jobs = &jobs;
        CHECK(EvaluatePCGCpu(plan, Spatial({1, 0, 0}), bounds, {}, limits).HasError());
        const auto recovery = Plan(PCGCpuNodeKind::Transform, PCGPointTransformSettings{});
        const auto plain = Bounds();
        CHECK(EvaluatePCGCpu(recovery, Spatial(), plain, {}, limits).HasValue());
        jobs.Shutdown(ShutdownPolicy::Cancel);
        CHECK(EvaluatePCGCpu(recovery, Spatial(), plain, {}, limits).HasError());
    }

    TEST_CASE("All transformation codecs reject trailing truncated skewed and negative-zero payloads", "[unit][pcg][transformation]") {
        const PCGPointNoiseSettings noise{PCGAttributeKey::Create("test.noise").Value()};
        const std::array<std::pair<PCGCpuNodeKind, std::vector<std::uint8_t>>, 4> cases{
            {{PCGCpuNodeKind::SurfaceAlignment, EncodePCGPointTransformationSettings(PCGPointAlignmentSettings{}).Value()},
             {PCGCpuNodeKind::RandomOffset, EncodePCGPointTransformationSettings(PCGPointRandomOffsetSettings{}).Value()},
             {PCGCpuNodeKind::PerlinNoise, EncodePCGPointTransformationSettings(noise).Value()},
             {PCGCpuNodeKind::SimplexNoise, EncodePCGPointTransformationSettings(noise).Value()}}};
        for (const auto &[kind, bytes] : cases) {
            PCGCookedNode node;
            node.type = PCGCpuNodeType(kind).Value();
            node.payload = bytes;
            REQUIRE(detail::DecodePointTransformation(node).HasValue());
            node.payload.push_back(0);
            CHECK(detail::DecodePointTransformation(node).HasError());
            node.payload = bytes;
            node.payload.pop_back();
            CHECK(detail::DecodePointTransformation(node).HasError());
            node.payload = bytes;
            node.payload[3] = 2;
            CHECK(detail::DecodePointTransformation(node).HasError());
        }
        PCGCookedNode node;
        node.type = PCGCpuNodeType(PCGCpuNodeKind::SurfaceAlignment).Value();
        node.payload = EncodePCGPointTransformationSettings(PCGPointAlignmentSettings{0}).Value();
        node.payload[4] = 128;
        CHECK(detail::DecodePointTransformation(node).HasError());
        auto invalid = noise;
        invalid.persistence = std::numeric_limits<double>::quiet_NaN();
        CHECK(EncodePCGPointTransformationSettings(invalid).HasError());
        invalid = noise;
        invalid.lacunarity = 9;
        CHECK(EncodePCGPointTransformationSettings(invalid).HasError());
    }

    TEST_CASE("Near-antiparallel alignment retains the captured lateral normal", "[unit][pcg][transformation]") {
        PCGSpatialSnapshotCandidate source;
        source.snapshot = Id<SpatialSnapshotId>(7);
        source.provenance = {Id<SpatialProviderId>(1), Id<SpatialSourceId>(2), Id<SpatialRevision>(7)};
        source.bounds = {{-10, -10, -10}, {10, 10, 10}};
        source.coverage = PCGSpatialCoverage::Complete;
        const auto normal = Math::TryNormalize(Math::Vec3{0.001F, -1, 0});
        REQUIRE(normal.HasValue());
        source.surfaces = {{Id<SpatialElementId>(9), {{{-5, -0.005F, -5}, {5, 0.005F, -5}, {-5, -0.005F, 5}}}, normal.Value()}};
        const auto snapshot = CapturePCGSpatialSnapshot(std::move(source));
        REQUIRE(snapshot.HasValue());
        const auto aligned = detail::TransformPoint(PCGPointAlignmentSettings{1}, snapshot.Value(), {}, 1, {});
        REQUIRE(aligned.HasValue());
        const auto actual = aligned.Value().rotation.TryRotate({0, 1, 0});
        REQUIRE(actual.HasValue());
        CHECK(actual.Value().x == Catch::Approx(normal.Value().x).margin(1e-6));
        CHECK(actual.Value().y == Catch::Approx(normal.Value().y).margin(1e-6));
        CHECK(actual.Value().z == Catch::Approx(normal.Value().z).margin(1e-6));
        CHECK(actual.Value().x > 0.0009F);
    }
}  // namespace Horo::PCG
