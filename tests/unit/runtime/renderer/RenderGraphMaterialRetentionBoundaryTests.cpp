#include "MaterialBindingTestFixture.h"
#include "RenderFrontendErrors.h"
#include "RenderGraphResourceLeasePool.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::Detail;

    /** @brief Supplies material metadata ownership only; never shader compilation, native pipeline validity or pixels. */
    class MetadataBackend final : public IMaterialBindingBackend {
        class Lease final : public IResidentMaterialBinding {};

    public:
        /** @brief Adopts a synthetic material descriptor for this isolated registry/pool ownership unit. */
        Result<std::unique_ptr<IResidentMaterialBinding>> Realize(const MaterialBindingDescriptor &) override {
            return Result<std::unique_ptr<IResidentMaterialBinding>>::Success(std::make_unique<Lease>());
        }
    };

    /** @brief Acquires an actual globally unique resource-owner incarnation, never a made-up slot/generation. */
    RenderResourceOwnerId Owner() {
        const auto owner = AcquireRenderResourceOwnerId();
        REQUIRE(owner.HasValue());
        return owner.Value();
    }

    /** @brief Exercises actual private registry/pool issuance and accounting, without graph/native qualification. */
    struct Fixture final {
        RenderResourceOwnerId owner = Owner();
        std::size_t released{};
        RenderResourceRegistry registry{owner,
                                        {.maximumSlots = 16,
                                         .maximumPendingRequests = 16,
                                         .retirementDrainBudget = 16,
                                         .maximumOperationResults = 16,
                                         .maximumSubmissionPins = 4096},
                                        [this](RenderResourceClass, std::uint64_t, std::optional<RenderMemoryAllocationId>,
                                               BackendResourceReleaseMode) noexcept {
            ++released;
        }};
        RenderGraphResourceLeasePool pool{registry, 4096};
        MetadataBackend backend;
        std::unique_ptr<MaterialBindingTable> table;
        std::shared_ptr<const ResidentMaterialBinding> material;
        RenderResourceIdentity meshIdentity, viewIdentity;
        RenderMeshHandle mesh;
        RenderTextureViewHandle view;
        std::vector<RenderGraphMaterialDraw> draws;
        std::vector<RenderGraphPassWorkload> workloads;
        std::array<IRenderGraphResourceLease *, 8> active{};
        std::uint64_t nextInstance{};

        /** @brief Issues every used handle through the actual registry and publishes only explicitly synthetic metadata instances. */
        Fixture() {
            const auto vertex = Publish(RenderResourceClass::Buffer);
            const auto index = Publish(RenderResourceClass::Buffer);
            const std::array meshDependencies{vertex, index};
            meshIdentity = Publish(RenderResourceClass::Mesh, meshDependencies);
            mesh = {meshIdentity.owner, meshIdentity.slot, meshIdentity.generation};
            const auto texture = Publish(RenderResourceClass::Texture);
            const std::array viewDependencies{texture};
            viewIdentity = Publish(RenderResourceClass::TextureView, viewDependencies);
            view = {viewIdentity.owner, viewIdentity.slot, viewIdentity.generation};
            const auto pipeline = Publish(RenderResourceClass::Pipeline);
            auto descriptor = Tests::MaterialBindingFixture::Descriptor();
            descriptor.pipeline = {pipeline.owner, pipeline.slot, pipeline.generation};
            auto created = MaterialBindingTable::Create(owner, backend);
            REQUIRE(created.HasValue());
            table = std::move(created).Value();
            const auto selected = table->Publish(std::move(descriptor));
            REQUIRE(selected.HasValue());
            const auto acquired = table->Acquire(selected.Value().binding);
            REQUIRE(acquired.HasValue());
            material = acquired.Value();
        }

        /** @brief Releases every accepted pool transaction before fixture-owned registry and material owners are destroyed. */
        ~Fixture() {
            for (auto *lease : active)
                if (lease)
                    lease->Release();
        }

        /** @brief Reserves and publishes one actual typed registry identity; ordinal backend numbers are metadata, not native handles. */
        RenderResourceIdentity Publish(RenderResourceClass type, std::span<const RenderResourceIdentity> dependencies = {}) {
            const auto reserved = registry.Reserve(type, dependencies);
            REQUIRE(reserved.HasValue());
            REQUIRE(registry.Publish(type, reserved.Value().identity, ++nextInstance).HasValue());
            return reserved.Value().identity;
        }

        /** @brief Partitions an aggregate into independently bounded raster batches without inventing a compiled graph. */
        void SetDraws(std::size_t count) {
            draws.assign(count, RenderGraphMaterialDraw{.mesh = mesh, .material = material});
            workloads.clear();
            for (std::size_t offset = 0; offset < count; offset += RenderGraphMaterialRaster::MaximumDraws) {
                const auto size = std::min(count - offset, RenderGraphMaterialRaster::MaximumDraws);
                workloads.push_back(
                    {.workload =
                         RenderGraphMaterialRaster{.colorView = view,
                                                   .draws = std::span<const RenderGraphMaterialDraw>{draws}.subspan(offset, size)}});
            }
        }

        /** @brief Calls the actual aggregate lease transaction, intentionally before any graph/native validator. */
        Result<IRenderGraphResourceLease *> Acquire() {
            return pool.Acquire({}, nullptr, nullptr, workloads);
        }

        /** @brief Tracks one actual accepted transaction so failure cleanup cannot leave an occupied lease. */
        void Retain(std::size_t index) {
            const auto result = Acquire();
            REQUIRE(result.HasValue());
            active[index] = result.Value();
        }

        /** @brief Releases the exact accepted transaction once. */
        void Release(std::size_t index) {
            active[index]->Release();
            active[index] = nullptr;
        }

        /** @brief Marks real registry generations retiring; actual submission pins must decide when they drain. */
        void RetireGeometry() {
            REQUIRE(registry.Release(RenderResourceClass::Mesh, meshIdentity).HasValue());
            REQUIRE(registry.Release(RenderResourceClass::TextureView, viewIdentity).HasValue());
        }
    };
}  // namespace

