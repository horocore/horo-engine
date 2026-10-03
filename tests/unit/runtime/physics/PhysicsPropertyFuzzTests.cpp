#include "Horo/Physics/PhysicsBodyDescriptor.h"
#include "Horo/Physics/PhysicsConvexHullCook.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsTriangleMeshCook.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "Horo/Physics/PhysicsWorldDescriptor.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <thread>

namespace Horo::Physics {
    namespace {
        // Fixed algorithm and seeds make a failed case portable across standard libraries.
        class Generator final {
        public:
            explicit Generator(const std::uint64_t seed) : m_state(seed) {}

            [[nodiscard]] std::uint64_t Next() noexcept {
                m_state += 0x9e3779b97f4a7c15ULL;
                std::uint64_t value = m_state;
                value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
                value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
                return value ^ (value >> 31U);
            }

            [[nodiscard]] float Bounded(const std::uint32_t maximum) noexcept {
                return static_cast<float>(Next() % maximum);
            }

        private:
            std::uint64_t m_state;
        };

        constexpr std::array<std::uint64_t, 3> Seeds{0x922ULL, 0x504859ULL, 0xdeadbeefULL};
        constexpr std::uint32_t CasesPerSeed = 32;

        [[nodiscard]] Assets::AssetId Asset() {
            return Assets::AssetId::Parse("10000000-0000-0000-0000-000000000922").Value();
        }

        [[nodiscard]] PhysicsShapeCookTargetDigest Target() {
            PhysicsShapeCookTargetDigest target;
            target.digest.bytes[0] = 9;
            return target;
        }

        [[nodiscard]] PhysicsConvexHullCookRequest HullRequest(const std::span<const Math::Vec3> vertices) {
            return {.asset = Asset(),
                    .subresource = PhysicsShapeSubresourceId::FromValue(5),
                    .vertices = vertices,
                    .target = Target(),
                    .sourceContext = "property/hull"};
        }

        [[nodiscard]] PhysicsTriangleMeshCookRequest MeshRequest(const std::span<const Math::Vec3> vertices,
                                                                 const std::span<const PhysicsTriangleMeshSourceTriangle> triangles,
                                                                 const std::span<const PhysicsMaterialSlotId> slots) {
            return {.asset = Asset(),
                    .subresource = PhysicsShapeSubresourceId::FromValue(6),
                    .vertices = vertices,
                    .triangles = triangles,
                    .materialSlots = slots,
                    .target = Target(),
                    .sourceContext = "property/mesh"};
        }

        [[nodiscard]] std::array<Math::Vec3, 8> Cube(const float offset, const float extent) {
            return {{{offset - extent, -extent, -extent},
                     {offset - extent, -extent, extent},
                     {offset - extent, extent, -extent},
                     {offset - extent, extent, extent},
                     {offset + extent, -extent, -extent},
                     {offset + extent, -extent, extent},
                     {offset + extent, extent, -extent},
                     {offset + extent, extent, extent}}};
        }

        [[nodiscard]] PhysicsStructuralCommand Command(const std::uint64_t worldGeneration, const std::uint64_t targetIdentity,
                                                       const std::uint64_t sourceSequence) {
            return {.order = {.simulationTick = 1,
                              .worldGeneration = worldGeneration,
                              .sceneGeneration = 1,
                              .targetKind = PhysicsCommandTargetKind::Body,
                              .targetIdentity = targetIdentity,
                              .commandKind = PhysicsStructuralCommandKind::Create,
                              .source = PhysicsCommandSourceId::Create(1).Value(),
                              .sourceSequence = sourceSequence}};
        }

        /** @brief Checks that arbitrary floating-point mass bits obey the authored-body bound. */
        void CheckArbitraryMass(Generator &generator, const PhysicsAuthoredBodyDescriptor &body) {
            auto arbitraryBody = body;
            const float mass = std::bit_cast<float>(static_cast<std::uint32_t>(generator.Next()));
            arbitraryBody.mass = PhysicsMass{mass};
            const bool admissibleMass = std::isfinite(mass) && mass >= MinimumPhysicsMassKilograms && mass <= MaximumPhysicsMassKilograms;
            const auto massResult = ValidatePhysicsAuthoredBodyDescriptor(arbitraryBody);
            REQUIRE(massResult.HasValue() == admissibleMass);
            if (!admissibleMass)
                REQUIRE(massResult.ErrorValue().code.Value() == PhysicsErrors::DescriptorInvalid.code.Value());
        }

