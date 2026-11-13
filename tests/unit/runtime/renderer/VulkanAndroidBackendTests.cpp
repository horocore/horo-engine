#include "Horo/Runtime/Render/Internal/VulkanAndroidBackendModule.h"
#include "VulkanAndroidProtocolFixture.h"

#include <string>
#include <thread>

namespace Horo::Render::AndroidVulkan::Testing {
    namespace {
        using namespace Platform::Android;

        /** @brief Fake native-window ledger, independent of actual lifecycle identity issuance. */
        struct NativeWindow final {
            AndroidWindowExtent extent{640, 480};
            std::size_t references{};
        };

        /** @brief Retains only the fake platform native reference. */
        void AcquireWindow(void *, void *pointer) noexcept {
            ++static_cast<NativeWindow *>(pointer)->references;
        }

        /** @brief Releases only the fake platform native reference. */
        void ReleaseWindow(void *, void *pointer) noexcept {
            --static_cast<NativeWindow *>(pointer)->references;
        }

        /** @brief Returns explicit fake native pixel facts. */
        AndroidWindowExtent WindowExtent(void *, void *pointer) noexcept {
            return static_cast<NativeWindow *>(pointer)->extent;
        }

        /** @brief Uses genuine lifecycle/window owners with fake native dispatch; no Android or GPU result is implied. */
        struct Fixture final {
            Driver driver;
            std::unique_ptr<Platform::AndroidLifecycleController> lifecycle;
            AndroidLifecycleAdapter adapter;
            Platform::AndroidActivityGeneration activity;
            std::array<NativeWindow, 2> native;
            std::unique_ptr<AndroidWindowHost> windows;
            std::unique_ptr<Runtime> runtime;

            /** @brief Composes actual owner-lane lifecycle authorities around explicit fake native operations. */
            Fixture() : lifecycle(Value(Platform::AndroidLifecycleController::Create(std::this_thread::get_id()))), adapter(*lifecycle) {
                Accepted(adapter.OnProcessCreated());
                Value(lifecycle->Drain());
                activity = Value(adapter.OnActivityCreated());
                Value(lifecycle->Drain());
                Accepted(adapter.OnActivityStarted(activity));
                Value(lifecycle->Drain());
                Accepted(adapter.OnActivityResumed(activity));
                Value(lifecycle->Drain());
                windows = Value(AndroidWindowHost::Create(*lifecycle, adapter, AndroidWindowMode::Interactive,
                                                          {nullptr, AcquireWindow, ReleaseWindow, WindowExtent}));
                runtime = std::make_unique<Runtime>(*windows, Driver::Resolve);
            }

            /** @brief Makes fake completion explicit before the real shutdown/drain owner destroys native parents. */
            ~Fixture() {
                driver.acquireReady = true;
                driver.renderReady = true;
                driver.presentReady = true;
                if (driver.deviceCreates) {
                    Accepted(runtime->Close());
                    CHECK(Value(runtime->Drain()) == RenderBackendDrainProgress::Drained);
                }
                runtime.reset();
                CHECK(Value(windows->Close()));
            }

            /** @brief Activates the genuine backend before any window admission. */
            void Initialize() {
                Accepted(runtime->Initialize({.requirePresentation = true, .maxFramesInFlight = 2, .presentMode = PresentMode::Fifo}));
            }

            /** @brief Commits real lifecycle observations before refreshing derived platform facts. */
            void Commit() {
                Value(lifecycle->Drain());
                Accepted(windows->Refresh());
            }

            /** @brief Publishes one authentic window generation from an explicit fake native occurrence. */
            Platform::AndroidWindowGeneration Available(std::size_t index = 0) {
                auto generation = Value(windows->ObserveAvailable(activity, &native[index]));
                Commit();
                return generation;
            }