TEST_CASE("Material retention admits the actual901draw aggregate and exact1024edge through its owning pool",
          "[renderer][material-retention][metadata-only]") {
    const auto count = GENERATE(std::size_t{901}, std::size_t{1024});
    Fixture fixture;
    fixture.SetDraws(count);
    const auto references = fixture.material.use_count();
    fixture.Retain(0);
    CHECK(fixture.material.use_count() == references + static_cast<long>(count));
    for (const auto &entry : fixture.workloads)
        CHECK(std::get<RenderGraphMaterialRaster>(entry.workload).draws.size() <= 128);
    fixture.RetireGeometry();
    CHECK(fixture.registry.DrainRetirements() == 0);
    CHECK(fixture.released == 0);
    fixture.Release(0);
    CHECK(fixture.material.use_count() == references);
    CHECK(fixture.registry.DrainRetirements() == 2);
    CHECK(fixture.released == 2);
}

TEST_CASE("Material retention rejects1025 with typed capacity and rolls back every material pin and occupied slot",
          "[renderer][material-retention][metadata-only]") {
    Fixture fixture;
    fixture.SetDraws(1025);
    const auto references = fixture.material.use_count();
    const auto denied = fixture.Acquire();
    Tests::RequireActionableError(denied, FrontendErrors::ResourceCapacityExhausted);
    CHECK(fixture.material.use_count() == references);
    fixture.SetDraws(1024);
    const auto retryReferences = fixture.material.use_count();
    for (std::size_t index = 0; index < fixture.active.size(); ++index)
        fixture.Retain(index);
    CHECK(fixture.material.use_count() == retryReferences + 8 * 1024);
    Tests::RequireActionableError(fixture.Acquire(), FrontendErrors::ResourceCapacityExhausted);
    for (std::size_t index = 0; index < fixture.active.size(); ++index)
        fixture.Release(index);
    CHECK(fixture.material.use_count() == retryReferences);
    fixture.RetireGeometry();
    CHECK(fixture.registry.DrainRetirements() == 2);
    CHECK(fixture.released == 2);
}
