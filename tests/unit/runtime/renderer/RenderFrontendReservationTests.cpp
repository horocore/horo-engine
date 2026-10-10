#include "RenderTransientGraphTestSupport.h"
#include "RenderTransientTestBackend.h"

#include <array>
#include <cstddef>
#include <new>
#include <optional>

namespace Horo::Render::FrontendReservationTest {
    using namespace TransientTest;

    /** @brief Exercises every bounded admission-allocation position through the public frontend with a real Null provider. */
    template <typename Create, typename Release>
    void CheckAdmissionRollback(const std::size_t position, const Create &create, const Release &release) {
        auto audit = std::make_shared<Audit>();
        auto frontend = MakeFrontend(audit, {}, {},
                                     {.maximumPendingBytes = 64,
                                      .maximumBytesPerDrain = 64,
                                      .maximumRequestsPerDrain = 1,
                                      .maximumPendingRequests = 1});
        std::optional<decltype(create(*frontend))> first;
        bool allocationFailed = false;
        {
            Tests::AllocationProbe::ScopedFailure failure{position};
            try {
                first.emplace(create(*frontend));
            } catch (const std::bad_alloc &) {
                allocationFailed = true;
            }
        }
        REQUIRE((allocationFailed || first.has_value()));
        if (first.has_value() && first->HasValue())
            REQUIRE(release(*frontend, first->Value().handle).HasValue());
        CHECK(frontend->MemorySnapshot().reservationCount == 0);
        CHECK(frontend->UploadSnapshot().pendingRequests == 0);
        const auto retried = create(*frontend);
        REQUIRE(retried.HasValue());
        CHECK(retried.Value().handle.slot == 1);
        REQUIRE(release(*frontend, retried.Value().handle).HasValue());
        CHECK(frontend->MemorySnapshot().reservationCount == 0);
        CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 0);
        CHECK(audit->resources.creates == 0);
    }

    /** @brief Owns ready native dependencies while a view or target remains a provisional metadata request. */
    struct MetadataParents {
        RenderTextureHandle texture;
        RenderTextureViewHandle view;
    };

    /** @brief Prepares real dependencies outside the injected allocation window. */
    [[nodiscard]] MetadataParents PrepareMetadataParents(RenderFrontend &frontend, const bool target) {
        const auto texture =
            frontend.CreateTexture({.extent = {1, 1}, .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment});
        REQUIRE(texture.HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        MetadataParents parents{.texture = texture.Value().handle};
        if (target) {
            const auto view = frontend.CreateTextureView({.texture = parents.texture});
            REQUIRE(view.HasValue());
            REQUIRE(frontend.ProcessResourceRequests().HasValue());
            parents.view = view.Value().handle;
        }
        return parents;
    }

    /** @brief Proves failed metadata growth cancels the child generation and restores real native dependency pins. */
    template <typename Create, typename Release>
    void CheckMetadataRollback(const std::size_t position, const bool target, const Create &create, const Release &release) {
        auto audit = std::make_shared<Audit>();
        auto frontend = MakeFrontend(audit, {}, {},
                                     {.maximumPendingBytes = 64,
                                      .maximumBytesPerDrain = 64,
                                      .maximumRequestsPerDrain = 1,
                                      .maximumPendingRequests = 1});
        const auto parents = PrepareMetadataParents(*frontend, target);
        std::optional<decltype(create(*frontend, parents))> first;
        bool allocationFailed = false;
        {
            Tests::AllocationProbe::ScopedFailure failure{position};
            try {
                first.emplace(create(*frontend, parents));
            } catch (const std::bad_alloc &) {
                allocationFailed = true;
            }
        }
        REQUIRE((allocationFailed || first.has_value()));
        if (first.has_value() && first->HasValue())
            REQUIRE(release(*frontend, first->Value().handle).HasValue());
        CHECK(frontend->UploadSnapshot().pendingRequests == 0);
        const auto retried = create(*frontend, parents);
        REQUIRE(retried.HasValue());
        CHECK(retried.Value().handle.slot == (target ? 3U : 2U));
        REQUIRE(release(*frontend, retried.Value().handle).HasValue());
        if (parents.view.IsValid())
            REQUIRE(frontend->ReleaseTextureView(parents.view).HasValue());
        REQUIRE(frontend->ReleaseTexture(parents.texture).HasValue());
        CHECK(audit->resources.creates == 1);
        CHECK(audit->resources.destroyed == 1);
        CHECK(frontend->MemorySnapshot().allocationCount == 0);
        CHECK(frontend->MemorySnapshot().reservationCount == 0);
    }

    TEST_CASE("Frontend dependency metadata allocation failures preserve reusable view and target generations",
              "[renderer][resource][rollback]") {
        SECTION("texture views") {
            for (std::size_t position = 0; position < 32; ++position)
                CheckMetadataRollback(position, false, [](RenderFrontend &frontend, const MetadataParents &parents) {
                    return frontend.CreateTextureView({.texture = parents.texture});
                }, [](RenderFrontend &frontend, const RenderTextureViewHandle handle) {
                    return frontend.ReleaseTextureView(handle);
                });
        }
        SECTION("render targets") {
            for (std::size_t position = 0; position < 32; ++position)
                CheckMetadataRollback(position, true, [](RenderFrontend &frontend, const MetadataParents &parents) {
                    return frontend.CreateRenderTarget({.colorAttachment = parents.view, .extent = {1, 1}});
                }, [](RenderFrontend &frontend, const RenderTargetHandle handle) {
                    return frontend.ReleaseRenderTarget(handle);
                });
        }
    }

    TEST_CASE("Frontend resource admission allocation failures preserve the first reusable generation", "[renderer][resource][rollback]") {
        SECTION("buffers") {
            for (std::size_t position = 0; position < 32; ++position)
                CheckAdmissionRollback(position, [](RenderFrontend &frontend) {
                    const std::array<std::byte, 16> bytes{};
                    return frontend.CreateBuffer(CopyBuffer(bytes.size()), bytes);
                }, [](RenderFrontend &frontend, const RenderBufferHandle handle) {
                    return frontend.ReleaseBuffer(handle);
                });
        }
        SECTION("textures") {
            for (std::size_t position = 0; position < 32; ++position)
                CheckAdmissionRollback(position, [](RenderFrontend &frontend) {
                    return frontend.CreateTexture({.extent = {1, 1}, .usage = RenderTextureUsage::Sampled});
                }, [](RenderFrontend &frontend, const RenderTextureHandle handle) {
                    return frontend.ReleaseTexture(handle);
                });
        }
    }
}  // namespace Horo::Render::FrontendReservationTest