            /** @brief Encodes and submits only the actual supported clear/render-pass path. */
            FrameToken Draw() {
                const auto frame = Value(runtime->Begin({1, {640, 480}}));
                const std::array passes{
                    RenderPassDescriptor{.id = {1}, .kind = RenderPassKind::Graphics, .primaryOutput = PrimaryOutputAttachment{}}};
                Accepted(runtime->Execute({frame, passes}));
                Accepted(runtime->Present(frame));
                return frame;
            }
        };
    }  // namespace

    TEST_CASE("Android Vulkan provider registration and device activation precede all native window access",
              "[renderer][vulkan-android][protocol-fake]") {
        Fixture f;
        RenderBackendRegistry registry;
        Accepted(RegisterVulkanAndroidRenderBackend(registry, *f.windows));
        Accepted(registry.Seal());
        auto inert = Value(registry.Create(RenderBackendId{"vulkan-android"}));
        CHECK(f.driver.instanceCreates == 0);
        CHECK(f.driver.surfaceCreates == 0);
        f.Initialize();
        CHECK(f.driver.requested11);
        CHECK(f.driver.enabledMaintenance);
        CHECK(f.driver.deviceCreates == 1);
        CHECK(f.driver.surfaceCreates == 0);
        CHECK_FALSE(Value(f.runtime->Availability({640, 480})));
        CHECK(f.native[0].references == 0);
        f.Available();
        CHECK(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.acquisitions == 0);
        f.Draw();
        CHECK(f.driver.passes == 1);
        CHECK(f.driver.submits == 1);
        CHECK(f.driver.presents == 1);
    }

    TEST_CASE("Android Vulkan requires actual maintenance support without acquiring a window or surface",
              "[renderer][vulkan-android][protocol-fake]") {
        Fixture f;
        f.driver.maintenance = false;
        const auto initialized = f.runtime->Initialize({.requirePresentation = true});
        REQUIRE(initialized.HasError());
        CHECK(initialized.ErrorValue().code.Value() == Errors::Unsupported.code.Value());
        CHECK(f.driver.deviceCreates == 0);
        CHECK(f.driver.surfaceCreates == 0);
        CHECK(f.native[0].references == 0);
    }

    TEST_CASE("Android Vulkan explicit headless denial never opens its native loader or device",
              "[renderer][vulkan-android][protocol-fake]") {
        Fixture f;
        const auto initialized = f.runtime->Initialize({.requirePresentation = false});
        REQUIRE(initialized.HasError());
        CHECK(initialized.ErrorValue().code.Value() == Errors::Unsupported.code.Value());
        CHECK(f.driver.instanceCreates == 0);
        CHECK(f.driver.deviceCreates == 0);
        CHECK(f.driver.surfaceCreates == 0);
    }

    TEST_CASE("Android Vulkan rejects foreign owner lanes before native or window access", "[renderer][vulkan-android][protocol-fake]") {
        Fixture f;
        f.Initialize();
        std::optional<Error> failure;
        std::thread foreign([&] {
            const auto result = f.runtime->Availability({640, 480});
            if (result.HasError())
                failure = result.ErrorValue();
        });
        foreign.join();
        REQUIRE(failure);
        CHECK(failure->code.Value() == Errors::WrongThread.code.Value());
        CHECK(f.driver.surfaceCreates == 0);
        CHECK(f.driver.acquisitions == 0);
    }

    TEST_CASE("Android Vulkan validates a new surface against the already selected graphics queue",
              "[renderer][vulkan-android][protocol-fake]") {
        Fixture f;
        f.Initialize();
        f.Available();
        f.driver.surfaceSupported = false;
        const auto ready = f.runtime->Availability({640, 480});
        REQUIRE(ready.HasError());
        CHECK(ready.ErrorValue().code.Value() == Errors::Unsupported.code.Value());
        CHECK(f.driver.deviceCreates == 1);
        CHECK(f.driver.surfaceCreates == 1);
        CHECK(f.driver.surfaceDestroys == 1);
        CHECK(f.driver.submits == 0);
    }

