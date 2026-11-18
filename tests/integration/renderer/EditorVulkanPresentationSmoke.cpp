/** @file EditorVulkanPresentationSmoke.cpp
 * @brief Real SDL/Vulkan GUI and cooked viewport composition; no simulated native admission. */
#include "CameraCutNativeTestSupport.h"
#include "ColorOutputPackage.h"
#include "EditorVulkanShaderAssets.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "VulkanBackendModule.h"
#include "VulkanEditorGraphicsBridge.h"
#include "editor/renderer/vulkan/EditorGuiRendererVulkan.h"
#include "editor/renderer/vulkan/EditorViewportRendererVulkan.h"
#include "editor/renderer/vulkan/SdlVulkanPresentationPort.h"

#include <SDL3/SDL.h>
#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <imgui.h>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace Horo::Tests::VulkanEditor {
    using namespace Render;
    using namespace Editor;

    template <typename T> void Check(const Result<T> &result, const std::string_view operation = "native composition") {
        if (result.HasError()) {
            const auto &error = result.ErrorValue();
            INFO("Vulkan / SDL WSI / " << operation);
            INFO(error.domain.Value() << "/" << error.code.Value() << ": " << error.message);
            FAIL("Vulkan operation failed");
        }
    }

    /** @brief Keeps process/window initialization exception-safe before the complete host exists. */
    struct SdlContext {
        SdlContext() {
            REQUIRE(SDL_Init(SDL_INIT_VIDEO));
        }

        ~SdlContext() {
            SDL_Quit();
        }
    };

    /** @brief Test host retains the production native parents through every finite drain poll. */
    struct Host {
        SdlContext sdl;
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{nullptr, &SDL_DestroyWindow};
        std::unique_ptr<SdlVulkanPresentationPort> port;
        std::unique_ptr<Render::Detail::VulkanEditorGraphicsBridge> bridge;
        std::unique_ptr<RenderBackendRegistry> registry;
        std::unique_ptr<RenderFrontend> frontend;
        std::unique_ptr<EditorGuiRendererVulkan> gui;
        std::unique_ptr<EditorViewportRendererVulkan> viewport;
        bool drained{};
        RenderTargetHandle target;
        const FramebufferExtent extent{640, 480};

        Host() {
            window.reset(
                SDL_CreateWindow("Horo Vulkan qualification", extent.width, extent.height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE));
            REQUIRE(window);
            port = std::make_unique<SdlVulkanPresentationPort>(*window);
            bridge = std::make_unique<Render::Detail::VulkanEditorGraphicsBridge>();
            registry = std::make_unique<RenderBackendRegistry>();
            Check(RegisterVulkanRenderBackend(*registry, *port, bridge.get()));
            Check(registry->Seal());
            auto created = RenderFrontend::Create(*registry, RenderBackendId{"vulkan"},
                                                  {.requirePresentation = true, .enableValidation = true, .maxFramesInFlight = 2}, {},
                                                  {.budget = {.maximumBlocks = 4096}});
            Check(created);
            frontend = std::move(created).Value();
            Check(frontend->Resize(extent));
            ImGui::CreateContext();
            ImGui::GetIO().IniFilename = nullptr;
            gui = std::make_unique<EditorGuiRendererVulkan>(*window, *frontend, *bridge);
            Check(gui->Initialize());
        }

        ~Host() {
            if (frontend && !Drain())
                std::terminate();
            ReleaseParents();
        }

        bool Drain() {
            if (drained)
                return true;
            if (frontend->BeginShutdown().HasError())
                return false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            do {
                const auto progress = frontend->PollDrain();
                if (progress.HasError())
                    return false;
                if (progress.Value() == RenderBackendDrainProgress::Drained) {
                    drained = true;
                    return true;
                }
                // Explicit native qualification budget only; production never waits here.
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        }

        void ReleaseParents() {
            viewport.reset();
            gui.reset();
            if (ImGui::GetCurrentContext())
                ImGui::DestroyContext();
            frontend.reset();
            // Registration factories also retain the native runtime in this test host.
            registry.reset();
            bridge.reset();
            port.reset();
            window.reset();
        }

        void RecordCapabilities() const {
            const auto borrowed = bridge->Port()->QueryDevice();
            Check(borrowed);
            const auto &device = borrowed.Value();
            const auto properties =
                std::bit_cast<PFN_vkGetPhysicalDeviceProperties>(device.instanceResolver(device.instance, "vkGetPhysicalDeviceProperties"));
            REQUIRE(properties);
            VkPhysicalDeviceProperties identity{};
            properties(device.physical, &identity);
            std::cout << "Vulkan device " << identity.deviceName << ", driver " << identity.driverVersion << ", API "
                      << VK_API_VERSION_MAJOR(identity.apiVersion) << '.' << VK_API_VERSION_MINOR(identity.apiVersion) << '.'
                      << VK_API_VERSION_PATCH(identity.apiVersion) << '\n';
            const auto query =
                std::bit_cast<PFN_vkGetPhysicalDeviceFeatures2>(device.instanceResolver(device.instance, "vkGetPhysicalDeviceFeatures2"));
            REQUIRE(query);
            VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
            VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &maintenance};
            query(device.physical, &features);
            REQUIRE(maintenance.swapchainMaintenance1 == VK_TRUE);
            std::cout << "{\"swapchainMaintenance1\":true,\"sameInstanceSurfaceAdmitted\":true,\"frames\":" << device.frames
                      << ",\"format\":" << static_cast<unsigned>(device.outputFormat) << "}\n";
        }

        void CreateViewport(const RenderSceneView &scene = {}, const EditorViewportColorInputs *color = nullptr) {
            const char *root = std::getenv("HORO_EDITOR_VULKAN_ASSET_ROOT");
            REQUIRE(root);
            auto cooked = LoadPackagedVulkanViewportShader(std::filesystem::path{root});
            Check(cooked);
            viewport = std::make_unique<EditorViewportRendererVulkan>(*frontend, *bridge, std::move(cooked).Value());
            Check(viewport->Initialize());
            if (color) {
                auto linear = LoadPackagedVulkanSceneLinearShader(std::filesystem::path{root});
                Check(linear, "load exact scene-linear material");
                Check(viewport->PrepareManualColor(std::move(linear).Value(), *color, {}), "activate manual Vulkan viewport color");
            }
            const auto view = frontend->CreateRenderViewIdentity();
            Check(view);
            Check(viewport->BindLightingViewIdentity(view.Value()));
            EditorViewportLightingProfile lighting;
            lighting.requested = LightPreparationMode::BaselineForwardCpu;
            const auto selected = frontend->ResolveLightPreparation(lighting.requested, lighting.fallback, {});
            Check(selected);
            Check(viewport->RequestLightingProfile(lighting, selected.Value()));
            viewport->RequestExtent({320, 240});
            viewport->RequestGrid({.visible = false});
            SynchronizeViewport(scene);
        }

        void SynchronizeViewport(const RenderSceneView &scene, const EditorViewportExtent requested = {320, 240}) {
            for (std::size_t attempt = 0; attempt < 8; ++attempt) {
                const auto prepared = viewport->PrepareResources(*frontend, scene);
                Check(prepared);
                Check(frontend->ProcessResourceRequests());
                if (prepared.Value().has_value() && viewport->RequestedExtent() == requested) {
                    target = *prepared.Value();
                    return;
                }
            }
            FAIL("Viewport resources did not become ready within the bounded preparation budget");
        }

        RenderFrameScope Frame(const std::uint64_t number) {
            return Frame(number, extent);
        }

        RenderFrameScope Frame(const std::uint64_t number, const FramebufferExtent output) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            do {
                const auto available = frontend->PollPresentationAvailability(output);
                Check(available, "poll swapchain acquisition");
                if (available.Value()) {
                    auto frame = frontend->BeginFrame({number, output});
                    Check(frame, "begin acquired frame");
                    return std::move(frame).Value();
                }
                SDL_PumpEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            } while (std::chrono::steady_clock::now() < deadline);
            FAIL("Vulkan / SDL WSI / poll swapchain acquisition: no retired slot ready within 5 seconds");
            std::terminate();
        }

        StaticMeshPassDescriptor ViewportPass(const RenderSceneView &scene) const {
            const auto size = viewport->RequestedExtent();
            return {.target = target, .extent = {size.width, size.height}, .scene = scene};
        }

        void PrepareScene(const RenderSceneView &scene = {}) {
            Check(viewport->PrepareLightingFrame(ViewportPass(scene), viewport->LightingViewIdentity()), "prepare viewport graph");
        }

        void ExecuteSceneAndGui(RenderFrameScope &frame, const RenderSceneView &scene = {}) {
            const std::array passes{RenderPassDescriptor{.kind = RenderPassKind::Graphics, .staticMesh = ViewportPass(scene)},
                                    RenderPassDescriptor{.kind = RenderPassKind::Graphics, .primaryOutput = PrimaryOutputAttachment{}}};
            Check(viewport->ExecutePreparedFrame(frame, passes));
            const auto view = viewport->TextureView();
            REQUIRE(view.IsValid());
            Check(gui->BeginFrame());
            ImGui::NewFrame();
            ImGui::GetForegroundDrawList()->AddImage(static_cast<ImTextureID>(view.textureId), {0, 0}, {320, 240});
            ImGui::GetForegroundDrawList()->AddText({8, 8}, IM_COL32_WHITE, "Canonical Vulkan GUI tail");
            ImGui::Render();
            gui->BindViewportTexture(view);
            Check(frame.ComposeGui(*gui));
        }
    };

    TEST_CASE("Native Vulkan partial GUI preparation releases native children before SDL parents", "[native-smoke][vulkan][gui]") {
        Host host;
        REQUIRE(ImGui::GetIO().BackendRendererUserData);
        REQUIRE(host.Drain());
        CHECK(ImGui::GetIO().BackendRendererUserData == nullptr);
        CHECK(host.port->GetInstanceProcAddr() == nullptr);
        CHECK(SDL_GetWindowID(host.window.get()) != 0);
        host.gui.reset();
        CHECK(ImGui::GetIO().BackendPlatformUserData == nullptr);
        CHECK(SDL_GetWindowID(host.window.get()) != 0);
    }

    TEST_CASE("Native Vulkan keeps frame ownership and resumes presentation after an observed SDL pixel resize",
              "[native-smoke][vulkan][presentation]") {
        Host host;
        INFO("Vulkan / SDL window and swapchain / " << SDL_GetCurrentVideoDriver());
        host.RecordCapabilities();
        const auto device = host.bridge->Port()->QueryDevice();
        Check(device, "query realized native attachment");
        const auto initialOutput = host.frontend->PresentationOutput();
        if (device.Value().outputFormat == VK_FORMAT_B8G8R8A8_UNORM) {
            Check(initialOutput, "observe unmanaged SDR attachment");
            CHECK(initialOutput.Value().active == RenderOutputColor{});
            CHECK(initialOutput.Value().display.attached);
            CHECK(initialOutput.Value().display.hdr == RenderDisplayHdrSupport::Unknown);
        } else {
            REQUIRE(initialOutput.HasError());
            CHECK(initialOutput.ErrorValue().code.Value() == RenderPresentationErrors::OutputUnsupported.code.Value());
        }
        const std::array passes{RenderPassDescriptor{.kind = RenderPassKind::Graphics, .primaryOutput = PrimaryOutputAttachment{}}};
        {
            auto initial = host.Frame(1);
            Check(initial.Execute(passes), "record initial SDR output");
            Check(initial.Present(), "present initial SDR output");
        }
        constexpr FramebufferExtent logical{512, 384};
        {
            auto active = host.Frame(2);
            const auto rejected = host.frontend->Resize(logical);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == "render.frontend.resize_during_frame");
            active.Cancel();
        }
        const auto suspended = host.frontend->PollPresentationAvailability({});
        Check(suspended, "poll without a drawable extent");
        CHECK_FALSE(suspended.Value());
        {
            INFO("Vulkan / SDL window / resize requested logical extent to " << logical.width << "x" << logical.height);
            REQUIRE(SDL_SetWindowSize(host.window.get(), static_cast<int>(logical.width), static_cast<int>(logical.height)));
            FramebufferExtent output;
            const auto pollWindow = [&] {
                int width{}, height{};
                INFO(SDL_GetError());
                REQUIRE(SDL_GetWindowSizeInPixels(host.window.get(), &width, &height));
                if (width > 0 && height > 0)
                    output = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
                return output.IsValid() && output != host.extent;
            };
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!pollWindow()) {
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                SDL_PumpEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            for (;;) {
                const auto resized = host.frontend->Resize(output);
                if (resized.HasValue())
                    break;
                const auto &error = resized.ErrorValue();
                INFO("Vulkan / swapchain / realize resized output: " << error.domain.Value() << "/" << error.code.Value() << ": "
                                                                     << error.message);
                REQUIRE(error.code.Value() == RenderPresentationErrors::DrawableUnavailable.code.Value());
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                SDL_PumpEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            if (initialOutput.HasValue()) {
                const auto resizedOutput = host.frontend->PresentationOutput();
                Check(resizedOutput, "observe replacement SDR attachment");
                CHECK(resizedOutput.Value().revision > initialOutput.Value().revision);
                CHECK(resizedOutput.Value().active == initialOutput.Value().active);
            }
            const auto retiredExtent = host.frontend->PollPresentationAvailability(host.extent);
            Check(retiredExtent, "poll the superseded pixel extent");
            CHECK_FALSE(retiredExtent.Value());
            auto resumed = host.Frame(3, output);
            Check(resumed.Execute(passes), "record SDR output at realized pixel extent");
            Check(resumed.Present(), "present the resized SDR output");
        }
        REQUIRE(host.Drain());
        CHECK(host.frontend->MemorySnapshot().allocationCount == 0);
        CHECK(host.port->GetInstanceProcAddr() == nullptr);
        CHECK(SDL_GetWindowID(host.window.get()) != 0);
    }

    TEST_CASE("Native Vulkan manual ColorOutput preserves graph acceptance and publishes the encoded GUI view",
              "[native-smoke][vulkan][gui][manual-color]") {
        const auto *root = std::getenv("HORO_TEST_VULKAN_COLOR_OUTPUT_PACKAGE");
        if (!root)
            SKIP("Supply HORO_TEST_VULKAN_COLOR_OUTPUT_PACKAGE with an actual Vulkan manual ColorOutput generation.");
        Host host;
        const auto presentation = host.frontend->PresentationOutput();
        if (presentation.HasError() && presentation.ErrorValue().code.Value() == RenderPresentationErrors::OutputUnsupported.code.Value())
            SKIP("Selected surface lacks the qualified BGRA8 unmanaged SDR attachment.");
        Check(presentation);

        auto package = Render::Internal::LoadColorOutputCookPackage(std::filesystem::path{root}, ShaderTargetBackend::Vulkan);
        Check(package, "load exact Vulkan manual color package");
        const EditorViewportColorInputs color{
            .package = std::make_shared<const Render::Internal::ColorOutputCookPackage>(std::move(package).Value())};
        host.CreateViewport({}, &color);
        CHECK(host.viewport->ColorDiagnostics().active);
        CHECK_FALSE(host.viewport->ColorDiagnostics().automatic);
        CHECK_FALSE(host.viewport->TextureView().IsValid());
        const auto rejectedSetting = host.viewport->RequestColorSettings({.mode = ExposureMode::AutomaticHistogram});
        REQUIRE(rejectedSetting.HasError());
        CHECK(rejectedSetting.ErrorValue().code.Value() == ExposureGpuErrors::Unsupported.code.Value());
        {
            host.PrepareScene();
            auto frame = host.Frame(1);
            host.ExecuteSceneAndGui(frame);
            CHECK(host.viewport->ColorDiagnostics().acceptedSubmission == 0);
            frame.Cancel();
            host.viewport->FinishColorFrame(false);
            CHECK_FALSE(host.viewport->TextureView().IsValid());
            CHECK(host.viewport->ColorDiagnostics().acceptedSubmission == 0);
        }
        {
            host.viewport->RequestGrid({.visible = true});
            host.PrepareScene();
            auto frame = host.Frame(2);
            host.ExecuteSceneAndGui(frame);
            Check(frame.Present(), "submit scene-linear output and GUI tail");
            host.viewport->FinishColorFrame(false);
            CHECK(host.viewport->ColorDiagnostics().acceptedSubmission == 1);
            CHECK(host.viewport->TextureView().IsValid());
        }
        {
            host.PrepareScene();
            auto frame = host.Frame(3);
            host.ExecuteSceneAndGui(frame);
            Check(frame.Present(), "resample the accepted analytic tables without another upload");
            host.viewport->FinishColorFrame(true);
            CHECK(host.viewport->ColorDiagnostics().acceptedSubmission == 2);
        }
        REQUIRE(host.Drain());
    }

    TEST_CASE("Native Vulkan empty viewport GUI tail and cancellation share canonical retirement", "[native-smoke][vulkan][gui]") {
        Host host;
        host.RecordCapabilities();
        host.CreateViewport();
        {
            host.PrepareScene();
            auto frame = host.Frame(1);
            host.ExecuteSceneAndGui(frame);
            Check(frame.Present());
        }
        {
            host.PrepareScene();
            auto frame = host.Frame(2);
            host.ExecuteSceneAndGui(frame);
            frame.Cancel();
        }
        REQUIRE(host.Drain());
        CHECK(ImGui::GetIO().BackendRendererUserData == nullptr);
        CHECK_FALSE(host.viewport->IsReady());
        CHECK(host.frontend->MemorySnapshot().allocationCount == 0);
        host.viewport.reset();
        host.gui.reset();
        CHECK(ImGui::GetIO().BackendPlatformUserData == nullptr);
        CHECK(SDL_GetWindowID(host.window.get()) != 0);
    }

    TEST_CASE("Native Vulkan grid selected light and mixed lighting retire together across presented and cancelled frames",
              "[native-smoke][vulkan][gui][viewport]") {
        const auto curve = GENERATE(std::optional<Cinematic::CameraBlendCurve>{}, std::optional{Cinematic::CameraBlendCurve::Linear},
                                    std::optional{Cinematic::CameraBlendCurve::SmoothStep});
        std::optional<CameraCuts::NativeCameraPlayback> cinematic;
        if (curve)
            cinematic.emplace(*curve);
        const std::array vertices{MeshVertex{{-1, -1, 0}, {0, 0, 1}, {0, 0}}, MeshVertex{{1, -1, 0}, {0, 0, 1}, {1, 0}},
                                  MeshVertex{{0, 1, 0}, {0, 0, 1}, {0.5F, 1}}};
        const std::array<std::uint32_t, 3> indices{0, 1, 2};
        const RenderMeshSourceHandle source{{1}, 1};
        const std::array meshes{RenderMeshResourceView{source, vertices, indices, {{-1, -1, 0}, {1, 1, 0}}}};
        const std::array instances{RenderStaticMeshInstance{.mesh = source}};
        const std::array lights{RenderLight{.stableIdentity = 1},
                                RenderLight{.kind = RenderLightKind::Point, .position = {2, 3, 4}, .stableIdentity = 2},
                                RenderLight{.kind = RenderLightKind::Spot, .position = {0, 0, 3}, .stableIdentity = 3}};
        RenderSceneView scene{.camera = {.position = {0, 2, 5}, .target = {0, 0, 0}},
                              .meshResources = meshes,
                              .instances = instances,
                              .lights = lights,
                              .lightSceneDomain = 1,
                              .lightSnapshotRevision = 1};
        if (cinematic) {
            scene.camera = cinematic->Advance(0);
            const float expectedInitialZ = *curve == Cinematic::CameraBlendCurve::Linear ? 3.6666667F : 3.5185185F;
            CHECK(scene.camera.position.z == Catch::Approx(expectedInitialZ));
        }
        REQUIRE(scene.IsValid());
        Host host;
        host.RecordCapabilities();
        host.CreateViewport(scene);
        REQUIRE(host.frontend->Capabilities().supportsMaterialRasterGraph);
        CHECK(host.viewport->PrepareLightingFrame(host.ViewportPass(scene), {}).HasError());
        const auto firstTexture = host.viewport->TextureView().sampledView;
        for (std::uint64_t number = 1; number <= 6; ++number) {
            if (number == 2 && cinematic)
                scene.camera = cinematic->Advance(2);
            if (number == 3) {
                const auto previous = host.ViewportPass(scene);
                constexpr EditorViewportExtent resized{384, 256};
                host.viewport->RequestExtent(resized);
                host.SynchronizeViewport(scene, resized);
                CHECK(host.viewport->TextureView().sampledView != firstTexture);
                CHECK(host.viewport->PrepareLightingFrame(previous, host.viewport->LightingViewIdentity()).HasError());
            }
            const bool overlays = number % 2 != 0;
            host.viewport->RequestGrid({.visible = overlays});
            host.viewport->RequestLightVisualizer({overlays ? std::optional{lights[1]} : std::nullopt});
            host.PrepareScene(scene);
            auto frame = host.Frame(number);
            CHECK(host.viewport->PrepareLightingFrame(host.ViewportPass(scene), host.viewport->LightingViewIdentity()).HasError());
            host.ExecuteSceneAndGui(frame, scene);
            if (number % 2 == 0)
                frame.Cancel();
            else
                Check(frame.Present());
        }
        if (cinematic)
            CHECK(cinematic->Position() == 8);
        REQUIRE(host.Drain());
        CHECK_FALSE(host.viewport->IsReady());
        CHECK(host.frontend->MemorySnapshot().allocationCount == 0);
    }
}  // namespace Horo::Tests::VulkanEditor
