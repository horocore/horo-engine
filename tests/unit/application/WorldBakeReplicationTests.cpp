#include "../runtime/networking/ReplicationDescriptorTestSupport.h"
#include "WorldBakeApplicationTestSupport.h"

#include <array>

namespace Horo::Application {
    namespace {
        namespace N = Network;
        namespace R = N::TestSupport;
        namespace T = WorldBakeTestSupport;

        /** @brief Authors exact complete requirements and actual observed installed schemas independently. */
        N::ReplicationMetadataCapturePtr Capture(const bool installed, const bool optional = false) {
            const std::array schemas{R::Schema()};
            const std::array<ModuleId, 1> modules{{{"game.replication"}}};
            N::NetworkReplicationInventory inventory{N::NetworkReplicationInventoryCompleteness::Complete,
                                                     {{R::Schema(), optional ? N::ReplicationDeclarationRequirement::Optional
                                                                             : N::ReplicationDeclarationRequirement::Required}}};
            const auto capture =
                N::ReplicationMetadataCapture::Build(inventory,
                                                     installed ? std::span<const N::ReplicationSchemaDescriptor>{schemas}
                                                               : std::span<const N::ReplicationSchemaDescriptor>{},
                                                     installed ? std::span<const ModuleId>{modules} : std::span<const ModuleId>{});
            REQUIRE(capture.HasValue());
            return capture.Value();
        }

        void Bind(WorldBakeRequest &request, N::ReplicationMetadataCapturePtr capture) {
            request.project.replication = std::move(capture);
            request.project.fence.replication = request.project.replication->Fingerprint();
        }
    }  // namespace

    TEST_CASE("World bake admits canonical complete replication and explicit optional omission", "[world_bake][replication]") {
        Editor::PartitionedWorldWorkspace workspace{T::T::Asset(9)};
        auto request = T::Request(workspace);
        Bind(request, Capture(true));
        const auto cooked = CookWorldPages(request);
        REQUIRE(cooked.HasValue());
        CHECK(cooked.Value().project.replication == request.project.replication->Fingerprint());
        Bind(request, Capture(false, true));
        CHECK(CookWorldPages(request).HasValue());
    }

    TEST_CASE("World bake denies unknown skewed and unfenced metadata before publication", "[world_bake][replication]") {
        Editor::PartitionedWorldWorkspace workspace{T::T::Asset(9)};
        auto request = T::Request(workspace);
        N::NetworkReplicationInventory unknown;
        const auto legacy = N::ReplicationMetadataCapture::Build(unknown, {}, {});
        REQUIRE(legacy.HasValue());
        Bind(request, legacy.Value());
        CHECK(CookWorldPages(request).HasError());
        Bind(request, Capture(false));
        const auto missing = CookWorldPages(request);
        REQUIRE(missing.HasError());
        CHECK(missing.ErrorValue().domain.Value() == N::NetworkErrors::ReplicationDescriptorIncompatible.domain.Value());
        CHECK(missing.ErrorValue().code.Value() == N::NetworkErrors::ReplicationDescriptorIncompatible.code.Value());
        REQUIRE(missing.ErrorValue().diagnostics.size() == 1);
        Bind(request, Capture(true));
        request.project.fence.replication = {};
        CHECK(CookWorldPages(request).ErrorValue().code.Value() == WorldBakeErrors::Stale.code.Value());
    }

    TEST_CASE("World bake failed replication replacement retains published output and fences pending generation",
              "[world_bake][replication]") {
        Editor::PartitionedWorldWorkspace workspace{T::T::Asset(9)};
        auto request = T::Request(workspace);
        Bind(request, Capture(true));
        JobSystem jobs{{.workerCount = 1}};
        OperationStore operations{8, 16};
        BuildOutputStore output{32};
        WorldBakeApplication host{jobs, operations, output};
        REQUIRE(host.Submit(request).HasValue());
        T::Drain(jobs);
        host.Pump(request.document, request.project.fence);
        const auto published = host.Published();
        REQUIRE(published);
        auto missing = request;
        Bind(missing, Capture(false));
        CHECK(host.Submit(missing).HasError());
        CHECK(host.Published() == published);
        T::Gate gate{jobs};
        REQUIRE(host.Submit(request).HasValue());
        auto changed = request.project.fence;
        changed.replication = Capture(false, true)->Fingerprint();
        gate.Release();
        T::Drain(jobs);
        host.Pump(request.document, changed);
        CHECK(host.Published() == published);
        CHECK_FALSE(host.ActiveOperation());
    }
}  // namespace Horo::Application
