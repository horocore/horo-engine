/** @file OpenXRRealVulkanReprojectionSmoke.inl
 * @brief Installed OpenXR Play driven by real Vulkan memory, graphics and coverage with scripted runtime ownership.
 */
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
                if (XR_SUCCEEDED(released))
                    ++active_->Current(handle).releases;
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

        /** @brief Exact signed Play composition with real SDL WSI, production viewport and real native graphics queues. */
        struct NativePlayFixture final {
            ApplicationNativeScript script;
            OpenXRProduct::InstalledProductFixture product{HORO_TEST_OPENXR_LOADER, std::nullopt};
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

            explicit NativePlayFixture(const std::filesystem::path &cooks) {
                REQUIRE(window);
                platform = std::make_unique<Editor::SdlVulkanPresentationPort>(*window);
                images = std::make_unique<NativePlayImages>(*platform, bridge);
                script.native.synthesisExtension = true;
                ++script.native.extensionCount;
                static constexpr std::array<std::int64_t, 3> formats{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                                     VK_FORMAT_D32_SFLOAT};
                script.census.formats = formats.size();
                script.census.formatPublication = formats;
                script.swapchain.imageCount = 1;
                ConfigurationResolutionRequest request;
                request.invocation.emplace(SettingKey{"xr.enabled"}, ConfigurationInputValue{true, std::nullopt});
                request.invocation.emplace(SettingKey{"xr.limits.spaces"}, ConfigurationInputValue{std::int64_t{5}, std::nullopt});
                request.invocation.emplace(SettingKey{"xr.features.space_warp"},
                                           ConfigurationInputValue{static_cast<std::int64_t>(
                                                                       XR::XRProjectFeaturePolicy::OptionalProjection),
                                                                   std::nullopt});
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
                RequireValue(viewport->PrepareXrProgram(LoadNativeCook(cooks, "multiview", Editor::LoadEditorVulkanMultiviewCook)));
                viewport->StageXrMotionProgram(LoadNativeCook(cooks, "motion", Editor::LoadEditorVulkanMultiviewMotionCook));
                viewport->StageXrCoverageProgram(LoadNativeCook(cooks, "coverage", Editor::LoadEditorVulkanMultiviewCoverageCook));
                viewport->StageXrCoverageReductionProgram(
                    LoadNativeCook(cooks, "reduction", Editor::LoadEditorVulkanCoverageReductionCook));
                gui = std::make_unique<Editor::EditorGuiRendererVulkan>(*window, *frontend, bridge);
                RequireValue(gui->Initialize());
                RequireValue(session->PrepareSession());
                play = RequireValue(Host::OpenXRPlayHost::Create({*session,
                                                                  router,
                                                                  *viewport,
                                                                  *frontend,
                                                                  bindings,
                                                                  scheduler->DispatchSource(),
                                                                  Duration::FromMilliseconds(10),
                                                                  {root.value}}));
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

            Input::InputActionCollection InputPort(const std::optional<Input::PlayerId>) {
                std::vector registered{Input::ActionDescriptor{Input::ActionId{"select"},
                                                               bindings->schema.actions.front().valueType,
                                                               Input::InputContextId{"gameplay"},
                                                               true,
                                                               {{.kind = Input::BindingControlKind::Key, .key = Input::Key::Space}}}};
                RequireValue(router.SetActionMap(std::move(registered)));
                inputContext = router.PushContext(Input::InputContextId{"gameplay"}, Input::InputContextKind::Gameplay);
                return RequireValue(router.ActionCollection(inputContext));
            }

            /** @brief Bounds explicit test observation without introducing a normal-frame GPU wait into Play. */
            template <typename Predicate> static void Wait(Predicate done) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
                while (!done()) {
                    REQUIRE(std::chrono::steady_clock::now() < deadline);
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }

            void Project(const std::uint64_t number) {
                CAPTURE(number);
                const auto step = [](std::string_view name, auto result) {
                    INFO(name);
                    return RequireValue(std::move(result));
                };
                Wait([&] {
                    SDL_PumpEvents();
                    return step("poll native WSI availability", frontend->PollPresentationAvailability(extent));
                });
                REQUIRE(step("prepare native prediction", play->PrepareRenderFrame(number)));
                auto frame = step("begin Vulkan WSI frame", frontend->BeginFrame({number, extent}));
                step("execute native Play graph", play->ExecuteRenderFrame(frame, {}));
                const auto mirror = play->MirrorView(number);
                REQUIRE(mirror.IsValid());
                step("begin GUI", gui->BeginFrame());
                ImGui::NewFrame();
                gui->BindViewportTexture(mirror);
                ImGui::GetForegroundDrawList()->AddImage(static_cast<ImTextureID>(mirror.textureId), {0, 0}, {80, 60});
                ImGui::Render();
                step("record Vulkan GUI tail", frame.ComposeGui(*gui));
                step("seal Play GUI phase", play->FinishGui(true));
                step("submit Vulkan WSI frame", frame.Present());
                Wait([&] {
                    step("poll native compositor tail", play->ProgressRenderTail());
                    return script.frames.ends == number;
                });
            }
        };
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
}  // namespace Horo::Tests::OpenXRApplication