        /** @brief Checks deterministic convex cooking and malformed payload/source rejection. */
        void CheckConvexCase(Generator &generator) {
            INFO("stage=convex cook/load source=property/hull subresource=5");
            auto cube = Cube(generator.Bounded(16), 1.0F + generator.Bounded(4));
            const auto originalCube = cube;
            const auto hull = CookPhysicsConvexHull(HullRequest(cube));
            REQUIRE(hull.HasValue());
            REQUIRE(cube == originalCube);
            REQUIRE(LoadCookedPhysicsConvexHull(hull.Value().descriptor, Target(), hull.Value().payload).HasValue());
            auto renamedHullRequest = HullRequest(cube);
            renamedHullRequest.sourceContext = "property/renamed-hull";
            const auto renamedHull = CookPhysicsConvexHull(renamedHullRequest);
            REQUIRE(renamedHull.HasValue());
            REQUIRE(renamedHull.Value().sourceDigest == hull.Value().sourceDigest);
            REQUIRE(renamedHull.Value().descriptor.cacheKeyDigest == hull.Value().descriptor.cacheKeyDigest);
            REQUIRE(renamedHull.Value().payload == hull.Value().payload);
            const auto truncatedHull =
                LoadCookedPhysicsConvexHull(hull.Value().descriptor, Target(),
                                            std::span<const std::uint8_t>{hull.Value().payload.data(),
                                                                          generator.Next() % hull.Value().payload.size()});
            REQUIRE(truncatedHull.HasError());
            REQUIRE(truncatedHull.ErrorValue().code.Value() == PhysicsErrors::ShapeArtifactInvalid.code.Value());

            auto invalidCube = cube;
            const auto invalidVertex = static_cast<std::size_t>(generator.Next() % invalidCube.size());
            invalidCube[invalidVertex].x = std::numeric_limits<float>::quiet_NaN();
            const auto badHull = CookPhysicsConvexHull(HullRequest(invalidCube));
            REQUIRE(badHull.HasError());
            REQUIRE(badHull.ErrorValue().code.Value() == PhysicsErrors::ShapeCookSourceInvalid.code.Value());
            REQUIRE(badHull.ErrorValue().message.find("property/hull") != std::string::npos);
            REQUIRE(badHull.ErrorValue().message.find(Asset().ToString()) != std::string::npos);
            REQUIRE(badHull.ErrorValue().message.find("vertex " + std::to_string(invalidVertex)) != std::string::npos);
        }
    }  // namespace

