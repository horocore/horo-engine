#include "AllocationProbe.h"
#include "PhysicsStateConstructionManifestCodec.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Physics::StateDetail {
    namespace {
        constexpr PhysicsStateFpControl ModelFp{PhysicsStateFpArchitecture::ArmFpcr, 0};

        struct ManifestCodecFixture final {
            std::array<PhysicsStateShapeRecipe, 1> nodes{{{ModelFp, PhysicsShapeDescriptor{PhysicsBoxShape{}}}}}, decodedNodes;
            std::array<PhysicsStateShapeRecipeRoot, 1> roots{{{900, 0}}}, decodedRoots;
            std::array<PhysicsStateConstructionBody, 1> bodies{}, decodedBodies;
            std::array<PhysicsStateConstructionJoint, 1> joints{}, decodedJoints;
            std::array<std::uint32_t, 2> scratch{};
            std::array<std::byte, 2048> bytes{};
            PhysicsStateConstructionManifestLimits limits{{1, 0, 1, 1, 8192}, 1, 0, 0, 1, 0, 0, 8192, 2048};

            ManifestCodecFixture() {
                auto &b = bodies[0];
                b.floatingPoint = ModelFp;
                b.massArithmeticSubprofile = ModelFp;
                b.id = 100;
                b.shape = 900;
                b.policy.motion = PhysicsMotionType::Dynamic;
                b.policy.mass = PhysicsMass{1};
                b.hasMotion = true;
                b.motionStorageReserved = true;
                b.inverseMass = 1;
                b.inverseInertia = {6, 6, 6};
                b.inertiaRotation = {0, 0, 0, 1};
                joints[0].id = 8;
                joints[0].first = 100;
                joints[0].floatingPoint = ModelFp;
            }

            PhysicsStateConstructionManifest View() const noexcept {
                return {{nodes, {}, roots}, bodies, {}, {}, joints, {}, {}};
            }

            ConstructionManifestStorage Storage() noexcept {
                return {.shapes = {decodedNodes, {}, decodedRoots}, .bodies = decodedBodies, .joints = decodedJoints};
            }

            Result<std::size_t> Encode() {
                return EncodeConstructionManifest(View(), limits, ModelFp, scratch, bytes);
            }

            Result<PhysicsStateConstructionManifest> Decode(std::size_t count) {
                return DecodeConstructionManifest(std::span<std::byte>{bytes}.first(count), limits, ModelFp, scratch, Storage());
            }

            std::size_t BodyOffset() {
                Wire wire{std::span<const std::byte>{bytes}.first(ConstructionManifestHeader::Bytes)};
                ConstructionManifestHeader header;
                ConstructionManifestHeaderFields(wire, header);
                return ConstructionManifestHeader::Bytes + static_cast<std::size_t>(header.shapeBytes);
            }
        };

        template <class T, class Fields> std::size_t FieldExtent(T value, Fields fields) {
            std::array<std::byte, 512> bytes{};
            Wire wire{std::span<std::byte>{bytes}};
            fields(wire, value);
            REQUIRE_FALSE(wire.Failed());
            return wire.Size();
        }
    }  // namespace

    TEST_CASE("Construction row scalar grammar matches explicit canonical extents", "[unit][physics][state][construction-codec]") {
        CHECK(FieldExtent(PhysicsStateConstructionBody{}, [](Wire &w, auto &v) {
            CHECK(ConstructionBodyFields(w, v, false));
        }) == ConstructionRowBytes[0]);
        CHECK(FieldExtent(PhysicsStateConstructionCollider{}, ConstructionColliderFields) == ConstructionRowBytes[1]);
        CHECK(FieldExtent(PhysicsQueryMaterial{}, ConstructionMaterialFields) == ConstructionRowBytes[2]);
        CHECK(FieldExtent(PhysicsStateConstructionJoint{}, [](Wire &w, auto &v) {
            CHECK(ConstructionJointFields(w, v, false));
        }) == ConstructionRowBytes[3]);
        CHECK(FieldExtent(PhysicsStateConstructionFixture{}, ConstructionFixtureFields) == ConstructionRowBytes[4]);
        CHECK(FieldExtent(PhysicsStateConstructionQueryChild{}, ConstructionQueryChildFields) == ConstructionRowBytes[5]);
    }

    TEST_CASE("Complete canonical construction record roundtrips without C++ heap requests", "[unit][physics][state][construction-codec]") {
        ManifestCodecFixture f;
        const auto before = Tests::AllocationProbe::Count();
        auto written = f.Encode();
        const auto afterWrite = Tests::AllocationProbe::Count();
        INFO((written.HasError() ? written.ErrorValue().message : "success"));
        REQUIRE(written.HasValue());
        const auto beforeRead = Tests::AllocationProbe::Count();
        auto decoded = f.Decode(written.Value());
        const auto afterRead = Tests::AllocationProbe::Count();
        INFO((decoded.HasError() ? decoded.ErrorValue().message : "success"));
        REQUIRE(decoded.HasValue());
        CHECK(before == afterWrite);
        CHECK(beforeRead == afterRead);
        CHECK(decoded.Value().bodies[0].id == 100);
        CHECK((decoded.Value().bodies[0].inverseInertia == std::array<float, 3>{6, 6, 6}));
        CHECK(decoded.Value().joints[0].first == 100);
        CHECK(written.Value() == ConstructionManifestHeader::Bytes + 51 + ConstructionRowBytes[0] + ConstructionRowBytes[3]);
    }

    TEST_CASE("Every truncated full construction record issues no admitted manifest", "[unit][physics][state][construction-codec]") {
        ManifestCodecFixture f;
        auto written = f.Encode();
        REQUIRE(written.HasValue());
        for (std::size_t count = 0; count < written.Value(); ++count)
            CHECK(f.Decode(count).HasError());
    }

    TEST_CASE("Construction record rejects data concealed behind absent profile and fixed-joint tags",
              "[unit][physics][state][construction-codec]") {
        ManifestCodecFixture f;
        auto written = f.Encode();
        REQUIRE(written.HasValue());
        const auto body = f.BodyOffset();
        SECTION("absent profile UUID") {
            f.bytes[body + 98] = std::byte{1};
        }
        SECTION("fixed joint unused minimum") {
            const auto joint = body + ConstructionRowBytes[0];
            f.bytes[joint + 82 + 2] = std::byte{0x80};
            f.bytes[joint + 82 + 3] = std::byte{0x3f};
        }
        CHECK(f.Decode(written.Value()).HasError());
    }

    TEST_CASE("Construction record rejects trailing data and independent byte or retained budgets",
              "[unit][physics][state][construction-codec]") {
        ManifestCodecFixture f;
        auto written = f.Encode();
        REQUIRE(written.HasValue());
        SECTION("trailing") {
            CHECK(f.Decode(written.Value() + 1).HasError());
        }
        SECTION("encoded budget") {
            f.limits.maximumEncodedBytes = written.Value() - 1;
            CHECK(f.Decode(written.Value()).HasError());
        }
        SECTION("retained budget") {
            f.limits.maximumRetainedBytes = 1;
            CHECK(f.Decode(written.Value()).HasError());
        }
    }

    TEST_CASE("Construction encode cannot overwrite still-borrowed typed source input", "[unit][physics][state][construction-codec]") {
        ManifestCodecFixture f;
        const auto original = f.bodies[0].inverseMass;
        auto result = EncodeConstructionManifest(f.View(), f.limits, ModelFp, f.scratch, std::as_writable_bytes(std::span{f.bodies}));
        CHECK(result.HasError());
        CHECK(f.bodies[0].inverseMass == original);
    }
}  // namespace Horo::Physics::StateDetail
