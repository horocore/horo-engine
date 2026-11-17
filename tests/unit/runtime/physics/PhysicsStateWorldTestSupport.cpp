#include "PhysicsStateWorldTestSupport.h"

#include "Horo/Foundation/Sha256.h"
#include "Horo/Physics/PhysicsCollisionSchema.h"
#include "HoroPhysicsStateBuildFacts.h"
#include "PhysicsStateImagesInternal.h"
#include "ProcessImageIdentity.h"

#include <exception>
#include <tuple>

namespace Horo::Physics::StateTest {
    namespace {
        template <class T> T Id(std::uint8_t value) noexcept {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = value;
            return T::FromBytes(bytes);
        }

        std::shared_ptr<const NormalizedCollisionSchema> Schema() {
            ProjectCollisionSchema authored;
            const auto layer = Id<CollisionLayerId>(1);
            const auto profile = Id<CollisionProfileId>(2);
            const auto channel = Id<PhysicsQueryChannelId>(3);
            authored.defaultProfile = profile;
            authored.layers.push_back({layer});
            authored.pairs.push_back({layer, layer, SimulationPairResponse::Block});
            authored.queryChannels.push_back({channel});
            authored.profiles.push_back({profile, layer, true, true, true, {{channel, CollisionQueryResponse::Block}}});
            return std::make_shared<const NormalizedCollisionSchema>(Take(NormalizedCollisionSchema::Create(authored)));
        }

        BodyHandle Body(PhysicsWorld &world, ShapeHandle shape, Math::Vec3 position, bool dynamic,
                        const PhysicsSurfaceWorldBinding &surfaces) {
            PhysicsBodyDescriptor descriptor;
            descriptor.shape = shape;
            descriptor.pose.translation = position;
            descriptor.motion = dynamic ? PhysicsMotionType::Dynamic : PhysicsMotionType::Static;
            descriptor.mass = dynamic ? PhysicsMassPolicy{PhysicsMass{1}} : PhysicsMassPolicy{PhysicsNoMass{}};
            PhysicsSceneCollisionBinding collision{Id<CollisionProfileId>(2), {}};
            if (surfaces.catalog && surfaces.defaultSurface) {
                auto evidence = MakePhysicsSurfaceEvidence(surfaces, *surfaces.defaultSurface);
                evidence.collider = 1;
                PhysicsSceneColliderBinding collider{.materials = {{Id<decltype(PhysicsQueryMaterial{}.asset)>(4), 11,
                                                                    PhysicsMaterialSlotId::FromValue(1)}},
                                                     .surface = evidence,
                                                     .collider = 1};
                collision.colliders.push_back(std::move(collider));
            }
            return Take(world.CreateSceneBody({.body = descriptor, .collision = std::move(collision)}));
        }
    }  // namespace

    /** @copydoc ActualStaticBinding */
    PhysicsStateBuildBinding ActualStaticBinding() {
        auto image = Take(Platform::CaptureCurrentProcessImageIdentity());
        Check(image.Verify());
        const auto addresses = Detail::ReadPhysicsStateImageAddresses();
        Check(image.VerifyStaticCodeAddress(addresses.adapter));
        Check(image.VerifyStaticCodeAddress(addresses.solver));
        return {image.Artifact(), image.Artifact(), Detail::StateBuildFacts::PlatformAbi, PhysicsStateCodeLifetime::ProcessStatic,
                image.Lease()};
    }

    PhysicsStateRuntimeBudget StateRuntimeBudget(std::uint32_t owners, std::size_t bytes, PhysicsStateConstructionTracking tracking) {
        return {.maximumCaptureOwners = owners,
                .maximumRetainedBytes = bytes,
                .construction = {128, 512, 128, 64, 2U * 1024U * 1024U},
                .constructionTracking = tracking};
    }

