#include "PCGPointTransformationTestSupport.h"

namespace Horo::PCG {
    namespace {
        /** @brief One immutable point and its independent four-channel Perlin/Simplex oracle. */
        struct NoiseReference {
            Math::Vec3 point;
            std::array<double, 4> perlin;
            std::array<double, 4> simplex;
        };

        /** @brief Independent exact-rational literals cover channels, every axis order, pairwise ties and domain boundaries. */
        constexpr std::array<NoiseReference, 19> NoiseReferenceCorpus{
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
              {-0.24170205761316874, 0.18965473251028805, 0.37611316872427986, 0.6130139917695473}},
             {{0.0625F, 0.0625F, 0.125F},
              {-0.12293018126334539, 0.07603671766906041, 0.015189095963146632, 0.06975102074951889},
              {-0.44202370455265044, 0.2219739572405815, 0.0008933831810951233, 0.2201184690952301}},
             {{0.0625F, 0.125F, 0.0625F},
              {-0.1966102989513832, -0.06999145450282597, 0.07451917675752394, -0.07655058151721812},
              {-0.6629668350458146, -0.2201184690952301, 0.2219739572405815, -0.22190523545742036}},
             {{0.0625F, 0.125F, 0.125F},
              {-0.19393799262810285, 0.004486118890447699, 0.0863026146535737, -0.0049938081945835044},
              {-0.6102255810514092, 0.0068372882105410095, 0.21042113030999898, -0.006311342963576317}},
             {{0.0625F, 0.125F, 0.1875F},
              {-0.18601917274132931, 0.09171628981000617, 0.11134783433376035, 0.07532263725468309},
              {-0.52886707020998, 0.19885479103326797, 0.19716362317800523, 0.1583904237230619}},
             {{0.0625F, 0.1875F, 0.125F},
              {-0.27422955291931195, -0.07699903057017247, 0.1653440535919799, -0.0913002226679929},
              {-0.7041719829678535, -0.1567161187609037, 0.3756372750004133, -0.19548088676929473}},
             {{0.125F, 0.0625F, 0.0625F},
              {-0.1729935100977622, 0.013419075003407999, -0.07003208302743309, -0.01535131242742964},
              {-0.6631042786121368, 0.0008933831810951233, -0.2201184690952301, -0.0009621049642562867}},
             {{0.125F, 0.0625F, 0.125F},
              {-0.17233000517945918, 0.08521472471016489, -0.05421114050534159, 0.05520107558642939},
              {-0.6112774715453386, 0.21042113030999898, -0.19727249913588166, 0.19674655388891696}},
             {{0.125F, 0.0625F, 0.1875F},
              {-0.16916190506375645, 0.16728244564745184, -0.020910479489679265, 0.13432236952211257},
              {-0.5322494059205055, 0.3756991056084633, -0.15821617382764816, 0.33356886478265124}},
             {{0.125F, 0.125F, 0.0625F},
              {-0.24361891967781402, -0.056931500291549586, 0.00156810220896908, -0.08889605631302455},
              {-0.8143353683978319, -0.19727249913588166, 0.0068372882105410095, -0.21042113030999898}},
             {{0.125F, 0.1875F, 0.0625F},
              {-0.32175068391216605, -0.13439847932771873, 0.08179811540906999, -0.17354449867349148},
              {-0.8826990339517593, -0.33524316974480944, 0.1987760975321134, -0.3740248006463051}},
             {{0.1875F, 0.0625F, 0.125F},
              {-0.20402897456010538, 0.10409822434969578, -0.1305117219370352, 0.023694632107365582},
              {-0.7107286787072817, 0.19709336112340292, -0.33524316974480944, 0.15485070101022722}},
             {{0.1875F, 0.125F, 0.0625F},
              {-0.2708053481931375, -0.030160445300347713, -0.08069443638895198, -0.11404649129540385},
              {-0.8858902568737665, -0.15828643588225047, -0.1567161187609037, -0.1988463595867157}}}};
    }  // namespace

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
        PCGPointNoiseSettings settings{PCGAttributeKey::Create("test.noise").Value()};
        for (const auto &reference : NoiseReferenceCorpus)
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