    TEST_CASE("Generated Physics world and body descriptors preserve independent bounds", "[physics][property][headless]") {
        for (const auto seed : Seeds) {
            Generator generator(seed);
            for (std::uint32_t index = 0; index < CasesPerSeed; ++index) {
                INFO("stage=world/body descriptor world=922 shapeSlot=" << index << " seed=" << seed << " case=" << index);
                PhysicsWorldDescriptor world;
                world.gravity = {generator.Bounded(12), -generator.Bounded(12), 0.0F};
                world.fixedDeltaSeconds = 0.001 + static_cast<double>(generator.Next() % 1000U) / 1000.0;
                REQUIRE(ValidatePhysicsWorldDescriptor(world).HasValue());

                auto malformedWorld = world;
                malformedWorld.gravity.x = std::numeric_limits<float>::quiet_NaN();
                const auto worldError = ValidatePhysicsWorldDescriptor(malformedWorld);
                REQUIRE(worldError.HasError());
                REQUIRE(worldError.ErrorValue().code.Value() == PhysicsErrors::DescriptorInvalid.code.Value());
                REQUIRE(ValidatePhysicsWorldDescriptor(world).HasValue());

                auto arbitraryWorld = world;
                arbitraryWorld.gravity.x = std::bit_cast<float>(static_cast<std::uint32_t>(generator.Next()));
                const bool admissibleGravity = std::isfinite(arbitraryWorld.gravity.x) &&
                                               static_cast<double>(arbitraryWorld.gravity.x) * arbitraryWorld.gravity.x +
                                                       static_cast<double>(arbitraryWorld.gravity.y) * arbitraryWorld.gravity.y <=
                                                   MaximumPhysicsGravityMagnitude * MaximumPhysicsGravityMagnitude;
                const auto gravityResult = ValidatePhysicsWorldDescriptor(arbitraryWorld);
                REQUIRE(gravityResult.HasValue() == admissibleGravity);
                if (!admissibleGravity)
                    REQUIRE(gravityResult.ErrorValue().code.Value() == PhysicsErrors::DescriptorInvalid.code.Value());

                PhysicsAuthoredBodyDescriptor body;
                body.motion = PhysicsMotionType::Dynamic;
                body.mass = PhysicsMass{MinimumPhysicsMassKilograms + generator.Bounded(1000)};
                body.initialLinearVelocity = {generator.Bounded(100), generator.Bounded(100), 0.0F};
                REQUIRE(ValidatePhysicsAuthoredBodyDescriptor(body).HasValue());
                const ShapeHandle shape{PhysicsWorldId::Create(922).Value(), {index, 1}};
                const auto resolved = ResolvePhysicsBodyDescriptor(body, shape, {}, shape.world);
                REQUIRE(resolved.HasValue());
                REQUIRE(resolved.Value().linearVelocity == body.initialLinearVelocity);
                REQUIRE(resolved.Value().shape == shape);

                auto malformedBody = body;
                malformedBody.initialLinearVelocity.x = std::numeric_limits<float>::infinity();
                const auto bodyError = ValidatePhysicsAuthoredBodyDescriptor(malformedBody);
                REQUIRE(bodyError.HasError());
                REQUIRE(bodyError.ErrorValue().code.Value() == PhysicsErrors::DescriptorInvalid.code.Value());
                REQUIRE(ValidatePhysicsAuthoredBodyDescriptor(body).HasValue());

                CheckArbitraryMass(generator, body);
            }
        }
    }

