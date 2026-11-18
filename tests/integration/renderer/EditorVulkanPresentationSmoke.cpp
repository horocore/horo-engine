/** @file EditorVulkanPresentationSmoke.cpp
 * @brief Real SDL/Vulkan GUI and cooked viewport composition; no simulated native admission. */
#include "CameraCutNativeTestSupport.h"
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

        void CreateViewport(const RenderSceneView &scene = {}) {
            const char *root = std::getenv("HORO_EDITOR_VULKAN_ASSET_ROOT");
            REQUIRE(root);
            auto cooked = LoadPackagedVulkanViewportShader(std::filesystem::path{root});
            Check(cooked);
            viewport = std::make_unique<EditorViewportRendererVulkan>(*frontend, *bridge, std::move(cooked).Value());
            Check(viewport->Initialize());
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
            for (std::size_t attempt = 0; attempt < 8; ++attempt) {
                const auto prepared = viewport->PrepareResources(*frontend, scene);
                Check(prepared);
                Check(frontend->ProcessResourceRequests());
                if (prepared.Value().has_value()) {
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

        void ExecuteSceneAndGui(RenderFrameScope &frame, const RenderSceneView &scene = {}) {
            const auto view = viewport->TextureView();
            REQUIRE(view.IsValid());
            const auto size = viewport->RequestedExtent();
            const std::array passes{RenderPassDescriptor{.kind = RenderPassKind::Graphics,
                                                         .staticMesh = StaticMeshPassDescriptor{.target = target,
                                                                                                .extent = {size.width, size.height},
                                                                                                .scene = scene}},
                                    RenderPassDescriptor{.kind = RenderPassKind::Graphics, .primaryOutput = PrimaryOutputAttachment{}}};
            Check(viewport->ExecutePreparedFrame(frame, passes));
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

    TEST_CASE("Native Vulkan empty viewport GUI tail and cancellation share canonical retirement", "[native-smoke][vulkan][gui]") {
        Host host;
        host.RecordCapabilities();
        host.CreateViewport();
        {
            auto frame = host.Frame(1);
            host.ExecuteSceneAndGui(frame);
            Check(frame.Present());
        }
        {
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
        host.viewport->RequestGrid({.visible = true});
        host.viewport->RequestLightVisualizer({lights[1]});
        for (std::uint64_t number = 1; number <= 6; ++number) {
            if (number == 2 && cinematic)
                scene.camera = cinematic->Advance(2);
            auto frame = host.Frame(number);
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
