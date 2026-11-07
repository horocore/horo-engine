#include "AllocationProbe.h"
#include "PhysicsStateConstructionManifestCodec.h"
#include "PhysicsStateConstructionManifestOwner.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Physics::StateDetail {
    namespace {
        template <class T> T Value(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().message : std::string{"success"}));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        struct OwnerFixture final {
            static constexpr PhysicsStateFpControl Fp{PhysicsStateFpArchitecture::ArmFpcr, 0};
            std::array<PhysicsStateShapeRecipe, 1> nodes{{{Fp, PhysicsShapeDescriptor{PhysicsBoxShape{}}}}};
            std::array<PhysicsStateShapeRecipeRoot, 1> roots{{{2, 0}}};
            std::array<PhysicsStateConstructionBody, 1> bodies{{{.id = 1,
                                                                 .shape = 2,
                                                                 .motionStorageReserved = true,
                                                                 .hasMotion = true,
                                                                 .floatingPoint = Fp,
                                                                 .massArithmeticSubprofile = Fp}}};
            std::array<PhysicsStatePersistentOwner, 2> owners{};
            std::array<std::uint32_t, 2> scratch{};
            std::array<std::byte, 2048> bytes{};
            PhysicsStateConstructionManifestLimits limits{.shapes = {1, 0, 1, 1, 4096},
                                                          .maximumBodies = 1,
                                                          .maximumRetainedBytes = 32768,
                                                          .maximumEncodedBytes = 2048,
                                                          .maximumOwners = 2};

            OwnerFixture() {
                owners[0] = {.generation = 3, .role = PhysicsStateOwnerRole::Body, .slot = 100, .object = 1};
                owners[1] = {.generation = 3, .role = PhysicsStateOwnerRole::Collider, .slot = 200, .object = 2};
                owners[0].entity[0] = 1;
                owners[1].entity[0] = 2;  // Same low64 bits; full high UUID bits remain distinguishing truth.
            }

            PhysicsStateConstructionManifest Manifest() const noexcept {
                return {.shapes = {nodes, {}, roots}, .bodies = bodies, .owners = owners};
            }

            Result<std::size_t> Encode() {
                return EncodeConstructionManifest(Manifest(), limits, Fp, scratch, bytes);
            }
        };

        void InvalidateOwner(OwnerFixture &fixture, std::uint32_t mode) {
            switch (mode) {
                case 0:
                    fixture.owners[0].entity.fill(0);
                    break;
                case 1:
                    fixture.owners[0].generation = 0;
                    break;
                case 2:
                    fixture.owners[0].role = static_cast<PhysicsStateOwnerRole>(255);
                    break;
                case 3:
                    fixture.owners[0].slot = 0;
                    break;
                case 4:
                    fixture.owners[0].object = 2;
                    break;
                case 5:
                    fixture.owners[1].role = PhysicsStateOwnerRole::Constraint;
                    break;
                case 6:
                    fixture.owners[1].entity = fixture.owners[0].entity;
                    fixture.owners[1].role = fixture.owners[0].role;
                    fixture.owners[1].slot = fixture.owners[0].slot;
                    break;
                case 7:
                    std::swap(fixture.owners[0].entity, fixture.owners[1].entity);
                    break;
            }
        }
    }  // namespace

    TEST_CASE("Physics persistent ownership stores all UUID bits with exact rank and closed wire extents",
              "[physics][state][ownership][codec]") {
        OwnerFixture fixture;
        std::array<std::byte, 41> row{};
        Wire writer{std::span<std::byte>{row}};
        auto owner = fixture.owners.front();
        ConstructionOwnerFields(writer, owner);
        REQUIRE(writer.Size() == ConstructionRowBytes[6]);
        REQUIRE(writer.Complete());
        const auto bytes = Value(fixture.Encode());
        auto storage = Value(ConstructionManifestOwner::Prepare(fixture.limits));
        REQUIRE(storage->Decode(std::span{fixture.bytes}.first(bytes), OwnerFixture::Fp).HasValue());
        CHECK(std::ranges::equal(storage->View().owners, fixture.owners));
        CHECK(storage->View().owners[0].entity[0] == 1);
        CHECK(storage->View().owners[1].entity[0] == 2);
    }

    TEST_CASE("Malformed persistent keys ranks and references fail before overwriting canonical output",
              "[physics][state][ownership][malformed]") {
        OwnerFixture fixture;
        Value(fixture.Encode());
        const auto before = fixture.bytes;
        const auto valid = fixture.owners;
        for (std::uint32_t mode = 0; mode < 8; ++mode) {
            fixture.owners = valid;
            InvalidateOwner(fixture, mode);
            CHECK(fixture.Encode().HasError());
            CHECK(fixture.bytes == before);
        }
    }

    TEST_CASE("Persistent owner key validation and canonical encoding reuse bounded scratch without allocation",
              "[physics][state][ownership][allocation]") {
        OwnerFixture fixture;
        Value(fixture.Encode());
        bool encoded{};
        Tests::AllocationProbe::Measurement measured;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            encoded = fixture.Encode().HasValue();
            measured = measurement.Snapshot();
        }
        REQUIRE(encoded);
        CHECK(measured.requests == 0);
        fixture.owners[1].entity = fixture.owners[0].entity;
        fixture.owners[1].role = PhysicsStateOwnerRole::CompoundBodyRoot;
        fixture.owners[1].slot = fixture.owners[0].slot;
        CHECK(fixture.Encode().HasValue());
        ++fixture.owners[1].slot;
        CHECK(fixture.Encode().HasError());
    }
}  // namespace Horo::Physics::StateDetail
