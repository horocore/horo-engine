#include "PCGPointTransformationTestSupport.h"

namespace Horo::PCG {
    TEST_CASE("Typed Perlin and Simplex outputs preserve all input identity columns", "[unit][pcg][transformation]") {
        JobSystem jobs{JobSystemConfig{.workerCount = 2, .maxQueuedJobs = 8}};
        for (auto kind : {PCGCpuNodeKind::PerlinNoise, PCGCpuNodeKind::SimplexNoise})
            for (auto type : {PCGAttributeType::Scalar, PCGAttributeType::Vector2, PCGAttributeType::Vector3, PCGAttributeType::Vector4}) {
                PCGPointNoiseSettings s{PCGAttributeKey::Create("test.noise").Value()};
                s.type = type;
                s.octaves = 3;
                s.normalization = PCGPointNoiseNormalization::UnitInterval;
                const auto plan = Plan(kind, s);
                const auto candidate = Evaluate(plan, Spatial(), &s);
                const auto parallel = Evaluate(plan, Spatial(), &s, 2, &jobs);
                SameIdentity(Points(candidate), Points(parallel));
                CHECK(Points(candidate).PointCount() == 4);
                ValidIdentity(Points(candidate));
                CHECK(Points(candidate).Schema().Find(s.attribute.Value())->type == type);
                switch (type) {
                    case PCGAttributeType::Scalar:
                        CHECK(*Points(candidate).FindColumn<PCGScalarColumn>(s.attribute.Value()) ==
                              *Points(parallel).FindColumn<PCGScalarColumn>(s.attribute.Value()));
                        break;
                    case PCGAttributeType::Vector2:
                        CHECK(*Points(candidate).FindColumn<PCGVector2Column>(s.attribute.Value()) ==
                              *Points(parallel).FindColumn<PCGVector2Column>(s.attribute.Value()));
                        break;
                    case PCGAttributeType::Vector3:
                        CHECK(*Points(candidate).FindColumn<PCGVector3Column>(s.attribute.Value()) ==
                              *Points(parallel).FindColumn<PCGVector3Column>(s.attribute.Value()));
                        break;
                    case PCGAttributeType::Vector4:
                        CHECK(*Points(candidate).FindColumn<PCGVector4Column>(s.attribute.Value()) ==
                              *Points(parallel).FindColumn<PCGVector4Column>(s.attribute.Value()));
                        break;
                    default:
                        FAIL("Unexpected floating type");
                }
                if (type == PCGAttributeType::Scalar)
                    for (auto value : *Points(candidate).FindColumn<PCGScalarColumn>(s.attribute.Value()))
                        CHECK((value >= 0 && value <= 1));
            }
    }

    TEST_CASE("Gradient noise reference lattice and normalization are exact", "[unit][pcg][transformation]") {
        PCGPointNoiseSettings s{PCGAttributeKey::Create("test.noise").Value()};
        for (auto kind : {PCGCpuNodeKind::PerlinNoise, PCGCpuNodeKind::SimplexNoise}) {
            const auto origin = detail::PointNoise(s, kind, {}, 123, {});
            REQUIRE(origin.HasValue());
            CHECK(origin.Value()[0] == 0);
            s.normalization = PCGPointNoiseNormalization::UnitInterval;
            const auto unit = detail::PointNoise(s, kind, {}, 123, {});
            REQUIRE(unit.HasValue());
            CHECK(unit.Value()[0] == 0.5);
            s.normalization = PCGPointNoiseNormalization::Signed;
        }
        CHECK(detail::PointNoise(s, PCGCpuNodeKind::PerlinNoise, {1, -2, 3}, 123, {}).Value()[0] == 0);
        // Independent exact-rational corner reference, seed123 and authored salt0.
        CHECK(detail::PointNoise(s, PCGCpuNodeKind::PerlinNoise, {0.25F, 0.5F, 0.75F}, 123, {}).Value()[0] ==
              Catch::Approx(0.1777210235595703).margin(1e-12));
        CHECK(detail::PointNoise(s, PCGCpuNodeKind::SimplexNoise, {0.25F, 0.5F, 0.75F}, 123, {}).Value()[0] ==
              Catch::Approx(0.332075).margin(1e-12));
        CHECK(detail::PointNoise(s, PCGCpuNodeKind::PerlinNoise, {0.25F, 0.5F, 0.75F}, 123, {}).Value()[0] !=
              detail::PointNoise(s, PCGCpuNodeKind::SimplexNoise, {0.25F, 0.5F, 0.75F}, 123, {}).Value()[0]);
    }