    TEST_CASE("Android Vulkan prefers reported opaque composition and preserves its finite clear alpha contract",
              "[renderer][vulkan-android][protocol-fake][alpha]") {
        Fixture f;
        SECTION("opaque only") {}
        SECTION("both opaque and inherit") {
            f.driver.supportedAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        }
        f.Initialize();
        f.Available();
        CHECK(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.chosenAlpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
        const auto frame = Value(f.runtime->Begin({1, {640, 480}}));
        PrimaryOutputAttachment attachment;
        attachment.clearColor.alpha = 0.25F;
        const std::array passes{RenderPassDescriptor{.id = {1}, .primaryOutput = attachment}};
        Accepted(f.runtime->Execute({frame, passes}));
        CHECK(f.driver.commandBegins == 1);
        CHECK(f.driver.recordedClearAlpha == 0.25F);
        Accepted(f.runtime->Present(frame));
    }

    TEST_CASE("Android Vulkan admits reported inherit as native window default with unchanged unit alpha",
              "[renderer][vulkan-android][protocol-fake][alpha]") {
        Fixture f;
        f.driver.supportedAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        f.Initialize();
        f.Available();
        CHECK(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.chainCreates == 1);
        CHECK(f.driver.chosenAlpha == VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR);
        f.Draw();
        CHECK(f.driver.recordedClearAlpha == 1.0F);
        CHECK(f.driver.submits == 1);
        CHECK(f.driver.presents == 1);
    }

    TEST_CASE("Android Vulkan rejects unrelated composite alpha modes before native swapchain creation",
              "[renderer][vulkan-android][protocol-fake][alpha]") {
        Fixture f;
        SECTION("premultiplied only") {
            f.driver.supportedAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
        }
        SECTION("postmultiplied only") {
            f.driver.supportedAlpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
        }
        SECTION("both unrelated modes") {
            f.driver.supportedAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR | VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
        }
        f.Initialize();
        f.Available();
        const auto available = f.runtime->Availability({640, 480});
        REQUIRE(available.HasError());
        CHECK(available.ErrorValue().code.Value() == Errors::Unsupported.code.Value());
        CHECK(f.driver.chainCreates == 0);
        CHECK(f.driver.commandBegins == 0);
        CHECK(f.driver.surfaceDestroys == 1);
    }

    TEST_CASE("Android Vulkan inherit rejects nonunit clear alpha before any command recording",
              "[renderer][vulkan-android][protocol-fake][alpha]") {
        Fixture f;
        f.driver.supportedAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        PrimaryOutputAttachment attachment;
        SECTION("transparent") {
            attachment.clearColor.alpha = 0.0F;
        }
        SECTION("fractional") {
            attachment.clearColor.alpha = 0.5F;
        }
        SECTION("above one") {
            attachment.clearColor.alpha = 2.0F;
        }
        SECTION("negative") {
            attachment.clearColor.alpha = -1.0F;
        }
        f.Initialize();
        f.Available();
        CHECK(Value(f.runtime->Availability({640, 480})));
        const auto frame = Value(f.runtime->Begin({1, {640, 480}}));
        const std::array passes{RenderPassDescriptor{.id = {1}, .primaryOutput = attachment}};
        const auto executed = f.runtime->Execute({frame, passes});
        REQUIRE(executed.HasError());
        CHECK(executed.ErrorValue().code.Value() == Errors::Unsupported.code.Value());
        CHECK(f.driver.commandBegins == 0);
        CHECK(f.driver.passes == 0);
        CHECK(f.driver.submits == 0);
        CHECK(f.driver.presents == 0);
        f.runtime->Abort(frame);
    }

    TEST_CASE("Android Vulkan native output stays available while every frame credit is busy",
              "[renderer][vulkan-android][protocol-fake]") {
        Fixture f;
        f.Initialize();
        f.Available();
        CHECK(Value(f.runtime->Availability({640, 480})));
        const auto original = Value(f.windows->Snapshot());
        f.driver.renderReady = false;
        f.driver.presentReady = false;
        f.driver.acquiredImage = 0;
        f.Draw();
        f.driver.acquiredImage = 1;
        f.Draw();
        CHECK(f.driver.acquisitions == 2);
        CHECK(Value(f.runtime->Availability({640, 480})));
        const auto blocked = f.runtime->Begin({3, {640, 480}});
        REQUIRE(blocked.HasError());
        CHECK(IsVulkanAndroidBackpressure(blocked.ErrorValue()));
        CHECK(f.driver.acquisitions == 2);
        CHECK(f.driver.surfaceCreates == 1);
        CHECK(f.driver.surfaceDestroys == 0);
        f.driver.renderReady = true;
        CHECK(Value(f.runtime->Availability({640, 480})));
        CHECK(Value(f.windows->Snapshot()).window == original.window);
        CHECK(Value(f.windows->Snapshot()).revision == original.revision);
    }

    TEST_CASE("Android Vulkan zero-time waits preserve independent acquisition rendering and presentation debt",
              "[renderer][vulkan-android][protocol-fake][fence]") {
        Fixture f;
        f.Initialize();
        const auto window = f.Available();
        CHECK(Value(f.runtime->Availability({640, 480})));
        f.driver.acquireReady = false;
        const auto pending = f.runtime->Begin({1, {640, 480}});
        REQUIRE(pending.HasError());
        CHECK(IsVulkanAndroidBackpressure(pending.ErrorValue()));
        CHECK(f.driver.waitsByKind[static_cast<std::size_t>(Driver::Kind::Acquire)] > 0);
        CHECK(f.driver.submits == 0);
        CHECK(f.driver.surfaceDestroys == 0);

        f.driver.acquireReady = true;
        f.driver.renderReady = false;
        f.driver.presentReady = false;
        f.Draw();
        CHECK(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.waitsByKind[static_cast<std::size_t>(Driver::Kind::Render)] > 0);
        CHECK(f.driver.waitsByKind[static_cast<std::size_t>(Driver::Kind::Present)] > 0);
        Accepted(f.windows->ObserveLost(window));
        f.Commit();
        CHECK_FALSE(Value(f.runtime->Availability({640, 480})));
        CHECK(f.native[0].references > 0);
        CHECK(f.driver.surfaceDestroys == 0);
        f.driver.renderReady = true;
        CHECK_FALSE(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.surfaceDestroys == 0);
        f.driver.presentReady = true;
        CHECK_FALSE(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.surfaceDestroys == 1);
        CHECK(f.native[0].references == 0);
    }

    TEST_CASE("Android Vulkan zero-time native fence failures propagate without releasing live resources",
              "[renderer][vulkan-android][protocol-fake][fence]") {
        Fixture f;
        f.Initialize();
        f.Available();
        CHECK(Value(f.runtime->Availability({640, 480})));
        f.Draw();
        f.driver.fenceFailure = VK_ERROR_OUT_OF_HOST_MEMORY;
        const auto failed = f.runtime->Availability({640, 480});
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().domain.Value() == Errors::NativeFailure.domain.Value());
        CHECK(failed.ErrorValue().code.Value() == Errors::NativeFailure.code.Value());
        CHECK(failed.ErrorValue().message.find("vkWaitForFences") != std::string::npos);
        CHECK(f.driver.surfaceDestroys == 0);
        CHECK(f.native[0].references > 0);
        f.driver.fenceFailure = VK_SUCCESS;
    }