    TEST_CASE("Generated convex and mesh sources fail closed on malformed geometry and indices", "[physics][property][fuzz][headless]") {
        const std::array slots{PhysicsMaterialSlotId::FromValue(7)};
        for (const auto seed : Seeds) {
            Generator generator(seed);
            for (std::uint32_t index = 0; index < CasesPerSeed; ++index) {
                INFO("asset=" << Asset().ToString() << " seed=" << seed << " case=" << index);
                CheckConvexCase(generator);

                {
                    INFO("stage=triangle cook/load source=property/mesh subresource=6");
                    const float offset = generator.Bounded(16);
                    const std::array vertices{Math::Vec3{offset, 0, 0}, Math::Vec3{offset + 1, 0, 0}, Math::Vec3{offset + 1, 0, 1},
                                              Math::Vec3{offset, 0, 1}};
                    auto triangles = std::array{PhysicsTriangleMeshSourceTriangle{.vertexIndices = {0, 1, 2},
                                                                                  .subshape = PhysicsShapeSubresourceId::FromValue(11),
                                                                                  .materialSlot = slots[0]},
                                                PhysicsTriangleMeshSourceTriangle{.vertexIndices = {0, 2, 3},
                                                                                  .subshape = PhysicsShapeSubresourceId::FromValue(12),
                                                                                  .materialSlot = slots[0]}};
                    const auto originalTriangles = triangles;
                    const auto mesh = CookPhysicsTriangleMesh(MeshRequest(vertices, triangles, slots));
                    REQUIRE(mesh.HasValue());
                    REQUIRE(triangles == originalTriangles);
                    REQUIRE(LoadCookedPhysicsTriangleMesh(mesh.Value().descriptor, Target(), mesh.Value().payload).HasValue());
                    auto renamedMeshRequest = MeshRequest(vertices, triangles, slots);
                    renamedMeshRequest.sourceContext = "property/renamed-mesh";
                    const auto renamedMesh = CookPhysicsTriangleMesh(renamedMeshRequest);
                    REQUIRE(renamedMesh.HasValue());
                    REQUIRE(renamedMesh.Value().sourceDigest == mesh.Value().sourceDigest);
                    REQUIRE(renamedMesh.Value().descriptor.cacheKeyDigest == mesh.Value().descriptor.cacheKeyDigest);
                    REQUIRE(renamedMesh.Value().payload == mesh.Value().payload);
                    auto corruptedMesh = mesh.Value().payload;
                    corruptedMesh[generator.Next() % corruptedMesh.size()] ^= 1U;
                    const auto badArtifact = LoadCookedPhysicsTriangleMesh(mesh.Value().descriptor, Target(), corruptedMesh);
                    REQUIRE(badArtifact.HasError());
                    REQUIRE(badArtifact.ErrorValue().code.Value() == PhysicsErrors::ShapeArtifactInvalid.code.Value());

                    const auto invalidTriangle = static_cast<std::size_t>(generator.Next() % triangles.size());
                    triangles[invalidTriangle].vertexIndices[generator.Next() % 3U] =
                        static_cast<std::uint32_t>(vertices.size() + generator.Next() % 16U);
                    const auto badMesh = CookPhysicsTriangleMesh(MeshRequest(vertices, triangles, slots));
                    REQUIRE(badMesh.HasError());
                    REQUIRE(badMesh.ErrorValue().code.Value() == PhysicsErrors::ShapeCookSourceInvalid.code.Value());
                    REQUIRE(badMesh.ErrorValue().message.find("property/mesh") != std::string::npos);
                    REQUIRE(badMesh.ErrorValue().message.find(Asset().ToString()) != std::string::npos);
                    REQUIRE(badMesh.ErrorValue().message.find("triangle " + std::to_string(triangles[invalidTriangle].subshape.Value())) !=
                            std::string::npos);
                }
            }
        }
    }

#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Generated world lifecycles reject foreign-thread and retired commands", "[physics][property][lifecycle][native]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        for (const auto seed : Seeds) {
            Generator generator(seed);
            for (std::uint32_t index = 0; index < 8; ++index) {
                auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
                const std::uint64_t generation = 10'000 + seed % 1000U * 100U + index * 2U;
                const std::uint64_t body = 1 + generator.Next() % 1'000U;
                const std::uint64_t sequence = 1 + generator.Next() % 1'000U;
                INFO("world=" << generation << " body=" << body << " sequence=" << sequence << " seed=" << seed << " case=" << index
                              << " stage=command lifecycle");
                REQUIRE(world->Activate(PhysicsWorldId::Create(generation).Value()).HasValue());
                const auto command = Command(generation, body, sequence);
                auto malformed = command;
                malformed.order.sourceSequence = 0;
                const auto malformedResult = world->QueueStructuralCommand(malformed);
                REQUIRE(malformedResult.HasError());
                REQUIRE(malformedResult.ErrorValue().code.Value() == PhysicsErrors::CommandOrderInvalid.code.Value());
                REQUIRE(world->TickStatistics().pendingCommands == 0);
                std::string workerErrorCode;
                std::thread worker([&] {
                    const auto result = world->QueueStructuralCommand(command);
                    if (result.HasError())
                        workerErrorCode = result.ErrorValue().code.Value();
                });
                worker.join();
                REQUIRE(workerErrorCode == PhysicsErrors::ThreadAffinityViolation.code.Value());
                REQUIRE(world->TickStatistics().pendingCommands == 0);
                REQUIRE(world->QueueStructuralCommand(command).HasValue());
                REQUIRE(world->TickStatistics().pendingCommands == 1);
                REQUIRE(world->Reset().HasValue());
                REQUIRE(world->TickStatistics().pendingCommands == 0);
                REQUIRE_FALSE(world->Identity().IsValid());
                const auto newGeneration = generation + 1;
                REQUIRE(world->Activate(PhysicsWorldId::Create(newGeneration).Value()).HasValue());
                const auto stale = world->QueueStructuralCommand(command);
                REQUIRE(stale.HasError());
                REQUIRE(stale.ErrorValue().code.Value() == PhysicsErrors::CommandOrderInvalid.code.Value());
                REQUIRE(world->UnloadScene().HasValue());
                REQUIRE(world->UnloadScene().HasValue());
                const auto retired = world->QueueStructuralCommand(Command(newGeneration, body, sequence));
                REQUIRE(retired.HasError());
                REQUIRE(retired.ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value());
            }
        }
    }
#endif
}  // namespace Horo::Physics
