/** @file OpenXRRealVulkanReprojectionSmoke.inl
 * @brief Installed OpenXR Play driven by real Vulkan memory, graphics and coverage with scripted runtime ownership.
 */
#include "../renderer/UiSubmissionSourceFixture.h"
#include "OpenXRFrameMetricsTestSupport.h"
#include "RuntimeUiPaintShaderPackage.h"
#include "editor/renderer/vulkan/EditorGuiRendererVulkan.h"
#include "editor/renderer/vulkan/SdlVulkanPresentationPort.h"

#include <SDL3/SDL.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <iterator>

namespace Horo::Tests::OpenXRApplication {
    namespace {
        /** @brief Creates one SDL video owner and GUI context for real presentation. */
        struct NativePlayPlatform final {
            NativePlayPlatform() {
                REQUIRE(SDL_Init(SDL_INIT_VIDEO));
                ImGui::CreateContext();
                ImGui::GetIO().IniFilename = nullptr;
            }

            ~NativePlayPlatform() {
                ImGui::DestroyContext();
                SDL_Quit();
            }
        };

        /** @brief Test runtime allocation authority. The selected renderer and its imports borrow these actual images. */
        class NativePlayImages final {
        public:
            NativePlayImages(Editor::SdlVulkanPresentationPort &platform, RenderNative::VulkanEditorGraphicsBridge &bridge)
                : platform_(platform), bridge_(bridge) {
                REQUIRE(active_ == nullptr);
                active_ = this;
                resolveNativePlayFixture = &Resolve;
            }

            ~NativePlayImages() {
                for (const auto &record : records_)
                    CHECK(record.image == VK_NULL_HANDLE);
                resolveNativePlayFixture = nullptr;
                active_ = nullptr;
            }

            struct Allocation final {
                XrSwapchain swapchain{XR_NULL_HANDLE};
                VkImage image{VK_NULL_HANDLE};
                VkDeviceMemory memory{VK_NULL_HANDLE};
                std::uint32_t releases{};
            };

            std::function<void()> onRelease;

            const Allocation &Read(XrSwapchain swapchain) const {
                const auto found = std::ranges::find(records_, swapchain, &Allocation::swapchain);
                REQUIRE(found != records_.end());
                return *found;
            }

        private:
            Allocation &Current(XrSwapchain swapchain) {
                const auto found = std::ranges::find(records_, swapchain, &Allocation::swapchain);
                REQUIRE(found != records_.end());
                return *found;
            }