    TEST_CASE("Android Vulkan loss retains authentic old windows until both render and present proofs retire",
              "[renderer][vulkan-android][protocol-fake]") {
        Fixture f;
        f.Initialize();
        const auto first = f.Available();
        CHECK(Value(f.runtime->Availability({640, 480})));
        f.driver.renderReady = false;
        f.driver.presentReady = false;
        f.Draw();
        Accepted(f.windows->ObserveLost(first));
        f.Commit();
        CHECK_FALSE(Value(f.runtime->Availability({640, 480})));
        CHECK(f.native[0].references > 0);
        CHECK(f.driver.surfaceDestroys == 0);
        f.driver.renderReady = true;
        CHECK_FALSE(Value(f.runtime->Availability({640, 480})));
        CHECK(f.native[0].references > 0);
        f.driver.presentReady = true;
        CHECK_FALSE(Value(f.runtime->Availability({640, 480})));
        CHECK(f.native[0].references == 0);
        CHECK(f.driver.surfaceDestroys == 1);
    }

    TEST_CASE("Android Vulkan replacement never imports a new window while old presentation debt is pending",
              "[renderer][vulkan-android][protocol-fake]") {
        Fixture f;
        f.Initialize();
        f.Available();
        CHECK(Value(f.runtime->Availability({640, 480})));
        f.driver.presentReady = false;
        f.Draw();
        const auto replacement = f.Available(1);
        CHECK_FALSE(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.surfaceCreates == 1);
        f.driver.presentReady = true;
        CHECK_FALSE(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.surfaceDestroys == 1);
        CHECK(f.driver.surfaceCreates == 1);
        CHECK(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.surfaceCreates == 2);
        CHECK(f.driver.deviceCreates == 1);
        CHECK(f.native[0].references == 0);
        CHECK(Value(f.windows->Snapshot()).window == replacement);
    }

