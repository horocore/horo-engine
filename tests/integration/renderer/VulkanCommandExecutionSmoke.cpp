#include "../../support/renderer/RenderBackendDrainTestSupport.h"
#include "../../support/renderer/RenderGraphSubmissionFixture.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Vfx/CpuParticleSimulator.h"
#include "Horo/Vfx/GpuParticleSimulator.h"
#include "Horo/Vfx/VfxWorld.h"
#include "VfxGpuVulkanCook.h"
#include "support/VfxTestSupport.h"

#include <VulkanBackendModule.h>
#include <array>
#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <memory>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    /** @brief Explicit Linux qualification host opens only the installed system loader, never a project library or API fallback. */
    class SystemLoader final : public IVulkanLoaderPort {
    public:
        ~SystemLoader() override {
            ReleaseLoader();
        }

        Result<void> AcquireLoader() override {
            if (library_ != nullptr)
                return Result<void>::Success();
#if defined(__aarch64__)
            constexpr std::array paths{"/usr/lib/aarch64-linux-gnu/libvulkan.so.1", "/lib/aarch64-linux-gnu/libvulkan.so.1"};
#elif defined(__x86_64__)
            constexpr std::array paths{"/usr/lib/x86_64-linux-gnu/libvulkan.so.1", "/lib/x86_64-linux-gnu/libvulkan.so.1"};
#else
            constexpr std::array<const char *, 0> paths{};
#endif
            for (const char *path : paths) {
                library_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
                if (library_ != nullptr)
                    break;
            }
            if (library_ == nullptr)
                return Failure("The installed Linux Vulkan loader is unavailable.");
            resolver_ = std::bit_cast<PFN_vkGetInstanceProcAddr>(dlsym(library_, "vkGetInstanceProcAddr"));
            if (resolver_ == nullptr) {
                ReleaseLoader();
                return Failure("The installed loader has no vkGetInstanceProcAddr.");
            }
            return Result<void>::Success();
        }

        PFN_vkGetInstanceProcAddr GetInstanceProcAddr() noexcept override {
            if (active_ != nullptr && active_ != this)
                return nullptr;
            active_ = this;
            return &ResolveInstance;
        }

        Result<std::vector<std::string>> RequiredPresentationExtensions() override {
            return Result<std::vector<std::string>>::Success({});
        }

        Result<bool> SupportsPresentation(VkPhysicalDevice, std::uint32_t) override {
            return Result<bool>::Success(false);
        }

        Result<bool> IsDriverAllowed(const VkPhysicalDeviceProperties &) override {
            // Explicit CI host policy admits the selected installed driver for native qualification.
            return Result<bool>::Success(true);
        }

        void ArmSecondSubmitFailure() {
            REQUIRE(active_ == this);
            REQUIRE(!faultArmed_);
            submitAttempts_ = forwardedSubmits_ = injectedSubmits_ = 0;
            faultArmed_ = true;
        }

        std::uint32_t SubmitAttempts() const noexcept {
            return submitAttempts_;
        }

        std::uint32_t ForwardedSubmits() const noexcept {
            return forwardedSubmits_;
        }

        std::uint32_t InjectedSubmits() const noexcept {
            return injectedSubmits_;
        }

        void ReleaseLoader() noexcept override {
            if (active_ == this)
                active_ = nullptr;
            deviceResolver_ = nullptr;
            submit_ = nullptr;
            resolver_ = nullptr;
            if (library_ != nullptr)
                dlclose(std::exchange(library_, nullptr));
        }

    private:
        static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ResolveInstance(VkInstance instance, const char *name) {
            auto &owner = *active_;
            const auto actual = owner.resolver_(instance, name);
            if (actual && std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
                owner.deviceResolver_ = std::bit_cast<PFN_vkGetDeviceProcAddr>(actual);
                return std::bit_cast<PFN_vkVoidFunction>(&ResolveDevice);
            }
            return actual;
        }

        static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ResolveDevice(VkDevice device, const char *name) {
            auto &owner = *active_;
            const auto actual = owner.deviceResolver_(device, name);
            if (actual && std::strcmp(name, "vkQueueSubmit2") == 0) {
                owner.submit_ = std::bit_cast<PFN_vkQueueSubmit2>(actual);
                return std::bit_cast<PFN_vkVoidFunction>(&Submit);
            }
            return actual;
        }

        static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo2 *submissions, VkFence fence) {
            auto &owner = *active_;
            ++owner.submitAttempts_;
            if (owner.faultArmed_ && owner.submitAttempts_ == 2) {
                owner.faultArmed_ = false;
                ++owner.injectedSubmits_;
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            ++owner.forwardedSubmits_;
            return owner.submit_(queue, count, submissions, fence);
        }

        inline static thread_local SystemLoader *active_{};
        PFN_vkGetDeviceProcAddr deviceResolver_{};
        PFN_vkQueueSubmit2 submit_{};
        std::uint32_t submitAttempts_{}, forwardedSubmits_{}, injectedSubmits_{};
        bool faultArmed_{};

        static Result<void> Failure(const char *message) {
            return Result<void>::Failure(Error{ErrorCode{"render.test.system_loader_unavailable"},
                                               ErrorDomainId{"horo.render.test"},
                                               ErrorSeverity::Critical,
                                               message,
                                               {}});
        }

        void *library_{};
        PFN_vkGetInstanceProcAddr resolver_{};
    };

    /** @brief Loader and registry survive the frontend's explicit bounded owner drain, including assertion unwind. */
    struct NativeOwner {
        SystemLoader loader;
        RenderBackendRegistry registry;
        std::unique_ptr<RenderFrontend> frontend;
        std::vector<RenderQueueAssignment> selectedQueues;

        NativeOwner() {
            REQUIRE(RegisterVulkanRenderBackend(registry, loader).HasValue());
            REQUIRE(registry.Seal().HasValue());
        }

        ~NativeOwner() {
            if (frontend != nullptr) {
                const auto begin = frontend->BeginShutdown();
                if (begin.HasError())
                    std::terminate();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                for (;;) {
                    const auto progress = frontend->PollDrain();
                    if (progress.HasValue() && progress.Value() == RenderBackendDrainProgress::Drained)
                        break;
                    if (std::chrono::steady_clock::now() >= deadline)
                        std::terminate();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }

        Result<void> Initialize(const bool distinctTransfer) {
            RenderBackendConfig config{.requirePresentation = false, .enableValidation = true};
            config.queueAssignments = {{RenderQueueRole::Graphics, {1}}, {RenderQueueRole::Transfer, {distinctTransfer ? 2U : 1U}}};
            selectedQueues = config.queueAssignments;
            auto created = RenderFrontend::Create(registry, RenderBackendId{"vulkan"}, config);
            if (created.HasError())
                return Result<void>::Failure(std::move(created).ErrorValue());
            frontend = std::move(created).Value();
            return Result<void>::Success();
        }
    };

    void RequireCompletion(RenderFrontend &frontend, const RenderTimelinePoint point) {
        const RenderCpuWait wait{point, RenderCpuWaitPurpose::DeterministicTest, std::chrono::seconds(5),
                                 frontend.Capabilities().support.deviceIncarnation};
        const auto completed = frontend.ObserveSubmission(wait);
        INFO((completed.HasError() ? completed.ErrorValue().message : "native timeline observation succeeded"));
        REQUIRE(completed.HasValue());
        REQUIRE(completed.Value());
    }

    void ExecuteCopies(RenderFrontend &frontend, const RenderQueueId consumer) {
        std::array<RenderBufferHandle, 3> handles;
        std::array<ResourceOperationId, 3> operations;
        std::array<std::byte, 16> bytes{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}};
        for (std::size_t index = 0; index < handles.size(); ++index) {
            auto created = frontend.CreateBuffer({.byteSize = bytes.size(),
                                                  .usage = RenderBufferUsage::CopySource | RenderBufferUsage::CopyDestination,
                                                  .access = RenderBufferAccess::HostVisible},
                                                 index == 0 ? std::span<const std::byte>{bytes} : std::span<const std::byte>{});
            INFO((created.HasError() ? created.ErrorValue().message : "native buffer admitted"));
            REQUIRE(created.HasValue());
            handles[index] = created.Value().handle;
            operations[index] = created.Value().operation;
        }
        const auto processed = frontend.ProcessResourceRequests();
        REQUIRE(processed.HasValue());
        REQUIRE(processed.Value() == 3);
        for (const auto operation : operations) {
            const auto completed = frontend.ResourceOperationResult(operation);
            INFO((completed.HasError() ? completed.ErrorValue().message : "native allocation published"));
            REQUIRE(completed.HasValue());
        }
        auto packet = SubmissionTest::CopyGraph(handles, {1}, consumer);
        const auto submissions = packet.Submissions();
        auto begun = frontend.BeginFrame({.frameNumber = 1, .outputExtent = {16, 16}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        const auto executed = frame.ExecuteSubmittedGraph(packet.graph, packet.workloads, submissions);
        INFO((executed.HasError() ? executed.ErrorValue().message : "native copies recorded"));
        REQUIRE(executed.HasValue());
        const auto submitted = frame.Present();
        INFO((submitted.HasError() ? submitted.ErrorValue().message : "native copies submitted"));
        REQUIRE(submitted.HasValue());
        RequireCompletion(frontend, submissions.back().submission.signal);
        for (const auto handle : handles)
            REQUIRE(frontend.ReleaseBuffer(handle).HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
    }

    void ExecuteColorAttachments(RenderFrontend &frontend) {
        using SubmissionTest::Take;
        const auto texture = frontend.CreateTexture(
            {.extent = {4, 4}, .format = RenderTextureFormat::Rgba8Unorm, .usage = RenderTextureUsage::RenderAttachment});
        REQUIRE(texture.HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        const auto ready = frontend.ResourceOperationResult(texture.Value().operation);
        INFO((ready.HasError() ? ready.ErrorValue().message : "native color image published"));
        REQUIRE(ready.HasValue());
        auto builder = Take(RenderGraphBuilder::Create({.maxPasses = 2, .maxResources = 1, .maxUsages = 2, .maxDependencies = 2}));
        const auto clear = Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
        const auto load = Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
        const auto image = Take(builder.ImportTexture(texture.Value().handle, RenderGraphResourceClass::Persistent));
        REQUIRE(builder.AddUsage({clear, image, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment}).HasValue());
        REQUIRE(builder.AddUsage({load, image, RenderGraphAccess::ReadWrite, RenderGraphUsageKind::ColorAttachment}).HasValue());
        const auto graph = Take(builder.Finalize());
        const auto schedule = Take(CompileRenderGraph(graph));
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        const std::array initial{RenderGraphImportedState{image, {}, RenderGraphInitialContents::Discarded}};
        const auto synchronization = Take(SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial));
        const auto compiled = Take(CompileRenderGraphExecution(graph, schedule, synchronization, queues));
        const std::array workloads{RenderGraphPassWorkload{clear, RenderGraphColorAttachment{image, {.clearColor = {1, 0, 0, 1}}}},
                                   RenderGraphPassWorkload{load,
                                                           RenderGraphColorAttachment{image,
                                                                                      {.loadOperation = AttachmentLoadOperation::Load}}}};
        const std::array submissions{RenderGraphQueueSubmission{RenderQueueSubmission{{1}, {3}, {}, {{1}, 3}}, {0, 2}}};
        auto begun = frontend.BeginFrame({.frameNumber = 2, .outputExtent = {4, 4}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        const auto executed = frame.ExecuteSubmittedGraph(compiled, workloads, submissions);
        INFO((executed.HasError() ? executed.ErrorValue().message : "native clear/load recorded"));
        REQUIRE(executed.HasValue());
        REQUIRE(frame.Present().HasValue());
        RequireCompletion(frontend, submissions.front().submission.signal);
        REQUIRE(frontend.ReleaseTexture(texture.Value().handle).HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
    }

    TEST_CASE("Vulkan system driver executes canonical offscreen copy submissions "
              "and timeline observation",
              "[integration][renderer][vulkan][gpu]") {
        NativeOwner owner;
        const auto initialized = owner.Initialize(false);
        INFO((initialized.HasError() ? initialized.ErrorValue().message : "Vulkan 1.3 native command owner admitted"));
        REQUIRE(initialized.HasValue());
        REQUIRE(owner.frontend->Capabilities().supportsExplicitGraphSubmissions);
        REQUIRE(owner.frontend->Capabilities().supportsSubmissionObservation);
        REQUIRE_FALSE(owner.frontend->Capabilities().support.synthetic);
        REQUIRE(owner.frontend->Capabilities().support.deviceIncarnation != 0);
        ExecuteCopies(*owner.frontend, {1});
        ExecuteColorAttachments(*owner.frontend);
        Horo::Render::Test::RequireDrainedOwner(*owner.frontend);
    }

    TEST_CASE("Vulkan distinct-family native execution never aliases an "
              "unavailable transfer queue",
              "[integration][renderer][vulkan][gpu][ownership]") {
        NativeOwner owner;
        const auto initialized = owner.Initialize(true);
        if (initialized.HasError() && initialized.ErrorValue().code.Value() == "render.vulkan.graphics_queue_unavailable") {
            REQUIRE(owner.frontend == nullptr);
            SKIP("The actual driver has no separate transfer-capable queue family; "
                 "cross-family native qualification is unavailable.");
        }
        INFO((initialized.HasError() ? initialized.ErrorValue().message : "distinct native queue families admitted"));
        REQUIRE(initialized.HasValue());
        ExecuteCopies(*owner.frontend, {2});
        Horo::Render::Test::RequireDrainedOwner(*owner.frontend);
    }

#include "VulkanVfxGpuNativeSmoke.inl"
}  // namespace