            template <typename Function> static Function DeviceProc(const RenderNative::VulkanEditorDevice &device, const char *name) {
                const auto function = std::bit_cast<Function>(device.deviceResolver(device.device, name));
                REQUIRE(function);
                return function;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL SelectPhysical(XrInstance, XrSystemId, VkInstance instance, VkPhysicalDevice *output) {
                const auto resolve = active_->platform_.GetInstanceProcAddr();
                const auto enumerate = std::bit_cast<PFN_vkEnumeratePhysicalDevices>(resolve(instance, "vkEnumeratePhysicalDevices"));
                REQUIRE(enumerate);
                std::array<VkPhysicalDevice, 16> devices{};
                std::uint32_t count = devices.size();
                REQUIRE(enumerate(instance, &count, devices.data()) == VK_SUCCESS);
                REQUIRE(count != 0);
                *output = devices.front();
                return XR_SUCCESS;
            }

            static void SetInitialLayout(const RenderNative::VulkanEditorDevice &device, VkImage image, const VkImageAspectFlags aspect,
                                         const std::uint32_t layers) {
                const auto poolInfo = VkCommandPoolCreateInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                                              .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                                              .queueFamilyIndex = device.graphicsFamily};
                VkCommandPool pool{};
                REQUIRE(DeviceProc<PFN_vkCreateCommandPool>(device, "vkCreateCommandPool")(device.device, &poolInfo, nullptr, &pool) ==
                        VK_SUCCESS);

                struct Pool final {
                    const RenderNative::VulkanEditorDevice &device;
                    VkCommandPool handle;

                    ~Pool() {
                        DeviceProc<PFN_vkDestroyCommandPool>(device, "vkDestroyCommandPool")(device.device, handle, nullptr);
                    }
                } lease{device, pool};

                const VkCommandBufferAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                           .commandPool = pool,
                                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                           .commandBufferCount = 1};
                VkCommandBuffer command{};
                REQUIRE(DeviceProc<PFN_vkAllocateCommandBuffers>(device, "vkAllocateCommandBuffers")(device.device, &allocate, &command) ==
                        VK_SUCCESS);
                const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                     .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
                REQUIRE(DeviceProc<PFN_vkBeginCommandBuffer>(device, "vkBeginCommandBuffer")(command, &begin) == VK_SUCCESS);
                const auto layout = aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                                        : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                                    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                                    .newLayout = layout,
                                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                    .image = image,
                                                    .subresourceRange = {aspect, 0, 1, 0, layers}};
                const VkDependencyInfo dependencies{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                    .imageMemoryBarrierCount = 1,
                                                    .pImageMemoryBarriers = &barrier};
                DeviceProc<PFN_vkCmdPipelineBarrier2>(device, "vkCmdPipelineBarrier2")(command, &dependencies);
                REQUIRE(DeviceProc<PFN_vkEndCommandBuffer>(device, "vkEndCommandBuffer")(command) == VK_SUCCESS);
                const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
                REQUIRE(DeviceProc<PFN_vkQueueSubmit>(device, "vkQueueSubmit")(device.graphicsQueue, 1, &submit, VK_NULL_HANDLE) ==
                        VK_SUCCESS);
                // Cold test-runtime allocation owns this explicit fence. Play continues to poll its own Vulkan queue.
                REQUIRE(DeviceProc<PFN_vkQueueWaitIdle>(device, "vkQueueWaitIdle")(device.graphicsQueue) == VK_SUCCESS);
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Create(XrSession session, const XrSwapchainCreateInfo *info, XrSwapchain *handle) {
                const auto result = OpenXR::SwapchainScript::Create(session, info, handle);
                if (XR_FAILED(result))
                    return result;
                const auto device = RequireValue(active_->bridge_.Port()->QueryDevice());
                const auto depth = (info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
                auto usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                const auto sampled = (info->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) != 0;
                const VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                                  .imageType = VK_IMAGE_TYPE_2D,
                                                  .format = static_cast<VkFormat>(info->format),
                                                  .extent = {info->width, info->height, 1},
                                                  .mipLevels = info->mipCount,
                                                  .arrayLayers = info->arraySize,
                                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                                  .usage =
                                                      static_cast<VkImageUsageFlags>(usage) | (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0U),
                                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
                auto &record = active_->records_.emplace_back(Allocation{*handle});
                REQUIRE(DeviceProc<PFN_vkCreateImage>(device, "vkCreateImage")(device.device, &imageInfo, nullptr, &record.image) ==
                        VK_SUCCESS);
                VkMemoryRequirements requirements{};
                DeviceProc<PFN_vkGetImageMemoryRequirements>(device, "vkGetImageMemoryRequirements")(device.device, record.image,
                                                                                                     &requirements);
                VkPhysicalDeviceMemoryProperties memory{};
                const auto query = std::bit_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
                    device.instanceResolver(device.instance, "vkGetPhysicalDeviceMemoryProperties"));
                REQUIRE(query);
                query(device.physical, &memory);
                std::optional<std::uint32_t> type;
                for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
                    if ((requirements.memoryTypeBits & (1U << i)) &&
                        (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                        type = i;
                        break;
                    }
                REQUIRE(type);
                const VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                      .allocationSize = requirements.size,
                                                      .memoryTypeIndex = *type};
                REQUIRE(DeviceProc<PFN_vkAllocateMemory>(device, "vkAllocateMemory")(device.device, &allocation, nullptr, &record.memory) ==
                        VK_SUCCESS);
                REQUIRE(DeviceProc<PFN_vkBindImageMemory>(device, "vkBindImageMemory")(device.device, record.image, record.memory, 0) ==
                        VK_SUCCESS);
                SetInitialLayout(device, record.image, depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, info->arraySize);
                return XR_SUCCESS;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Enumerate(XrSwapchain handle, std::uint32_t capacity, std::uint32_t *count,
                                                            XrSwapchainImageBaseHeader *images) {
                *count = 1;
                if (capacity) {
                    auto &entry = *reinterpret_cast<XrSwapchainImageVulkanKHR *>(images);
                    REQUIRE(entry.type == XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR);
                    entry.image = active_->Read(handle).image;
                }
                return OpenXR::SwapchainScript::Record("enumerate");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Release(XrSwapchain handle, const XrSwapchainImageReleaseInfo *info) {
                const auto released = OpenXR::SwapchainScript::Release(handle, info);
                if (XR_SUCCEEDED(released)) {
                    ++active_->Current(handle).releases;
                    if (active_->onRelease)
                        active_->onRelease();
                }
                return released;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Destroy(XrSwapchain handle) {
                const auto destroyed = OpenXR::SwapchainScript::Destroy(handle);
                if (XR_FAILED(destroyed))
                    return destroyed;
                const auto device = RequireValue(active_->bridge_.Port()->QueryDevice());
                auto &record = active_->Current(handle);
                DeviceProc<PFN_vkDestroyImage>(device, "vkDestroyImage")(device.device, record.image, nullptr);
                DeviceProc<PFN_vkFreeMemory>(device, "vkFreeMemory")(device.device, record.memory, nullptr);
                record.image = VK_NULL_HANDLE;
                record.memory = VK_NULL_HANDLE;
                return destroyed;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Resolve(XrInstance, const char *name, PFN_xrVoidFunction *output) {
                const std::array routes{std::pair<std::string_view, PFN_xrVoidFunction>{"xrGetVulkanGraphicsDeviceKHR",
                                                                                        std::bit_cast<PFN_xrVoidFunction>(&SelectPhysical)},
                                        std::pair<std::string_view, PFN_xrVoidFunction>{"xrCreateSwapchain",
                                                                                        std::bit_cast<PFN_xrVoidFunction>(&Create)},
                                        std::pair<std::string_view, PFN_xrVoidFunction>{"xrEnumerateSwapchainImages",
                                                                                        std::bit_cast<PFN_xrVoidFunction>(&Enumerate)},
                                        std::pair<std::string_view, PFN_xrVoidFunction>{"xrReleaseSwapchainImage",
                                                                                        std::bit_cast<PFN_xrVoidFunction>(&Release)},
                                        std::pair<std::string_view, PFN_xrVoidFunction>{"xrDestroySwapchain",
                                                                                        std::bit_cast<PFN_xrVoidFunction>(&Destroy)}};
                const auto route = std::ranges::find_if(routes, [name](const auto &entry) {
                    return entry.first == name;
                });
                if (route == routes.end())
                    return XR_ERROR_FUNCTION_UNSUPPORTED;
                *output = route->second;
                return XR_SUCCESS;
            }

            static inline thread_local NativePlayImages *active_{};
            Editor::SdlVulkanPresentationPort &platform_;
            RenderNative::VulkanEditorGraphicsBridge &bridge_;
            std::vector<Allocation> records_;
        };

        /** @brief Authors a fixed planar surface covering the complete two-eye frusta across both native predictions. */
        Editor::SceneObjectId AuthorFullEyeScene(Editor::SceneDocument &document) {
            Editor::EditorHistory history;
            Editor::SceneDocumentCommandExecutor commands{document, history};
            const auto root = RequireValue(commands.Execute(Editor::CreateSceneObjectCommand{.name = "XR reference"})).object;
            RequireValue(commands.Execute(Editor::CreateSceneObjectCommand{
                .name = "Full-eye surface",
                .parent = root,
                .localTransform = Math::Transform{.translation = {0.0F, 0.0F, -5.0F}, .scale = {40.0F, 40.0F, 1.0F}},
                .primitiveMesh = Runtime::PrimitiveMeshDescriptor{},
            }));
            return root;
        }

        std::vector<std::uint8_t> ReadNativeCook(const std::filesystem::path &path) {
            std::ifstream stream{path, std::ios::binary};
            REQUIRE(stream.good());
            return {std::istreambuf_iterator<char>{stream}, {}};
        }

        template <typename Loader> auto LoadNativeCook(const std::filesystem::path &root, const char *stem, Loader load) {
            const auto prefix = root / stem;
            return RequireValue(load(ReadNativeCook(prefix.string() + ".horoshdr"), ReadNativeCook(prefix.string() + ".reflection.json")));
        }

        struct NativeComfortProjection final {
            std::filesystem::path shaders;
            XR::OpenXRInternal::NativeFrameMetricHandles metrics;
        };

        /** @brief Signs the exact smooth-movement action used by the native comfort fixture. */
        std::string ComfortBindingDocument() {
            auto document = OpenXRProduct::Json::parse(OpenXRProduct::InstalledProductFixture::BindingDocument());
            document["locomotion"] = {{"actor", 17}, {"speedMetersPerSecond", 2.5}, {"moveAction", "select"}, {"deadZone", 0.1}};
            const auto axis = static_cast<unsigned>(Input::ActionValueType::Axis2D);
            document["actions"][0]["type"] = axis;
            document["controls"][0]["type"] = axis;
            document["controls"][0]["path"] = "/user/hand/left/input/thumbstick";
            document["poses"].push_back({{"profile", 1},
                                         {"set", 1},
                                         {"role", static_cast<unsigned>(XR::XRTrackedDeviceRole::LeftController)},
                                         {"kind", static_cast<unsigned>(XR::XRTrackedPoseKind::Aim)},
                                         {"path", "/user/hand/left/input/aim/pose"},
                                         {"label", "Aim"}});
            return document.dump();
        }

        /** @brief Exact signed Play composition with real SDL WSI, production viewport and real native graphics queues. */
        struct NativePlayFixture final {
            ApplicationNativeScript script;
            OpenXRProduct::InstalledProductFixture product;
            ConfigurationService configuration{XrSchema()};
            NativePlayPlatform video;
            const Render::FramebufferExtent extent{160, 120};
            std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{SDL_CreateWindow("Horo XR Vulkan qualification", extent.width,
                                                                                              extent.height, SDL_WINDOW_VULKAN),
                                                                             &SDL_DestroyWindow};
            std::unique_ptr<Editor::SdlVulkanPresentationPort> platform;
            RenderNative::VulkanEditorGraphicsBridge bridge;
            std::unique_ptr<NativePlayImages> images;
            DeterministicClock schedulerClock;
            std::unique_ptr<Runtime::RuntimeHost> scheduler{
                RequireValue(Runtime::RuntimeHost::Create(schedulerClock, {.fixedStep = Duration::FromMilliseconds(10)}))};
            Editor::SceneDocument document;
            const Editor::SceneObjectId root{AuthorFullEyeScene(document)};
            Runtime::PrimitiveMeshCache primitives;
            Editor::EditorAssetMeshCache assets;
            Editor::EditorXrSceneSourceOwner source{document, primitives, assets};
            const Runtime::RuntimeSceneDefinition definition{
                RequireValue(Editor::ConvertSceneDocumentToRuntime(document.Snapshot(), Runtime::SceneDefinitionId{1}))};
            std::unique_ptr<Runtime::RuntimeScene> scene{RequireValue(Runtime::RuntimeScene::Create(definition, {1}))};
            Input::InputRouter router;
            Input::InputContextToken inputContext;
            std::shared_ptr<const Host::OpenXRApplicationBindings> bindings{product.ReadBindings()};
            std::unique_ptr<Host::OpenXRApplicationSession> session;
            Render::RenderBackendRegistry renderers;
            std::unique_ptr<Render::RenderFrontend> frontend;
            std::unique_ptr<Editor::EditorViewportRendererVulkan> viewport;
            std::unique_ptr<Editor::EditorGuiRendererVulkan> gui;
            std::unique_ptr<Host::OpenXRPlayHost> play;

            explicit NativePlayFixture(const std::filesystem::path &cooks, const Host::IOpenXRPlayLightingSource *lighting = nullptr,
                                       const std::filesystem::path &lightingCooks = {},
                                       std::shared_ptr<const Host::IOpenXRLightPermission> lightPermission = {},
                                       const NativeComfortProjection *comfort = nullptr)
                : product{HORO_TEST_OPENXR_LOADER, comfort ? std::optional{ComfortBindingDocument()} : std::nullopt} {
                REQUIRE(window);
                platform = std::make_unique<Editor::SdlVulkanPresentationPort>(*window);
                images = std::make_unique<NativePlayImages>(*platform, bridge);
                script.native.synthesisExtension = true;
                ++script.native.extensionCount;
                if (lightPermission) {
                    static constexpr std::array<std::string_view, 1> extensions{XR_ANDROID_LIGHT_ESTIMATION_EXTENSION_NAME};
                    script.native.extraExtensions = extensions;
                    ++script.native.extensionCount;
                    script.census.extraProc = &OpenXR::LightScript::GetProc;
                    script.census.lightEstimationSupport = XR_TRUE;
                    product.policy->lightPermission = std::move(lightPermission);
                }
                static constexpr std::array<std::int64_t, 3> formats{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                                     VK_FORMAT_D32_SFLOAT};
                script.census.formats = formats.size();
                script.census.formatPublication = formats;
                script.swapchain.imageCount = 1;
                ConfigurationResolutionRequest request;
                request.invocation.emplace(SettingKey{"xr.enabled"}, ConfigurationInputValue{true, std::nullopt});
                request.invocation.emplace(SettingKey{"xr.limits.spaces"},
                                           ConfigurationInputValue{std::int64_t{comfort ? 6 : 5}, std::nullopt});
                request.invocation.emplace(SettingKey{"xr.features.space_warp"},
                                           ConfigurationInputValue{static_cast<std::int64_t>(
                                                                       XR::XRProjectFeaturePolicy::OptionalProjection),
                                                                   std::nullopt});
                if (comfort) {
                    request.invocation.emplace(SettingKey{"xr.locomotion.maximum_vignette_percent"},
                                               ConfigurationInputValue{std::int64_t{40}, std::nullopt});
                    script.actions.tracking.enabled = true;
                    script.actions.sample.expectedType = XR_ACTION_TYPE_VECTOR2F_INPUT;
                }
                RequireValue(configuration.ResolveAndCommit(request));
                session =
                    std::make_unique<Host::OpenXRApplicationSession>(Host::OpenXRApplicationAuthorities{configuration, *product.host,
                                                                                                        product.Root(),
                                                                                                        product.host->Generation(),
                                                                                                        [] {
                    return Result<void>::Success();
                }},
                                                                     RequireValue(XR::ResolveXRProjectSettings(configuration.Snapshot())),
                                                                     product.Candidate(), *product.policy);
                RequireValue(session->PrepareDiscovery());
                RequireValue(session->AttachGraphics(*platform, bridge));
                RequireValue(Render::RegisterVulkanRenderBackend(renderers, session->LoaderPort(), &bridge));
                RequireValue(renderers.Seal());
                Render::RenderBackendConfig config{.requirePresentation = true, .enableValidation = true, .maxFramesInFlight = 2};
                const auto graphics =
                    std::ranges::find(config.queueAssignments, Render::RenderQueueRole::Graphics, &Render::RenderQueueAssignment::role);
                REQUIRE(graphics != config.queueAssignments.end());
                config.queueAssignments.push_back({Render::RenderQueueRole::Compute, graphics->queue});
                frontend = RequireValue(Render::RenderFrontend::Create(renderers, Render::RenderBackendId{"vulkan"}, config, {},
                                                                       {.budget = {.maximumBlocks = 4096}}));
                RequireValue(frontend->Resize(extent));
                auto base = LoadNativeCook(cooks, "viewport", Editor::LoadEditorVulkanViewportCook);
                base.shadow = LoadNativeCook(cooks, "shadow", Editor::LoadEditorVulkanShadowCook);
                viewport = std::make_unique<Editor::EditorViewportRendererVulkan>(*frontend, bridge, std::move(base));
                RequireValue(viewport->Initialize());
                if (lightingCooks.empty()) {
                    RequireValue(viewport->PrepareXrProgram(LoadNativeCook(cooks, "multiview", Editor::LoadEditorVulkanMultiviewCook)));
                } else {
                    RequireValue(viewport->PrepareXrProgram(LoadNativeCook(lightingCooks, "multiview-environment-vulkan",
                                                                           Editor::LoadEditorVulkanMultiviewEnvironmentCook),
                                                            Editor::EditorVulkanMultiviewProgram::EnvironmentLighting));
                }
                if (comfort)
                    RequireValue(viewport->PrepareXrComfortProgram(
                        LoadNativeCook(comfort->shaders, "xr-comfort-vulkan", Editor::LoadEditorVulkanXrComfortCook)));
                viewport->StageXrMotionProgram(LoadNativeCook(cooks, "motion", Editor::LoadEditorVulkanMultiviewMotionCook));
                viewport->StageXrCoverageProgram(LoadNativeCook(cooks, "coverage", Editor::LoadEditorVulkanMultiviewCoverageCook));
                viewport->StageXrCoverageReductionProgram(
                    LoadNativeCook(cooks, "reduction", Editor::LoadEditorVulkanCoverageReductionCook));
                gui = std::make_unique<Editor::EditorGuiRendererVulkan>(*window, *frontend, bridge);
                RequireValue(gui->Initialize());
                RequireValue(session->PrepareSession());
                play = RequireValue(
                    Host::OpenXRPlayHost::Create({*session,
                                                  router,
                                                  *viewport,
                                                  *frontend,
                                                  bindings,
                                                  scheduler->DispatchSource(),
                                                  Duration::FromMilliseconds(10),
                                                  {root.value},
                                                  lighting,
                                                  comfort ? comfort->metrics : XR::OpenXRInternal::NativeFrameMetricHandles{}}));
            }

            ~NativePlayFixture() {
                if (play) {
                    if (play->ProgressRenderTail().HasError())
                        std::terminate();
                    const auto close = play->Close();
                    if (close.HasError())
                        std::terminate();
                    if (!close.Value()) {
                        script.events.Push(XR_SESSION_STATE_STOPPING);
                        script.events.Push(XR_SESSION_STATE_IDLE);
                        const auto stopped = play->Close();
                        if (stopped.HasError() || !stopped.Value())
                            std::terminate();
                    }
                    play.reset();
                }
                if (session && session->Close().HasError())
                    std::terminate();
                if (frontend) {
                    if (frontend->BeginShutdown().HasError())
                        std::terminate();
                    Wait([&] {
                        const auto drained = frontend->PollDrain();
                        if (drained.HasError())
                            std::terminate();
                        return drained.Value() == Render::RenderBackendDrainProgress::Drained;
                    });
                }
            }

            Input::InputActionCollection InputPort(const std::optional<Input::PlayerId> player) {
                std::vector registered{Input::ActionDescriptor{Input::ActionId{"select"},
                                                               bindings->schema.actions.front().valueType,
                                                               Input::InputContextId{"gameplay"},
                                                               true,
                                                               {{.kind = Input::BindingControlKind::Key, .key = Input::Key::Space}}}};
                RequireValue(router.SetActionMap(std::move(registered)));
                inputContext = router.PushContext(Input::InputContextId{"gameplay"}, Input::InputContextKind::Gameplay);
                return RequireValue(player ? router.ActionCollectionForPlayer(inputContext, *player)
                                           : router.ActionCollection(inputContext));
            }

            /** @brief Bounds explicit test observation without introducing a normal-frame GPU wait into Play. */
            template <typename Predicate> static void Wait(Predicate done) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
                while (!done()) {
                    REQUIRE(std::chrono::steady_clock::now() < deadline);
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }

            struct PublicationHooks final {
                std::function<void()> beforePresent;
                std::function<void()> afterPresent;
            };

            void Project(const std::uint64_t number, const Host::OpenXRPlayHost::WorldCanvasFrame *canvas = nullptr,
                         const Editor::EditorVulkanXrViewportAccess::CanvasSource *submitCanvas = nullptr,
                         const PublicationHooks &hooks = {}, const Runtime::FrameContext *dispatch = nullptr) {
                CAPTURE(number);
                const auto step = [](std::string_view name, auto result) {
                    INFO(name);
                    return RequireValue(std::move(result));
                };
                Wait([&] {
                    SDL_PumpEvents();
                    return step("poll native WSI availability", frontend->PollPresentationAvailability(extent));
                });
                REQUIRE(step("prepare native prediction",
                             dispatch ? play->PrepareRenderFrame(*dispatch, canvas) : play->PrepareRenderFrame(number, canvas)));
                {
                    auto frame = step("begin Vulkan WSI frame", frontend->BeginFrame({number, extent}));
                    step("execute native Play graph", play->ExecuteRenderFrame(frame, {}, submitCanvas));
                    const auto mirror = play->MirrorView(number);
                    REQUIRE(mirror.IsValid());
                    step("begin GUI", gui->BeginFrame());
                    ImGui::NewFrame();
                    gui->BindViewportTexture(mirror);
                    ImGui::GetForegroundDrawList()->AddImage(static_cast<ImTextureID>(mirror.textureId), {0, 0}, {80, 60});
                    ImGui::Render();
                    step("record Vulkan GUI tail", frame.ComposeGui(*gui));
                    step("seal Play GUI phase", play->FinishGui(true));
                    if (hooks.beforePresent)
                        hooks.beforePresent();
                    if (step("recheck Play before submission", play->RevalidateRenderLighting(number))) {
                        step("submit Vulkan WSI frame", frame.Present());
                        if (hooks.afterPresent)
                            hooks.afterPresent();
                    }
                }
                Wait([&] {
                    step("poll native compositor tail", play->ProgressRenderTail());
                    return script.frames.ends == number;
                });
            }
        };

        /** @brief Extracts one resource-free authored panel for the native scene canvas writer. */
        Runtime::Ui::UiRenderSnapshot MakeNativeWorldCanvas() {
            using namespace Runtime::Ui;
            using Render::Test::UiOwner;
            const auto tree = Render::Test::UiTree();
            const UiRenderViewId view{UiOwner(), 9, 1};
            const UiRenderSnapshotLimits limits{1, 0, 0, 0, 0, 1, 0};
            auto extractor = RequireValue(UiRenderExtractor::Create({view, limits, 1}));
            const auto descriptor = Runtime::Ui::Test::SnapshotDescriptor(tree, RequireValue(UiInteractionRevision::Create(1)),
                                                                          RequireValue(UiRenderSnapshotRevision::Create(1)), view, limits);
            const std::array commands{UiDrawCommand{RequireValue(tree.Root()).handle,
                                                    {{0, 0}, {4096, 2048}},
                                                    0,
                                                    NoUiRenderIndex,
                                                    NoUiRenderIndex,
                                                    1.0F,
                                                    UiSolidDraw{{0, 1, 0, 1}}}};
            const std::array transforms{UiLogicalTransform{}};
            return RequireValue(extractor.Extract(tree, descriptor, {.commands = commands, .transforms = transforms}));
        }
    }  // namespace

    TEST_CASE("Signed Play submits true Vulkan full-eye reduction to its matching native synthesis End",
              "[xr][application][play][reprojection][native-positive]") {
        const char *root = std::getenv("HORO_XR_PLAY_NATIVE_COOK_ROOT");
        if (!root)
            SKIP("The real Play case needs the locked canonical Vulkan viewport, shadow and four XR cooks.");
        NativePlayFixture fixture{root};
        auto scene = ActivatePlay(fixture, std::nullopt);
        fixture.script.clock.sample = 1;
        fixture.Project(1);
        REQUIRE(fixture.script.frames.ext.front().type == XR_TYPE_UNKNOWN);
        const auto nativeColor = fixture.script.frames.encoded.front().subImage.swapchain;
        REQUIRE(fixture.images->Read(nativeColor).releases == 1);
        fixture.script.frames.prediction += fixture.script.frames.period;

        fixture.Project(2);
        const auto &native = fixture.script.frames;
        REQUIRE(native.layers == 1);
        REQUIRE(native.views == 2);
        REQUIRE(fixture.images->Read(nativeColor).releases == 2);
        for (std::size_t eye = 0; eye < native.ext.size(); ++eye) {
            const auto &chain = native.ext[eye];
            REQUIRE(chain.type == XR_TYPE_FRAME_SYNTHESIS_INFO_EXT);
            REQUIRE(chain.motionVectorSubImage.imageArrayIndex == eye);
            REQUIRE(chain.depthSubImage.imageArrayIndex == eye);
            REQUIRE(chain.motionVectorSubImage.imageRect.extent.width ==
                    640 + static_cast<std::int32_t>(eye * fixture.script.census.synthesisEyeWidthStep));
            REQUIRE(chain.motionVectorSubImage.imageRect.extent.height == 512);
            const auto &motion = fixture.images->Read(chain.motionVectorSubImage.swapchain);
            const auto &depth = fixture.images->Read(chain.depthSubImage.swapchain);
            REQUIRE(motion.image != depth.image);
            REQUIRE(motion.image != fixture.images->Read(nativeColor).image);
            REQUIRE(depth.image != fixture.images->Read(nativeColor).image);
            REQUIRE(motion.releases == 1);
            REQUIRE(depth.releases == 1);
            REQUIRE(chain.appSpaceDeltaPose.position.x == 0.0F);
            REQUIRE(chain.appSpaceDeltaPose.orientation.w == 1.0F);
        }
        scene->Shutdown();
    }

#if HORO_ENABLE_TELEMETRY
    TEST_CASE("Signed Vulkan Play submits the comfort shader over the two acquired eyes", "[xr][application][play][native-comfort]") {
        using namespace Character::TestDetail;
        const auto *root = SDL_getenv("HORO_XR_PLAY_NATIVE_COOK_ROOT");
        const auto *shaderDirectory = SDL_getenv("HORO_XR_COMFORT_SHADER_DIRECTORY");
        if (!root || !shaderDirectory)
            SKIP("Supply the genuine offline Vulkan projection and comfort programs.");

        OpenXR::FrameMetricRecorder metrics;
        metrics.Start(4096);
        const NativeComfortProjection comfort{shaderDirectory, metrics.handles};

        NativePlayFixture fixture{root, nullptr, {}, {}, &comfort};
        auto scene = ActivatePlay(fixture, Input::PlayerId{7});
        auto character = SpawnedActiveWorldWithController();
        const auto runtime = fixture.scene->View().RuntimeId();
        auto input = std::make_unique<AppInputSample>(*fixture.play);
        const auto *observed = input.get();
        input->onFixed = [&](const Runtime::FixedStepContext &step) {
            RequireValue(fixture.play->TickAuthoredCharacter(runtime, character.controller, *character.world, step.simulationTick, step));
            RequireValue(character.world->AdvanceFixedTick(FixedTick(step.simulationTick)));
        };
        input->onCommit = [&](const Runtime::FixedStepContext &step) noexcept {
            fixture.play->CommitAuthoredCharacter(runtime, *character.world, step);
        };
        input->onRender = [&](const Runtime::FrameContext &frame) {
            fixture.Project(frame.frameNumber, nullptr, nullptr, {}, &frame);
            CHECK(fixture.script.frames.layers == 1);
            REQUIRE(fixture.script.frames.views == 2);
        };
        RequireValue(fixture.scheduler->AddParticipant(std::move(input)));
        RequireValue(fixture.scheduler->Startup());
        fixture.script.clock.sample = 1;
        const auto frame = [&] {
            RequireValue(fixture.scheduler->RunFrame());
            REQUIRE(observed->result);
            RequireValue(*observed->result);
            fixture.script.frames.prediction += fixture.script.frames.period;
            fixture.script.clock.sample += 1'000'000;
            fixture.schedulerClock.Advance(Duration::FromMilliseconds(10));
        };
        frame();
        frame();
        const auto before = RequireValue(character.world->ControllerTransform(character.controller));
        fixture.script.actions.sample.vector = {0.0F, 1.0F};
        frame();
        const auto axis = fixture.router.ReadAction(fixture.inputContext, Input::ActionId{"select"}, Input::PlayerId{7});
        CHECK(axis.y == 1.0F);
        const auto moved = RequireValue(character.world->ControllerTransform(character.controller));
        CHECK(moved.position.z < before.position.z);
        fixture.script.actions.sample.vector = {};
        frame();
        fixture.scheduler->Shutdown();
        scene->Shutdown();
        metrics.Finish();
        CHECK(metrics.sink->Total("horo.xr.projection_comfort", 0) == 3);
        CHECK(metrics.sink->Total("horo.xr.projection_comfort", 1) == 1);
    }
#endif

    TEST_CASE("Signed Vulkan Play reads optional lighting only for the selected program and current frame",
              "[xr][application][play][native-lighting]") {
        struct Source final : Host::IOpenXRPlayLightingSource {
            [[nodiscard]] std::optional<Host::OpenXRPlayLightingSample> Read(
                XR::XRFrameId frame, const XR::XRCoordinateSpace &world, XR::XRRenderPredictionTime prediction) const noexcept override {
                ++reads;
                if (withdrawn)
                    return std::nullopt;
                const auto grant = XR::XRLightEstimateGrant::Create(1);
                if (grant.HasError())
                    return std::nullopt;
                return Host::OpenXRPlayLightingSample{{frame.session, world, grant.Value()},
                                                      frame,
                                                      prediction,
                                                      {.ambient = Math::Vec3{0.25F, 0.5F, 0.75F}}};
            }

            mutable std::uint32_t reads{};
            bool withdrawn{};
        } lighting;

        const auto *root = SDL_getenv("HORO_XR_PLAY_NATIVE_COOK_ROOT");
        const auto *environment = SDL_getenv("HORO_XR_ENVIRONMENT_SHADER_DIRECTORY");
        if (!root || !environment)
            SKIP("Supply the genuine offline Vulkan projection and environment programs.");
        const bool supported = GENERATE(false, true);
        NativePlayFixture fixture{root, &lighting, supported ? std::filesystem::path{environment} : std::filesystem::path{}};
        auto scene = ActivatePlay(fixture, std::nullopt);
        fixture.script.clock.sample = 1;
        fixture.Project(1);
        CHECK(lighting.reads == (supported ? 1U : 0U));
        lighting.withdrawn = true;
        fixture.script.frames.prediction += fixture.script.frames.period;
        fixture.Project(2);
        CHECK(lighting.reads == (supported ? 2U : 0U));
        REQUIRE(fixture.script.frames.views == 2);
        scene->Shutdown();
    }

    TEST_CASE("Signed Vulkan Play samples the native estimator only under current platform and application approval",
              "[xr][application][play][native-estimator]") {
        struct Permission final : Host::IOpenXRLightPermission {
            bool SupportsLightEstimation() const noexcept override {
                return supported;
            }

            bool Allows(const XR::XRLightEstimateScope &) const noexcept override {
                return allowed;
            }

            bool supported{true};
            bool allowed{};
        };

        const auto *root = SDL_getenv("HORO_XR_PLAY_NATIVE_COOK_ROOT");
        const auto *environment = SDL_getenv("HORO_XR_ENVIRONMENT_SHADER_DIRECTORY");
        if (!root || !environment)
            SKIP("Supply the genuine offline Vulkan projection and environment programs.");
        OpenXR::LightScript nativeLight;
        nativeLight.sampleAtCutoff = true;
        nativeLight.expectedSpace = XR_NULL_HANDLE;
        auto permission = std::make_shared<Permission>();
        NativePlayFixture fixture{root, nullptr, environment, permission};
        auto scene = ActivatePlay(fixture, std::nullopt);
        auto input = std::make_unique<AppInputSample>(*fixture.play);
        auto *observed = input.get();
        RequireValue(fixture.scheduler->AddParticipant(std::move(input)));
        RequireValue(fixture.scheduler->Startup());
        for (const auto now : {100'000'000, 110'000'000}) {
            fixture.script.clock.sample = now;
            RequireValue(fixture.scheduler->RunFrame());
            REQUIRE(observed->result);
            RequireValue(*observed->result);
        }
        const auto session = fixture.session->RetainedSession();
        REQUIRE(session);
        const auto world = RequireValue(fixture.session->WorldSpace(*session));
        const XR::XRLightEstimateScope scope{*session, world, RequireValue(XR::XRLightEstimateGrant::Create(1))};
        const XR::XRLightEstimateLimits limits{Duration::FromMilliseconds(1), Duration::FromMilliseconds(100)};
        const XR::XRLightProjectionPolicy policy{.ambientColor = {Render::SceneColorEncoding::LinearAcesCg},
                                                 .directionalColor = {Render::SceneColorEncoding::LinearAcesCg},
                                                 .direction = XR::XRLightDirectionConvention::TowardScene,
                                                 .ambientScale = 0.25F,
                                                 .directionalScale = 0.25F,
                                                 .maximumAbsoluteChannel = 4.0F,
                                                 .directionalIdentity = 1,
                                                 .maximumAge = limits.maximumAge};
        CHECK(fixture.play->StartNativeLighting(scope, limits, policy).HasError());
        CHECK(nativeLight.createCalls == 0);
        permission->allowed = true;
        RequireValue(fixture.play->StartNativeLighting(scope, limits, policy));
        fixture.script.frames.prediction = 120'000'000;
        SECTION("The platform withdraws capability while the grant remains allowed") {
            fixture.Project(1);
            permission->supported = false;
            CHECK_FALSE(fixture.session->AllowsNativeLighting(scope));
        }
        SECTION("The platform withdraws the grant while capability remains available") {
            fixture.Project(1);
            permission->allowed = false;
        }
        SECTION("Capability withdrawal inside the native sample retires that activation") {
            nativeLight.onSample = [&] {
                permission->supported = false;
            };
            fixture.Project(1);
            nativeLight.onSample = {};
            permission->supported = true;
            REQUIRE(fixture.session->AllowsNativeLighting(scope));
        }
        SECTION("A revoked encoded frame is canceled before the GPU submission") {
            fixture.Project(1, nullptr, nullptr, {.beforePresent = [&] {
                permission->allowed = false;
            }});
            CHECK(fixture.script.frames.layers == 0);
            permission->allowed = true;
        }
        SECTION("A revoked submitted frame closes with zero headset layers") {
            fixture.Project(1, nullptr, nullptr, {.afterPresent = [&] {
                permission->supported = false;
            }});
            CHECK(fixture.script.frames.layers == 0);
            permission->supported = true;
        }
        SECTION("Pausing Play withdraws lighting from the encoded frame and subsequent frames") {
            fixture.Project(1, nullptr, nullptr, {.beforePresent = [&] {
                RequireValue(fixture.play->SetPlayState(Editor::EditorPlayState::Paused));
            }});
            CHECK(fixture.script.frames.layers == 0);
            RequireValue(fixture.play->SetPlayState(Editor::EditorPlayState::Playing));
        }
        SECTION("Revocation during physical image release prevents the final projection") {
            fixture.images->onRelease = [&] {
                permission->allowed = false;
            };
            fixture.Project(1);
            CHECK(fixture.script.frames.layers == 0);
            fixture.images->onRelease = {};
            permission->allowed = true;
        }
        REQUIRE(nativeLight.sampleCalls == 1);
        fixture.script.clock.sample = fixture.script.frames.prediction;
        // Advance the native prediction with one full period reserved for the next frame.
        fixture.script.frames.prediction += fixture.script.frames.period;
        RequireValue(fixture.scheduler->RunFrame());
        fixture.Project(2);
        CHECK(nativeLight.sampleCalls == 1);
        RequireValue(fixture.play->StopNativeLighting());
        CHECK(nativeLight.retireCalls == 1);
        scene->Shutdown();
    }

    TEST_CASE("Signed Vulkan Play projects a world canvas and rebuilds synthesis history after it leaves",
              "[xr][application][play][reprojection][native-positive][canvas]") {
        using namespace Render;
        using namespace Runtime::Ui;
        const auto *root = std::getenv("HORO_XR_PLAY_NATIVE_COOK_ROOT");
        const auto *uiRoot = std::getenv("HORO_VULKAN_UI_PAINT_ASSET_ROOT");
        if (!root || !uiRoot)
            SKIP("Supply the locked XR and canvas shader packages.");
        const std::filesystem::path shaders = std::filesystem::path{uiRoot} / "shaders";
        const auto writer =
            RequireValue(Host::DecodeProjectUiPaintVulkanScenePackage(ReadNativeCook(shaders / "runtime/ui-paint-vulkan.horoshdr"),
                                                                      ReadNativeCook(shaders / "runtime/ui-paint-vulkan.reflection.json"),
                                                                      ShaderTargetBackend::Vulkan));
        const auto consumer = LoadNativeCook(shaders / "editor", "viewport-world-canvas-vulkan", Editor::LoadEditorVulkanWorldCanvasCook);
        const auto snapshot = MakeNativeWorldCanvas();
        const UiResolvedScreenCanvas raster{.logicalExtent = {4096, 2048}, .pixelExtent = {64, 32}, .pixelsPerDip = {1, 1}};
        auto arena = RequireValue(UiRenderGeometryArena::Create({snapshot.Descriptor().view, {4, 6, 1}, 1}));
        const auto geometry = RequireValue(arena.Build(snapshot, raster));
        const UiPaintProgramDescriptor paint{raster, {32, 64, 4}, 1, 32};
        auto transform = Math::Mat4::Identity();
        transform.values[14] = -2.0F;
        const Host::OpenXRPlayHost::WorldCanvasFrame canvas{{paint, snapshot, {}, *writer, consumer}, transform};
        unsigned submissions{};
        const Editor::EditorVulkanXrViewportAccess::CanvasSource submit = [&geometry, &submissions](const auto &, std::uint32_t slot) {
            CHECK(slot == 0);
            ++submissions;
            return Result<UiRenderSubmission>::Success({.geometry = &geometry});
        };

        NativePlayFixture fixture{root};
        auto scene = ActivatePlay(fixture, std::nullopt);
        fixture.script.clock.sample = 1;
        fixture.Project(1);
        fixture.script.frames.prediction += fixture.script.frames.period;
        fixture.Project(2);
        const auto &projection = fixture.script.frames;
        const auto synthesis = projection.ext.front();
        REQUIRE(synthesis.type == XR_TYPE_FRAME_SYNTHESIS_INFO_EXT);
        const auto nativeColor = projection.encoded.front().subImage.swapchain;
        const auto nativeMotion = synthesis.motionVectorSubImage.swapchain;
        const auto nativeDepth = synthesis.depthSubImage.swapchain;
        fixture.script.frames.prediction += fixture.script.frames.period;
        fixture.Project(3, &canvas, &submit);
        CHECK(submissions == 1);
        REQUIRE(projection.layers == 1);
        REQUIRE(projection.views == 2);
        REQUIRE(projection.ext.front().type == XR_TYPE_UNKNOWN);
        CHECK(fixture.images->Read(nativeColor).releases == 3);
        CHECK(fixture.images->Read(nativeMotion).releases == 1);
        CHECK(fixture.images->Read(nativeDepth).releases == 1);
        const auto completed = fixture.play->TakeCanvasCompletion();
        REQUIRE(completed);
        CHECK(completed->frameNumber == 3);
        CHECK(completed->presented);

        fixture.script.frames.prediction += fixture.script.frames.period;
        fixture.Project(4);
        REQUIRE(projection.ext.front().type == XR_TYPE_UNKNOWN);
        fixture.script.frames.prediction += fixture.script.frames.period;
        fixture.Project(5);
        REQUIRE(projection.ext.front().type == XR_TYPE_FRAME_SYNTHESIS_INFO_EXT);
        scene->Shutdown();
    }
}  // namespace Horo::Tests::OpenXRApplication