    TEST_CASE("Android Vulkan canceled acquisition retires by its fence and returns the exact image without rendering",
              "[renderer][vulkan-android][protocol-fake]") {
        Fixture f;
        f.Initialize();
        const auto window = f.Available();
        f.driver.acquireReady = false;
        CHECK(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.acquisitions == 0);
        const auto pending = f.runtime->Begin({1, {640, 480}});
        REQUIRE(pending.HasError());
        CHECK(IsVulkanAndroidBackpressure(pending.ErrorValue()));
        CHECK(f.driver.acquisitions == 1);
        Accepted(f.windows->ObserveLost(window));
        f.Commit();
        CHECK_FALSE(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.releases == 0);
        f.driver.acquireReady = true;
        CHECK_FALSE(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.releases == 1);
        CHECK(f.driver.submits == 0);
        CHECK(f.native[0].references == 0);
    }

    TEST_CASE("Android Vulkan invalid acquired image retains actual acquisition debt until whole chain retirement",
              "[renderer][vulkan-android][protocol-fake]") {
        Fixture f;
        f.Initialize();
        f.Available();
        CHECK(Value(f.runtime->Availability({640, 480})));
        f.driver.acquiredImage = 8;
        f.driver.acquireReady = false;
        const auto frame = f.runtime->Begin({1, {640, 480}});
        REQUIRE(frame.HasError());
        CHECK(frame.ErrorValue().code.Value() == Errors::Invalid.code.Value());
        Accepted(f.runtime->Close());
        CHECK(Value(f.runtime->Drain()) == RenderBackendDrainProgress::Pending);
        CHECK(f.driver.surfaceDestroys == 0);
        CHECK(f.native[0].references > 0);
        f.driver.acquireReady = true;
        CHECK(Value(f.runtime->Drain()) == RenderBackendDrainProgress::Drained);
        CHECK(f.driver.surfaceDestroys == 1);
        CHECK(f.driver.releases == 0);
        CHECK(f.driver.submits == 0);
    }

    TEST_CASE("Android Vulkan rejects unsupported work before recording and recovers an unsent frame",
              "[renderer][vulkan-android][protocol-fake]") {
        Fixture f;
        f.Initialize();
        f.Available();
        CHECK(Value(f.runtime->Availability({640, 480})));
        const auto frame = Value(f.runtime->Begin({1, {640, 480}}));
        const std::array passes{RenderPassDescriptor{.id = {1}, .kind = RenderPassKind::Compute}};
        REQUIRE(f.runtime->Execute({frame, passes}).HasError());
        CHECK(f.driver.passes == 0);
        CHECK(f.driver.submits == 0);
        f.runtime->Abort(frame);
        CHECK(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.releases == 1);
        f.Draw();
        CHECK(f.driver.presents == 1);
    }