    WorldFixture::WorldFixture(const PhysicsWorldSettings &settings, PhysicsStateConstructionTracking tracking,
                               PhysicsSurfaceWorldBinding surfaceBinding, std::uint64_t filterGeneration)
        : content(std::make_shared<const std::string>(
              "PhysicsStateWorldFixture.v1;floor=box(4,.5,4);"
              "bodies=box(.5,.5,.5),mass1,positions(-1.5,1,0),(1.5,1,0);stableIDs=900,700,100;schema=layer1/profile2/channel3")),
          schema(Schema()), surfaces(std::move(surfaceBinding)) {
        runtime = Take(PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical));
        Check(runtime->BindStateSnapshotBuild(ActualStaticBinding(), StateRuntimeBudget(8, 256U * 1024U * 1024U, tracking)));
        world = Take(runtime->PrepareWorld(settings, {schema, filterGeneration, surfaces}));
        Check(world->Activate(Take(PhysicsWorldId::Create(70))));
        Build();
        Advance(5);
    }

    WorldLayout PopulateStateWorld(PhysicsWorld &world, const PhysicsSurfaceWorldBinding &surfaces) {
        const auto floor = Take(world.CreateSceneShape(PhysicsBoxShape{{4, .5F, 4}}));
        const auto box = Take(world.CreateSceneShape(PhysicsBoxShape{{.5F, .5F, .5F}}));
        WorldLayout layout;
        layout.bodies = {{{900, Body(world, floor, {}, false, surfaces)},
                          {700, Body(world, box, {-1.5F, 1, 0}, true, surfaces)},
                          {100, Body(world, box, {1.5F, 1, 0}, true, surfaces)}}};
        layout.shapes = {{{300, floor}, {200, box}}};
        return layout;
    }

    void WorldFixture::Build() {
        auto layout = PopulateStateWorld(*world, surfaces);
        bodies = layout.bodies;
        shapes = layout.shapes;
    }

    void WorldFixture::Advance(std::uint32_t count) {
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto next = world->PublishedTick().completedTick + 1;
            Check(world->AdvanceFixedTick(
                {.simulationTick = next, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)}));
        }
    }

    PhysicsStateCaptureConfiguration WorldFixture::Configuration(std::uint32_t slots) const {
        const auto bytes = std::as_bytes(std::span{content->data(), content->size()});
        return {slots, 64U * 1024U * 1024U, ComputeSha256(bytes), bodies, shapes, {}, content};
    }

    std::size_t WorldFixture::Configure(std::uint32_t slots) {
        configuredBytes = Take(world->ConfigureStateSnapshots(Configuration(slots)));
        return configuredBytes;
    }

    PhysicsStateSnapshotLease WorldFixture::Capture() {
        return Take(world->CaptureState());
    }

    void WorldFixture::Shutdown() noexcept {
        if (!runtime)
            return;
        world.reset();
        auto retirement = runtime->BeginRetirement();
        if (retirement.HasError())
            std::terminate();
        auto owner = std::move(retirement).Value();
        runtime.reset();
        for (std::uint32_t scan = 0; scan < 8; ++scan) {
            auto progress = owner.Collect(64);
            if (progress.HasError())
                std::terminate();
            if (progress.Value().phase == PhysicsRuntimeRetirementPhase::Drained)
                return;
        }
        std::terminate();
    }

    WorldFixture::~WorldFixture() {
        Shutdown();
    }

    Transcript ReadTranscript(PhysicsWorld &world) {
        Transcript transcript;
        auto access = Take(world.IssueQueryEventCapability());
        const auto publication = world.PublishedTick();
        const auto read = Take(access.ReadEvents({access.Identity(), publication.eventTick, publication.publicationRevision,
                                                  static_cast<std::uint32_t>(transcript.events.size())},
                                                 transcript.events));
        REQUIRE_FALSE(read.truncated);
        transcript.count = read.recordCount;
        return transcript;
    }

    bool SamePublication(const PhysicsPublishedTick &one, const PhysicsPublishedTick &two) noexcept {
        return std::tie(one.completedTick, one.publicationRevision, one.transformTick, one.queryTick, one.eventTick, one.appliedCommands,
                        one.eventCount, one.droppedEventCount) == std::tie(two.completedTick, two.publicationRevision, two.transformTick,
                                                                           two.queryTick, two.eventTick, two.appliedCommands,
                                                                           two.eventCount, two.droppedEventCount);
    }
}  // namespace Horo::Physics::StateTest