    TEST_CASE("Noise schema errors and coordinate overflow cannot publish candidates", "[unit][pcg][transformation]") {
        PCGPointNoiseSettings s{PCGAttributeKey::Create("test.noise").Value()};
        for (auto bad : {PCGAttributeType::Boolean, PCGAttributeType::SignedInteger, PCGAttributeType::UnsignedInteger}) {
            s.type = bad;
            CHECK(EncodePCGPointTransformationSettings(s).HasError());
        }
        s.type = PCGAttributeType::Scalar;
        s.attribute = PCGAttributeKey::Create(PCGSourceSampleAttribute).Value();
        CHECK(EncodePCGPointTransformationSettings(s).HasError());
        s.attribute = PCGAttributeKey::Create("test.noise").Value();
        s.octaves = 17;
        CHECK(EncodePCGPointTransformationSettings(s).HasError());
        s.octaves = 1;
        const auto plan = Plan(PCGCpuNodeKind::PerlinNoise, s);
        auto bounds = Bounds();
        CHECK(EvaluatePCGCpu(plan, Spatial(), bounds, {}, Limits(plan)).HasError());
        bounds = Bounds(&s);
        s.frequency = 1e100;
        const auto overflow = Plan(PCGCpuNodeKind::PerlinNoise, s);
        CHECK(EvaluatePCGCpu(overflow, Spatial({1, 0, 0}), bounds, {}, Limits(overflow)).HasError());
    }

    TEST_CASE("Noise replacement requires exact existing type and preserves schema", "[unit][pcg][transformation]") {
        PCGPointNoiseSettings noise{PCGAttributeKey::Create("test.scalar").Value()};
        const auto plan = Plan(PCGCpuNodeKind::PerlinNoise, noise);
        const auto candidate = Evaluate(plan, Spatial(), &noise);
        CHECK(Points(candidate).Schema().Attributes().size() == Schema()->Attributes().size());
        ValidIdentity(Points(candidate));
        noise.type = PCGAttributeType::Vector2;
        const auto wrong = Plan(PCGCpuNodeKind::PerlinNoise, noise);
        const auto bounds = Bounds(&noise);
        CHECK(EvaluatePCGCpu(wrong, Spatial(), bounds, {}, Limits(wrong)).HasError());
        noise.type = PCGAttributeType::Scalar;
        const auto admitted = detail::DecodePointTransformation(plan.Nodes()[1]);
        REQUIRE(admitted.HasValue());
        auto smaller = Bounds(&noise);
        smaller[1].maximumPoints = 3;
        CHECK(detail::ValidatePointTransformation(admitted.Value(), smaller[0], smaller[1]).HasError());
    }

    TEST_CASE("Gradient noise fixed rational corpus covers channel order ties negatives and domain boundaries",
              "[unit][pcg][transformation]") {
        struct Reference {
            Math::Vec3 point;
            std::array<double, 4> perlin;
            std::array<double, 4> simplex;
        };

        // Literal oracle from independent exact-rational weighted-corner and simplex support calculations.
        const std::array<Reference, 7> references{
            {{{0.25F, 0.5F, 0.75F},
              {0.1777210235595703, 0.034800052642822266, 0.41738176345825195, -0.08120012283325195},
              {0.332075, 0.284234375, -0.01366875, 0.352578125}},
             {{-0.25F, -0.5F, -0.75F},
              {0.40540552139282227, -0.05226612091064453, -0.3116421699523926, -0.5420055389404297},
              {-0.04100625, -0.020503125, 0.006834375, 0.7461625}},
             {{0.5F, 0.5F, 0.5F}, {-0.125, 0.125, 0.375, -0.25}, {0.0, 0.0, 0.0, 0.0}},
             {{-1.0F, 0.0F, 0.0F}, {0.0, 0.0, 0.0, 0.0}, {0.0, 0.10787818930041153, -0.7600995884773663, 0.6522213991769548}},
             {{1048576.0F, 1048576.0F, 1048576.0F}, {0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}},
             {{-1048576.0F, -1048576.0F, -1048576.0F}, {0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}},
             {{1048576.0F, 0.5F, -0.5F},
              {0.0, 0.5, -0.25, -0.125},
              {-0.24170205761316874, 0.18965473251028805, 0.37611316872427986, 0.6130139917695473}}}};
        PCGPointNoiseSettings settings{PCGAttributeKey::Create("test.noise").Value()};
        for (const auto &reference : references)
            for (const auto kind : {PCGCpuNodeKind::PerlinNoise, PCGCpuNodeKind::SimplexNoise})
                for (const auto type :
                     {PCGAttributeType::Scalar, PCGAttributeType::Vector2, PCGAttributeType::Vector3, PCGAttributeType::Vector4}) {
                    settings.type = type;
                    const auto actual = detail::PointNoise(settings, kind, reference.point, 123, {});
                    REQUIRE(actual.HasValue());
                    const auto &expected = kind == PCGCpuNodeKind::PerlinNoise ? reference.perlin : reference.simplex;
                    const auto channels = static_cast<unsigned>(type) - static_cast<unsigned>(PCGAttributeType::Scalar) + 1;
                    for (unsigned channel = 0; channel < channels; ++channel)
                        CHECK(actual.Value()[channel] == Catch::Approx(expected[channel]).margin(1e-9));
                }
        CHECK(detail::PointNoise(settings, PCGCpuNodeKind::PerlinNoise, {1048576.125F, 0, 0}, 123, {}).HasError());
        CHECK(detail::PointNoise(settings, PCGCpuNodeKind::SimplexNoise, {-1048576.125F, 0, 0}, 123, {}).HasError());
        settings.octaves = 16;
        settings.persistence = 0;
        settings.lacunarity = 8;
        CHECK(detail::PointNoise(settings, PCGCpuNodeKind::PerlinNoise, {0.25F, 0, 0}, 123, {}).HasError());
    }

}  // namespace Horo::PCG