    TEST_CASE("Android Vulkan partial swapchain failure releases only its candidate and allows an actual retry",
              "[renderer][vulkan-android][protocol-fake]") {
        Fixture f;
        f.Initialize();
        f.Available();
        f.driver.failSwapchain = true;
        REQUIRE(f.runtime->Availability({640, 480}).HasError());
        CHECK(f.driver.surfaceDestroys == 1);
        CHECK(f.driver.submits == 0);
        f.driver.failSwapchain = false;
        CHECK(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.surfaceCreates == 2);
    }

    TEST_CASE("Android Vulkan rejected native presentation preserves pending debt and its native cause",
              "[renderer][vulkan-android][protocol-fake]") {
        Fixture f;
        f.Initialize();
        f.Available();
        CHECK(Value(f.runtime->Availability({640, 480})));
        const auto originalWindow = Value(f.windows->Snapshot());
        const auto frame = Value(f.runtime->Begin({1, {640, 480}}));
        const std::array passes{
            RenderPassDescriptor{.id = {1}, .kind = RenderPassKind::Graphics, .primaryOutput = PrimaryOutputAttachment{}}};
        Accepted(f.runtime->Execute({frame, passes}));
        f.driver.presentResult = VK_ERROR_OUT_OF_DATE_KHR;
        f.driver.presentReady = false;
        f.driver.renderReady = false;
        const auto presented = f.runtime->Present(frame);
        REQUIRE(presented.HasError());
        CHECK(IsVulkanAndroidBackpressure(presented.ErrorValue()));
        REQUIRE(presented.ErrorValue().cause.Get());
        CHECK(presented.ErrorValue().cause.Get()->code.Value() == Errors::NativeFailure.code.Value());
        CHECK_FALSE(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.surfaceDestroys == 0);
        f.driver.presentReady = true;
        CHECK_FALSE(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.surfaceDestroys == 0);
        f.driver.renderReady = true;
        CHECK_FALSE(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.surfaceDestroys == 1);
        CHECK(f.driver.surfaceCreates == 1);
        CHECK(f.driver.acquisitions == 1);
        const auto unchangedWindow = Value(f.windows->Snapshot());
        CHECK(unchangedWindow.window == originalWindow.window);
        CHECK(unchangedWindow.revision == originalWindow.revision);
        CHECK(Value(f.runtime->Availability({640, 480})));
        CHECK(f.driver.surfaceCreates == 2);
    }

    TEST_CASE("Android Vulkan resize replaces native generation resources while preserving the initialized device",
              "[renderer][vulkan-android][protocol-fake]") {
        Fixture f;
        f.Initialize();
        f.Available();
        CHECK(Value(f.runtime->Availability({640, 480})));
        f.Draw();
        f.native[0].extent = {320, 240};
        f.Commit();
        CHECK_FALSE(Value(f.runtime->Availability({320, 240})));
        CHECK(f.driver.surfaceDestroys == 1);
        CHECK(f.driver.surfaceCreates == 1);
        CHECK(Value(f.runtime->Availability({320, 240})));
        CHECK(f.driver.surfaceCreates == 2);
        CHECK(f.driver.deviceCreates == 1);
    }

    TEST_CASE("Android Vulkan terminal drain releases window leases before the platform owner closes",
              "[renderer][vulkan-android][protocol-fake]") {
        Fixture f;
        f.Initialize();
        f.Available();
        CHECK(Value(f.runtime->Availability({640, 480})));
        f.driver.presentReady = false;
        f.Draw();
        Accepted(f.runtime->Close());
        CHECK(Value(f.runtime->Drain()) == RenderBackendDrainProgress::Pending);
        CHECK(f.native[0].references > 0);
        f.driver.presentReady = true;
        CHECK(Value(f.runtime->Drain()) == RenderBackendDrainProgress::Drained);
        CHECK(Value(f.windows->Close()));
        CHECK(f.native[0].references == 0);
    }
}  // namespace Horo::Render::AndroidVulkan::Testing
