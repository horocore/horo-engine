#include "HoroEditorApp.h"
#if defined(HORO_HAS_OPENXR_HOST)
#include "EditorOpenXRBootstrap.h"
#include "EditorOpenXRHostServices.h"
#endif
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
#include "OpenXRApplicationSession.h"
#include "OpenXRPlayHost.h"
#endif

#include "EditorAudioPreviewHost.h"
#include "EditorFrameAdmission.h"
#include "EditorGuiLiveReload.h"
#include "EditorGuiLiveReloadBuildConfig.h"
#include "EditorNavigationBakeHost.h"
#include "EditorPresentationSurface.h"
#include "EditorProjectSceneHost.h"
#include "EditorUiStylePreparation.h"
#include "Horo/Editor/UiInspectorResources.h"
#include "Horo/Extensions/EditorSurfaceContext.h"
#include "Horo/Gameplay/GameModuleRetirement.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "ScenePhysicsHost.h"
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
#include "EditorSurfaceLifecycleQualification.h"
#endif
#include "EditorUserStateMigration.h"
#include "EditorWindowChrome.h"
#include "EditorWorkerShutdown.h"
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Application/HostObservability.h"
#include "Horo/Application/NetworkDebugger.h"
#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ShaderBuildService.h"
#include "Horo/Application/WorldBakeApplication.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Editor/CollisionProjectSettingsSession.h"
#include "Horo/Editor/DefaultScreenFactories.h"
#include "Horo/Editor/DefaultWorkspacePanels.h"
#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorIcons.h"
#include "Horo/Editor/EditorMenuModel.h"
#include "Horo/Editor/EditorSettingsEvents.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/GuiRoute.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/NativeAssetRegistrySource.h"
#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Editor/ProjectOpenService.h"
#include "Horo/Editor/RecentProjectInspectionService.h"
#include "Horo/Editor/WelcomeController.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "Horo/Editor/WorkspaceReadSources.h"
#include "Horo/Editor/WorldStreamingPreview.h"
#include "Horo/Editor/WorldStreamingPreviewRuntime.h"
#include "Horo/Extensions/ExtensionInventory.h"
#include "Horo/Extensions/ExtensionMarketplace.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Logging/StructuredLogStore.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Physics/PhysicsSceneActivation.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "Horo/Platform/SecureRandom.h"
#include "Horo/Runtime/Input.h"
#include "Horo/Runtime/Render/FramePacingErrors.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "Horo/Runtime/Save/SaveTelemetry.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "HostModuleComposition.h"
#include "PackageHostPolicy.h"
#include "PackageProjectHost.h"
#include "RenderGraphInspectionPane.h"
#include "RuntimeUiPaintHost.h"
#include "RuntimeUiPaintProjectSources.h"
#include "editor/EditorServiceErrors.h"
#include "editor/document/EditorViewportSceneExtractor.h"
#include "editor/input/EditorInputActions.h"
#include "editor/project_model/RendererAvailability.h"
#include "editor/renderer/EditorGuiRenderer.h"
#include "editor/renderer/EditorRenderMemoryScopes.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "editor/update/UpdateExperienceSession.h"
#include "runtime/input/sdl/SdlInputBackend.h"

#if defined(HORO_HAS_RENDER_OPENGL)
#include "OpenGLBackendModule.h"
#endif
#if defined(HORO_HAS_RENDER_METAL)
#include "runtime/renderer/modules/metal/MetalBackendModule.h"
#endif

#include "Horo/Editor/SettingsModal.h"
#include "editor/menu/EditorMenuPlatform.h"
#if defined(HORO_HAS_RENDER_OPENGL)
#include "editor/renderer/opengl/EditorGuiRendererOpenGL.h"
#include "editor/renderer/opengl/EditorViewportRendererOpenGL.h"
#include "editor/renderer/opengl/SdlOpenGLPresentationPort.h"
#endif
#if defined(HORO_HAS_RENDER_METAL)
#include "editor/renderer/metal/EditorGuiRendererMetal.h"
#include "editor/renderer/metal/EditorViewportRendererMetal.h"
#include "editor/renderer/metal/SdlMetalPresentationPort.h"
#endif

#if defined(HORO_HAS_RENDER_VULKAN)
#include "EditorVulkanShaderAssets.h"
#include "VulkanBackendModule.h"
#include "VulkanEditorGraphicsBridge.h"
#include "editor/renderer/vulkan/EditorGuiRendererVulkan.h"
#include "editor/renderer/vulkan/EditorViewportRendererVulkan.h"
#include "editor/renderer/vulkan/SdlVulkanPresentationPort.h"
#endif

#if defined(HORO_HAS_OPENXR_HOST)
#include "Horo/XR/XRErrors.h"
#include "Horo/XR/XRProjectSettings.h"
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <portable-file-dialogs.h>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include <filesystem>
#include <stb_image.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace Horo::Editor {
    namespace {
        using Theme::Fonts;

        /** @brief Desktop picker adapter composed only by the graphical editor host. */
        class PfdNativeDialogs final : public NativeDialogs {
        public:
            std::vector<std::filesystem::path> ChooseOpenFiles(const std::string_view title) override {
                std::vector<std::filesystem::path> selected;
                for (const std::string &utf8Path :
                     pfd::open_file(std::string{title}, "", {"All Files", "*"}, pfd::opt::multiselect).result())
                    selected.push_back(FromUtf8(utf8Path));
                return selected;
            }

            std::optional<std::filesystem::path> ChooseFolder(const std::string_view title) override {
                const std::string utf8Path = pfd::select_folder(std::string{title}).result();
                if (utf8Path.empty())
                    return std::nullopt;
                return FromUtf8(utf8Path);
            }

        private:
            /** @brief Converts the UTF-8 path returned by the picker to the native path representation. */
            [[nodiscard]] static std::filesystem::path FromUtf8(const std::string_view utf8Path) {
                std::u8string bytes;
                bytes.reserve(utf8Path.size());
                for (const char byte : utf8Path)
                    bytes.push_back(static_cast<char8_t>(byte));
                return std::filesystem::path{bytes};
            }
        };

        class PreparedAssetRegistryState final : public IPreparedProjectOpenDerivedState {
        public:
            PreparedAssetRegistryState(Assets::AssetRegistry &registry, CollisionInventoryEvidence &evidence,
                                       std::filesystem::path projectRoot, Assets::AssetRegistryCandidate candidate,
                                       Application::ProjectCollisionFileSnapshot collision)
                : registry_(registry), evidence_(evidence), projectRoot_(std::move(projectRoot)), candidate_(std::move(candidate)),
                  collision_(std::move(collision)) {}

            Result<std::string> Install() override {
                evidence_ = {};
                auto collision = Application::LoadProjectCollisionSettings(projectRoot_, collision_.settings->Input().projectId);
                if (collision.HasError())
                    return Result<std::string>::Failure(collision.ErrorValue());
                if (collision.Value().identity != collision_.identity)
                    return Result<std::string>::Failure(
                        MakeError(ProjectOpenErrors::DerivedStateFailed,
                                  "Collision settings changed after project preparation; retry project open."));
                Assets::AssetRegistry validationRegistry;
                Assets::AssetRegistryCandidate diskCandidate = candidate_;
                if (const Assets::AssetRegistryBuildReport validated = validationRegistry.Publish(std::move(diskCandidate));
                    validated.status == Assets::AssetRegistryBuildStatus::Failed)
                    return Result<std::string>::Failure(
                        MakeError(ProjectOpenErrors::DerivedStateFailed, "Asset registry candidate was rejected."));
                if (const auto saved =
                        Assets::AssetIndexStore::SaveAtomically(projectRoot_ / std::filesystem::path{ProjectLayout::AssetIndexPath},
                                                                validationRegistry.Snapshot());
                    saved.HasError())
                    return Result<std::string>::Failure(saved.ErrorValue());
                const Assets::AssetRegistryBuildReport report = registry_.Publish(std::move(candidate_));
                if (report.status == Assets::AssetRegistryBuildStatus::Failed)
                    return Result<std::string>::Failure(
                        MakeError(ProjectOpenErrors::DerivedStateFailed, "Validated asset registry publication failed."));
                evidence_ = {report.publishedRevision, report.status};
                return Result<std::string>::Success(std::to_string(report.publishedRevision.value));
            }

        private:
            Assets::AssetRegistry &registry_;
            CollisionInventoryEvidence &evidence_;
            std::filesystem::path projectRoot_;
            Assets::AssetRegistryCandidate candidate_;
            Application::ProjectCollisionFileSnapshot collision_;
        };

        class AssetRegistryProjectOpenContributor final : public IProjectOpenDerivedStateContributor {
        public:
            AssetRegistryProjectOpenContributor(Assets::AssetRegistry &registry, CollisionInventoryEvidence &evidence)
                : registry_(registry), evidence_(evidence) {}

            std::string_view Id() const noexcept override {
                return "horo.assets.registry";
            }

            Result<std::unique_ptr<IPreparedProjectOpenDerivedState>> Prepare(const std::filesystem::path &projectRoot,
                                                                              const CancellationToken &cancellation) override {
                if (cancellation.IsCancellationRequested())
                    return Result<std::unique_ptr<IPreparedProjectOpenDerivedState>>::Failure(MakeError(ProjectOpenErrors::Cancelled));
                auto metadata = Application::LoadProjectMetadata(projectRoot);
                if (metadata.HasError())
                    return Result<std::unique_ptr<IPreparedProjectOpenDerivedState>>::Failure(metadata.ErrorValue());
                auto collision = Application::LoadProjectCollisionSettings(projectRoot, metadata.Value().projectId);
                if (collision.HasError())
                    return Result<std::unique_ptr<IPreparedProjectOpenDerivedState>>::Failure(collision.ErrorValue());
                auto candidate = Assets::PrepareAssetRegistryCandidate(projectRoot);
                if (candidate.HasError())
                    return Result<std::unique_ptr<IPreparedProjectOpenDerivedState>>::Failure(candidate.ErrorValue());
                return Result<std::unique_ptr<IPreparedProjectOpenDerivedState>>::Success(
                    std::make_unique<PreparedAssetRegistryState>(registry_, evidence_, projectRoot, std::move(candidate).Value(),
                                                                 std::move(collision).Value()));
            }

        private:
            Assets::AssetRegistry &registry_;
            CollisionInventoryEvidence &evidence_;
        };

        /** @brief Admits project scope before any new project's registry is installed. */
        class SceneScopeProjectOpenContributor final : public IProjectOpenDerivedStateContributor {
        public:
            SceneScopeProjectOpenContributor(std::filesystem::path root, const Physics::PhysicsRuntimeMode mode)
                : root_(std::move(root)), mode_(mode) {}

            std::string_view Id() const noexcept override {
                return "horo.scene.project_scope";
            }

            Result<std::unique_ptr<IPreparedProjectOpenDerivedState>> Prepare(const std::filesystem::path &root,
                                                                              const CancellationToken &cancellation) override {
                if (cancellation.IsCancellationRequested())
                    return Result<std::unique_ptr<IPreparedProjectOpenDerivedState>>::Failure(MakeError(ProjectOpenErrors::Cancelled));
                if (const auto admitted = Application::Internal::ValidateEditorPreviewProjectScope(root_, root, mode_); admitted.HasError())
                    return Result<std::unique_ptr<IPreparedProjectOpenDerivedState>>::Failure(admitted.ErrorValue());
                return Result<std::unique_ptr<IPreparedProjectOpenDerivedState>>::Success(std::make_unique<PreparedScope>());
            }

        private:
            class PreparedScope final : public IPreparedProjectOpenDerivedState {
            public:
                Result<std::string> Install() override {
                    return Result<std::string>::Success("startup-project-scope");
                }
            };

            std::filesystem::path root_;
            Physics::PhysicsRuntimeMode mode_;
        };

        struct EditorGuiOptions {
            bool textPreview = false;
            bool exitAfterFirstFrame = false;
            std::uint64_t exitAfterFrames = 0;
            std::uint32_t frameRateLimit = 0;
            std::string rendererBackend;
            std::string projectRoot;
            std::optional<Physics::PhysicsRuntimeMode> physicsMode{Physics::PhysicsRuntimeMode::Null};
            std::string cookedRoot;
            std::string cookTarget;
            bool cookOptionsValid{true};
            std::optional<Runtime::SceneObjectId> xrTrackingRoot;
            bool xrTrackingRootValid{true};
            std::optional<bool> prefabGranted{false};
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
            bool qualifySurfaceLifecycle = false;
#endif
        };

#if defined(HORO_HAS_OPENXR_HOST)
        /** @brief Joins explicit invocation selection with installer selection before any native XR preparation. */
        [[nodiscard]] Result<Runtime::SceneObjectId> SelectedXrTrackingRoot(const EditorGuiOptions &options,
                                                                            const EditorOpenXRHostServices &host) {
            const auto selected = options.xrTrackingRoot.value_or(host.trackingRoot);
            if (!options.xrTrackingRootValid || !selected.IsValid() ||
                (options.xrTrackingRoot && host.trackingRoot.IsValid() && *options.xrTrackingRoot != host.trackingRoot))
                return Result<Runtime::SceneObjectId>::Failure(
                    MakeError(XR::XRErrors::OperationInvalid,
                              "Selected XR Play requires one explicit consistent authored tracking-root identity"));
            return Result<Runtime::SceneObjectId>::Success(selected);
        }
#endif

        struct EditorTextures {
            std::uintptr_t logo{0};
        };

        /** @brief Resolves packaged assets relative to the executable or application bundle. */
        [[nodiscard]] const std::filesystem::path &EditorAssetRoot() {
            static const std::filesystem::path root = [] {
                if (const char *basePath = SDL_GetBasePath(); basePath != nullptr && basePath[0] != '\0') {
                    return (std::filesystem::path{basePath} / HORO_EDITOR_PACKAGED_ASSET_ROOT_RELATIVE).lexically_normal();
                }
                return std::filesystem::path{};
            }();
            return root;
        }

        /** @brief Resolves one editor asset below the active asset root. */
        [[nodiscard]] std::string AssetPath(const char *rel) {
            return (EditorAssetRoot() / rel).string();
        }

        /** @brief Applies the shared Horo logo through SDL on window systems that support runtime icons. */
        void ApplyEditorWindowIcon(SDL_Window &window) {
            const std::string path = AssetPath("launcher/logo.png");
            int width = 0;
            int height = 0;
            int channels = 0;
            const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{stbi_load(path.c_str(), &width, &height, &channels, 4),
                                                                              &stbi_image_free};
            if (pixels == nullptr) {
                LOG_WARN("platform.assets", "Window icon not found at '%s'.", path.c_str());
                return;
            }
            if (width <= 0 || height <= 0) {
                LOG_WARN("platform.assets", "Window icon at '%s' has invalid dimensions.", path.c_str());
                return;
            }

            const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> icon{SDL_CreateSurfaceFrom(width, height,
                                                                                                         SDL_PIXELFORMAT_RGBA32,
                                                                                                         pixels.get(), width * 4),
                                                                                   &SDL_DestroySurface};
            if (icon == nullptr) {
                LOG_WARN("platform.sdl", "Unable to create the Horo Editor window icon: %s", SDL_GetError());
                return;
            }
            if (!SDL_SetWindowIcon(&window, icon.get()))
                LOG_WARN("platform.sdl", "Unable to apply the Horo Editor window icon: %s", SDL_GetError());
        }

        [[nodiscard]] bool LoadEditorCatalogResources(LocalizationService &localization) {
            const std::filesystem::path root = EditorAssetRoot() / "localization" / "editor";
            bool loadedAny = false;
            std::error_code error;
            if (!std::filesystem::exists(root, error))
                return false;
            for (const auto &entry : std::filesystem::directory_iterator(root, error)) {
                if (error || !entry.is_regular_file() || entry.path().extension() != ".json")
                    continue;
                LocalizationError loadError;
                loadedAny = localization.LoadCatalogFile(entry.path(), &loadError) || loadedAny;
            }
            return loadedAny;
        }

        [[nodiscard]] const ImWchar *BuildEditorGlyphRanges(ImGuiIO &io) {
            // ImGui default ranges do not always include the small UI glyphs used
            // by the HTML mockups (arrows, multiplication sign, square icon,
            // middle dot). Without these, they render as '?' in the editor.
            static ImVector<ImWchar> ranges;
            if (ranges.empty()) {
                ImFontGlyphRangesBuilder builder;
                builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
                // Add Latin Extended-A for Turkish and other European characters
                static constexpr std::array<ImWchar, 3> latinExtendedA = {0x0100, 0x017F, 0};
                builder.AddRanges(latinExtendedA.data());

                builder.AddChar(0x00B7);  // ·
                builder.AddChar(0x00D7);  // ×
                builder.AddChar(0x2013);  // –
                builder.AddChar(0x2014);  // —
                builder.AddChar(0x2026);  // …
                builder.AddChar(0x2190);  // ←
                builder.AddChar(0x2191);  // ↑
                builder.AddChar(0x2192);  // →
                builder.AddChar(0x2193);  // ↓
                builder.AddChar(0x25A1);  // □
                builder.AddChar(0x25AA);  // ▪
                builder.AddChar(0x25AB);  // ▫
                builder.AddChar(0x2713);  // ✓
                builder.BuildRanges(&ranges);
            }
            return ranges.Data;
        }

        [[nodiscard]] float QueryRasterizerDensity(SDL_Window *window) {
            int windowW = 0;
            int windowH = 0;
            int drawableW = 0;
            int drawableH = 0;
            SDL_GetWindowSize(window, &windowW, &windowH);
            SDL_GetWindowSizeInPixels(window, &drawableW, &drawableH);
            if (windowW <= 0 || windowH <= 0) {
                return 1.0F;
            }
            const float scaleX = static_cast<float>(drawableW) / static_cast<float>(windowW);
            const float scaleY = static_cast<float>(drawableH) / static_cast<float>(windowH);
            const float scale = (scaleX < scaleY) ? scaleX : scaleY;
            return (scale > 1.0F) ? scale : 1.0F;
        }

        [[nodiscard]] Fonts LoadEditorFonts(ImGuiIO &io, const float rasterizerDensity) {
            Fonts f;
            const ImWchar *ranges = BuildEditorGlyphRanges(io);
            ImFontConfig sansCfg{};
            sansCfg.OversampleH = 3;
            sansCfg.OversampleV = 2;
            sansCfg.RasterizerDensity = rasterizerDensity;
            ImFontConfig compactCfg{};
            compactCfg.OversampleH = 3;
            compactCfg.OversampleV = 2;
            compactCfg.RasterizerDensity = rasterizerDensity;
            ImFontConfig emphasisCfg{};
            emphasisCfg.OversampleH = 3;
            emphasisCfg.OversampleV = 2;
            emphasisCfg.RasterizerDensity = rasterizerDensity;
            f.sans =
                io.Fonts->AddFontFromFileTTF(AssetPath("fonts/inter/InterVariable.ttf").c_str(), Theme::FontPx::Sans, &sansCfg, ranges);
            f.sansCompact = io.Fonts->AddFontFromFileTTF(AssetPath("fonts/inter/InterVariable.ttf").c_str(), Theme::FontPx::SansCompact,
                                                         &compactCfg, ranges);
            f.sansEmphasis = io.Fonts->AddFontFromFileTTF(AssetPath("fonts/inter/InterVariable.ttf").c_str(), Theme::FontPx::SansEmphasis,
                                                          &emphasisCfg, ranges);

            ImFontConfig iconCfg{};
            iconCfg.OversampleH = 3;
            iconCfg.OversampleV = 2;
            iconCfg.RasterizerDensity = rasterizerDensity;
            const std::span iconRanges = Ui::UiIconRegistry::MaterialSymbolGlyphRanges();
            f.monospace = io.Fonts->AddFontFromFileTTF(AssetPath("fonts/ibm-plex-mono/IBMPlexMono-Regular.ttf").c_str(),
                                                       Theme::FontPx::Sans, &sansCfg, ranges);
            f.monospaceEmphasis = io.Fonts->AddFontFromFileTTF(AssetPath("fonts/ibm-plex-mono/IBMPlexMono-SemiBold.ttf").c_str(),
                                                               Theme::FontPx::Sans, &sansCfg, ranges);
            f.codeFamilies.push_back({"IBM Plex Mono", f.monospace, f.monospaceEmphasis});
            // Native font discovery stays in the executable composition root. Only loaded
            // atlas families are offered; the source widget never probes the filesystem.
            const auto addFamily = [&](const char *name, const char *path) {
                std::error_code error;
                if (!std::filesystem::is_regular_file(path, error))
                    return;
                if (auto *face = io.Fonts->AddFontFromFileTTF(path, Theme::FontPx::Sans, &sansCfg, ranges))
                    f.codeFamilies.push_back({name, face, face});
            };
#if defined(__APPLE__)
            addFamily("Menlo", "/System/Library/Fonts/Menlo.ttc");
            addFamily("SF Mono", "/System/Library/Fonts/SFNSMono.ttf");
#elif defined(_WIN32)
            if (const char *windows = std::getenv("WINDIR")) {
                const auto font = std::filesystem::path{windows} / "Fonts" / "consola.ttf";
                addFamily("Consolas", font.string().c_str());
            }
#else
            addFamily("DejaVu Sans Mono", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf");
            addFamily("Liberation Mono", "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf");
#endif
            f.icon = io.Fonts->AddFontFromFileTTF(AssetPath("fonts/MaterialSymbolsOutlined.ttf").c_str(), Theme::FontPx::Icon, &iconCfg,
                                                  iconRanges.data());
            if (f.sans)
                io.FontDefault = f.sans;
            return f;
        }

        [[nodiscard]] std::uintptr_t LoadEditorTexture(IEditorGuiRenderer &guiRenderer, const char *assetPath) {
            const std::string path = AssetPath(assetPath);
            int w = 0;
            int h = 0;
            int c = 0;
            auto *px = stbi_load(path.c_str(), &w, &h, &c, 4);
            if (!px) {
                LOG_WARN("platform.assets", "Editor texture not found at '%s'.", path.c_str());
                return 0;
            }
            if (w <= 0 || h <= 0) {
                stbi_image_free(px);
                LOG_WARN("platform.assets", "Editor texture at '%s' has invalid dimensions.", path.c_str());
                return 0;
            }
            const std::span pixels{px, static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U};
            const Result<std::uintptr_t> uploaded = guiRenderer.CreateTexture(EditorRgba8ImageView{
                .width = static_cast<std::uint32_t>(w),
                .height = static_cast<std::uint32_t>(h),
                .pixels = pixels,
            });
            stbi_image_free(px);
            if (uploaded.HasError()) {
                LOG_WARN("platform.assets", "Editor texture upload failed for '%s': %s", path.c_str(),
                         uploaded.ErrorValue().message.c_str());
                return 0;
            }
            return uploaded.Value();
        }

        [[nodiscard]] EditorTextures LoadEditorTextures(IEditorGuiRenderer &guiRenderer) {
            EditorTextures t;
            t.logo = LoadEditorTexture(guiRenderer, "launcher/logo.png");
            return t;
        }

        void DestroyEditorTextures(EditorTextures &textures, IEditorGuiRenderer &guiRenderer) noexcept {
            if (textures.logo != 0) {
                guiRenderer.DestroyTexture(textures.logo);
                textures.logo = 0;
            }
        }

        /** @brief Captures explicit host Physics ownership without claiming activation of an unknown world. */
        [[nodiscard]] CollisionActivationEvidence CaptureCollisionActivation(const Physics::PhysicsRuntimeMode mode) {
            CollisionActivationEvidence evidence{.revision = 1};
            if (mode == Physics::PhysicsRuntimeMode::Null)
                evidence.state = CollisionActivationState::NoPhysics;
            return evidence;
        }

        [[nodiscard]] std::string CurrentSystemErrorMessage() {
#if defined(_MSC_VER)
            if (std::array<char, 256> buffer{}; strerror_s(buffer.data(), buffer.size(), errno) == 0)
                return buffer.data();
            return "unknown system error";
#else
            return std::strerror(errno);
#endif
        }

        /** @brief Composes the editor's single local logging, metrics and profiler runtime. */
        [[nodiscard]] std::unique_ptr<Application::HostObservabilitySession> InitializeEditorObservability() {
            Application::HostObservabilityConfiguration hostConfiguration{.logging = {.logDirectory = "~/.horo/logs",
                                                                                      .baseName = "horo-editor",
                                                                                      .hostName = "HoroEditor",
                                                                                      .hostVersion = HORO_ENGINE_VERSION_STRING},
                                                                          .identity = {.processRole = "editor",
                                                                                       .engineVersion = HORO_ENGINE_VERSION_STRING,
                                                                                       .buildConfiguration = HORO_BUILD_CONFIGURATION,
                                                                                       .sourceRevision = HORO_SOURCE_REVISION}};
            hostConfiguration.summaries.push_back(&Runtime::SummarizeSaveTelemetry);
            auto session = Application::HostObservabilitySession::Start(std::move(hostConfiguration));
            if (session == nullptr)
                std::fprintf(stderr, "[Observability] local runtime initialization failed; emergency logging remains available\n");
            return session;
        }

        struct EditorTelemetry {
            Telemetry::Gauge frameNumber;
            Telemetry::Gauge frameDuration;
            Telemetry::Gauge presentCallDuration;
            Telemetry::Gauge nativeFeedbackSupported;
            Telemetry::Gauge pacingMissedDeadlines;
            Telemetry::Gauge droppedRecords;
            Telemetry::Gauge sinkFailures;
            Prefab::PrefabMetricHandles prefab;
        };

        [[nodiscard]] EditorTelemetry RegisterEditorTelemetry() {
            using enum Telemetry::MetricUnit;
            return {
                .frameNumber =
                    Telemetry::Runtime::RegisterGauge({.name = "horo.editor.frame.number", .subsystem = "Editor.Runtime", .unit = Count}),
                .frameDuration = Telemetry::Runtime::RegisterGauge(
                    {.name = "horo.editor.frame.duration", .subsystem = "Editor.Runtime", .unit = Seconds}),
                .presentCallDuration = Telemetry::Runtime::RegisterGauge(
                    {.name = "horo.editor.present.call_duration", .subsystem = "Editor.Runtime", .unit = Seconds}),
                .nativeFeedbackSupported = Telemetry::Runtime::RegisterGauge(
                    {.name = "horo.editor.present.native_feedback_supported", .subsystem = "Editor.Runtime", .unit = Count}),
                .pacingMissedDeadlines = Telemetry::Runtime::RegisterGauge(
                    {.name = "horo.editor.pacing.missed_deadlines", .subsystem = "Editor.Runtime", .unit = Count}),
                .droppedRecords = Telemetry::Runtime::RegisterGauge(
                    {.name = "horo.observability.records.dropped", .subsystem = "Foundation.Observability", .unit = Count}),
                .sinkFailures = Telemetry::Runtime::RegisterGauge(
                    {.name = "horo.observability.sink.failures", .subsystem = "Foundation.Observability", .unit = Count}),
                .prefab = Prefab::RegisterPrefabMetricHandles(Telemetry::MetricCollectionLevel::Detailed, true),
            };
        }

        // Shared components and the welcome screen renderer live under src/editor/design_system and src/editor/screens.
        // Modal implementations live under src/editor/modals.
    }  // namespace

    /** @brief Parses one cap value; the invalid sentinel is rejected by the typed pacing policy. */
    [[nodiscard]] static std::uint32_t ParseFrameRateLimit(const std::string_view value) noexcept {
        std::uint32_t limit{};
        if (const auto parsed = std::from_chars(value.data(), value.data() + value.size(), limit);
            parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size())
            return limit;
        return 1001;
    }

    /** @brief Identifies the one pending option value without stringly typed parser state. */
    enum class PendingEditorOption : std::uint8_t {
        None,
        Renderer,
        Project,
        Physics,
        Prefab,
        CookedRoot,
        CookTarget,
        FrameRateLimit,
        ExitAfterFrames,
        XrTrackingRoot
    };

    /** @brief Consumes one value before interpreting flag syntax, preserving existing argument ordering. */
    [[nodiscard]] static bool ConsumeEditorOptionValue(const std::string_view value, PendingEditorOption &pending,
                                                       EditorGuiOptions &opts) noexcept {
        using enum PendingEditorOption;
        switch (std::exchange(pending, None)) {
            case Renderer:
                opts.rendererBackend = value;
                return true;
            case Project:
                opts.projectRoot = value;
                return true;
            case Physics:
                opts.physicsMode = Application::Internal::ParseScenePhysicsHostMode(value);
                return true;
            case Prefab:
                if (value == "grant")
                    opts.prefabGranted = true;
                else if (value == "deny")
                    opts.prefabGranted = false;
                else
                    opts.prefabGranted.reset();
                return true;
            case CookedRoot:
                opts.cookedRoot = value;
                opts.cookOptionsValid = !value.empty();
                return true;
            case CookTarget:
                opts.cookTarget = value;
                opts.cookOptionsValid = !value.empty();
                return true;
            case FrameRateLimit:
                opts.frameRateLimit = ParseFrameRateLimit(value);
                return true;
            case ExitAfterFrames:
                if (const auto parsed = std::from_chars(value.data(), value.data() + value.size(), opts.exitAfterFrames);
                    parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                    opts.exitAfterFrames = 0;
                return true;
            case XrTrackingRoot: {
                std::uint64_t identity{};
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), identity);
                opts.xrTrackingRootValid = opts.xrTrackingRootValid && !opts.xrTrackingRoot && identity != 0 && parsed.ec == std::errc{} &&
                                           parsed.ptr == value.data() + value.size();
                if (opts.xrTrackingRootValid)
                    opts.xrTrackingRoot = Runtime::SceneObjectId{identity};
                return true;
            }
            case None:
                return false;
        }
        return false;
    }

    /** @brief Applies one independent flag or records which value the next argument owns. */
    static void ApplyEditorOptionFlag(const std::string_view argument, PendingEditorOption &pending, EditorGuiOptions &opts) noexcept {
        using enum PendingEditorOption;
        if (argument == "--text-preview")
            opts.textPreview = true;
        else if (argument == "--exit-after-first-frame")
            opts.exitAfterFirstFrame = true;
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
        else if (argument == "--qualify-opengl-surface-lifecycle")
            opts.qualifySurfaceLifecycle = true;
#endif
        else if (argument == "--frame-rate-limit")
            pending = FrameRateLimit;
        else if (argument == "--exit-after-frames")
            pending = ExitAfterFrames;
        else if (argument == "--xr-tracking-root")
            pending = XrTrackingRoot;
        else if (argument.starts_with("--xr-tracking-root=")) {
            pending = XrTrackingRoot;
            static_cast<void>(ConsumeEditorOptionValue(argument.substr(std::string_view{"--xr-tracking-root="}.size()), pending, opts));
        } else if (argument.starts_with("--renderer="))
            opts.rendererBackend = std::string{argument.substr(std::string_view{"--renderer="}.size())};
        else if (argument == "--renderer")
            pending = Renderer;
        else if (argument == "--project")
            pending = Project;
        else if (argument.starts_with("--physics="))
            opts.physicsMode = Application::Internal::ParseScenePhysicsHostMode(argument.substr(std::string_view{"--physics="}.size()));
        else if (argument == "--physics")
            pending = Physics;
        else if (argument == "--prefab")
            pending = Prefab;
        else if (argument == "--cooked-root")
            pending = CookedRoot;
        else if (argument == "--cook-target")
            pending = CookTarget;
        else if (argument.starts_with("--cooked-root="))
            opts.cookedRoot = argument.substr(std::string_view{"--cooked-root="}.size());
        else if (argument.starts_with("--cook-target="))
            opts.cookTarget = argument.substr(std::string_view{"--cook-target="}.size());
    }

    [[nodiscard]] static EditorGuiOptions ParseOptions(const std::span<char *> args) noexcept {
        using enum PendingEditorOption;
        EditorGuiOptions opts;
        PendingEditorOption pending = None;
        for (const char *rawArgument : args.subspan(std::min<std::size_t>(1, args.size()))) {
            const std::string_view argument{rawArgument};
            if (!ConsumeEditorOptionValue(argument, pending, opts))
                ApplyEditorOptionFlag(argument, pending, opts);
        }
        if (pending == FrameRateLimit)
            opts.frameRateLimit = 1001;
        if (pending == Physics)
            opts.physicsMode.reset();
        if (pending == Prefab)
            opts.prefabGranted.reset();
        if (pending == XrTrackingRoot)
            opts.xrTrackingRootValid = false;
        if (pending == CookedRoot || pending == CookTarget || opts.cookedRoot.empty() != opts.cookTarget.empty())
            opts.cookOptionsValid = false;
        return opts;
    }

    namespace {
        [[nodiscard]] const Render::RenderBackendModuleInfo *ResolveCompiledBackendModuleInfo(
            const std::string_view requestedBackend) noexcept {
#if defined(HORO_HAS_RENDER_OPENGL)
            if (requestedBackend.empty() || requestedBackend == "opengl")
                return &Render::GetOpenGLRenderBackendModuleInfo();
#endif
#if defined(HORO_HAS_RENDER_METAL)
            if (requestedBackend.empty() || requestedBackend == "metal")
                return &Render::GetMetalRenderBackendModuleInfo();
#endif
#if defined(HORO_HAS_RENDER_VULKAN)
            if (requestedBackend == "vulkan")
                return &Render::GetVulkanRenderBackendModuleInfo();
#endif
            return nullptr;
        }

        [[nodiscard]] RendererAvailabilitySnapshot BuildRendererAvailabilitySnapshot(const std::string_view activeBackendId) {
            std::vector<RendererBackendAvailability> entries;
            entries.reserve(3);
#if defined(HORO_HAS_RENDER_OPENGL)
            entries.push_back(RendererBackendAvailability{"opengl",
                                                          "OpenGL",
                                                          activeBackendId == "opengl" ? RendererAvailabilityState::Active
                                                                                      : RendererAvailabilityState::Available,
                                                          {}});
#else
            entries.push_back(RendererBackendAvailability{"opengl", "OpenGL", RendererAvailabilityState::NotInstalled,
                                                          "OpenGL renderer component is not installed in this editor build."});
#endif
#if defined(HORO_HAS_RENDER_METAL)
            entries.push_back(RendererBackendAvailability{"metal",
                                                          "Metal",
                                                          activeBackendId == "metal" ? RendererAvailabilityState::Active
                                                                                     : RendererAvailabilityState::Available,
                                                          {}});
#else
            entries.emplace_back("metal", "Metal", RendererAvailabilityState::NotInstalled,
                                 "Metal renderer component is not installed in this editor build.");
#endif
#if defined(HORO_HAS_RENDER_VULKAN)
            entries.emplace_back("vulkan", "Vulkan", RendererAvailabilityState::InstalledUnchecked,
                                 "Vulkan interactive presentation qualification is pending.");
#else
            entries.emplace_back("vulkan", "Vulkan", RendererAvailabilityState::NotInstalled,
                                 "Vulkan renderer component is not installed in this editor build.");
#endif
            return RendererAvailabilitySnapshot{std::move(entries), std::string{activeBackendId}};
        }

        [[nodiscard]] int RelaunchEditorForProject(const char *executable, const EditorRendererRestartRequest &request,
                                                   const EditorGuiOptions &options) {
            std::vector<std::string> arguments{executable,
                                               "--renderer",
                                               request.backendId,
                                               "--project",
                                               request.projectRoot,
                                               "--physics",
                                               std::string{Application::Internal::ScenePhysicsHostModeName(options.physicsMode.value())}};
            arguments.insert(arguments.end(), {"--prefab", options.prefabGranted.value() ? "grant" : "deny"});
            if (!options.cookedRoot.empty()) {
                arguments.insert(arguments.end(), {"--cooked-root", options.cookedRoot, "--cook-target", options.cookTarget});
            }
            std::vector<char *> nativeArguments;
            nativeArguments.reserve(arguments.size() + 1);
            for (std::string &argument : arguments) {
                nativeArguments.emplace_back(argument.data());
            }
            nativeArguments.emplace_back(nullptr);

#if defined(_WIN32)
            _execvp(executable, nativeArguments.data());
#else
            execvp(executable, nativeArguments.data());
#endif
            const std::string errorMessage = CurrentSystemErrorMessage();
            std::fprintf(stderr, "Unable to restart HoroEditor for renderer '%s': %s\n", request.backendId.c_str(), errorMessage.c_str());
            return 1;
        }

        [[nodiscard]] bool InitializeSdlAndCreateWindow(SDL_Window *&window,
                                                        const Render::RenderHostWindowRequirements &windowRequirements) {
            SDL_WindowFlags rendererWindowFlag = 0;
            switch (windowRequirements.presentation) {
                case Render::RenderPresentationKind::OpenGL:
                    rendererWindowFlag = SDL_WINDOW_OPENGL;
                    break;
                case Render::RenderPresentationKind::Metal:
                    rendererWindowFlag = SDL_WINDOW_METAL;
                    break;
                case Render::RenderPresentationKind::Vulkan:
                    rendererWindowFlag = SDL_WINDOW_VULKAN;
                    break;
                default:
                    LOG_CRITICAL("editor.renderer", "Selected renderer does not describe an interactive host window.");
                    return false;
            }

            if (!SDL_SetAppMetadata("Horo Editor", HORO_ENGINE_VERSION_STRING, HORO_EDITOR_APP_ID))
                LOG_WARN("platform.sdl", "Unable to set Horo Editor application metadata: %s", SDL_GetError());
            if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
                const char *err = SDL_GetError();
                LOG_CRITICAL("platform.sdl", "SDL_Init failed: %s", err);
                std::fprintf(stderr, "SDL_Init: %s\n", err);
                return false;
            }
            if (windowRequirements.presentation == Render::RenderPresentationKind::OpenGL) {
                SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
                SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
                SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
            }

            SDL_WindowFlags windowFlags = rendererWindowFlag;
            if (windowRequirements.resizable)
                windowFlags |= SDL_WINDOW_RESIZABLE;
            if (windowRequirements.highPixelDensity)
                windowFlags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
            window = SDL_CreateWindow("Horo Editor", 1000, 760, windowFlags);
            if (!window) {
                LOG_CRITICAL("platform.sdl", "SDL_CreateWindow failed: %s", SDL_GetError());
                SDL_Quit();
                return false;
            }
            ApplyEditorWindowIcon(*window);

            return true;
        }

        void ApplySavedThemePreference() {
            const char *overridePath = std::getenv("HORO_THEME_OVERRIDE");
            const int active = Theme::GetActiveThemeIndex();
            if (overridePath && active >= 0 && static_cast<std::size_t>(active) < Theme::GetThemeList().size() &&
                Theme::GetThemeList()[static_cast<std::size_t>(active)].sourcePath == overridePath)
                return;
            auto doc = LoadEditorSettingsDocument();
            if (doc.loadedFromDisk && !doc.parseError) {
                const auto savedIndex = static_cast<int>(doc.settings.themePreset);
                Theme::SelectThemeByIndex(savedIndex);
                LOG_DEBUG("editor.settings", "Restored theme preset index %d from disk.", savedIndex);
            } else if (doc.loadedFromDisk && doc.parseError) {
                LOG_WARN("editor.settings", "editor_settings.json exists but failed to parse — using defaults.");
            } else {
                LOG_DEBUG("editor.settings", "No editor_settings.json on disk — using defaults.");
            }
        }

        void ActivateInitialLocale(const EditorSettings &initialSettings, LocalizationService &localization) {
            LocalizationError localizationError;
            if (const auto locale = LocaleTag::Parse(initialSettings.languageTag); locale.has_value()) {
                if (localization.Prepare(*locale, &localizationError)) {
                    (void)localization.ActivatePrepared(&localizationError);
                    LOG_INFO("editor.startup", "Activated language tag: '%s'", locale->value.c_str());
                } else {
                    LOG_WARN("editor.startup", "Failed to prepare language tag '%s': %s", locale->value.c_str(),
                             localizationError.message.c_str());
                }
            } else {
                LOG_WARN("editor.startup", "Failed to parse language tag: '%s'", initialSettings.languageTag.c_str());
            }
        }

        struct EditorRenderComposition {
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
            Application::Internal::OpenXRApplicationSession *xrSession{}; /**< Startup-owned native parents survive this renderer. */
#endif
#if defined(HORO_HAS_RENDER_OPENGL)
            std::unique_ptr<Render::IOpenGLPresentationPort> openGlPresentationPort;
#endif
#if defined(HORO_HAS_RENDER_METAL)
            std::unique_ptr<Render::IMetalPresentationPort> metalPresentationPort;
            std::unique_ptr<Render::MetalEditorGraphicsBridge> metalGraphicsBridge;
#endif
#if defined(HORO_HAS_RENDER_VULKAN)
            std::unique_ptr<SdlVulkanPresentationPort> vulkanPresentationPort;
            std::unique_ptr<Render::Detail::VulkanEditorGraphicsBridge> vulkanGraphicsBridge;
#endif
            std::unique_ptr<Render::RenderFrontend> frontend;
            std::unique_ptr<IEditorGuiRenderer> guiRenderer;
            std::unique_ptr<IEditorViewportRenderer> viewportRenderer;
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
            EditorViewportRendererVulkan *xrViewport{}; /**< Borrow of the exact selected concrete viewport, never a downcast. */
            std::unique_ptr<Application::Internal::OpenXRPlayHost> xrPlay;
#endif
            Render::RenderTargetHandle viewportTarget;
            std::optional<Application::Internal::RuntimeUiPaintPort> uiRetirement;
            bool retirementBegun{false};
            std::optional<Error> retirementFailure;
        };

        [[nodiscard]] Error MakeEditorRendererError(const char *code, std::string message) {
            return Error{.code = ErrorCode{code},
                         .domain = ErrorDomainId{"horo.editor.renderer"},
                         .severity = ErrorSeverity::Critical,
                         .message = std::move(message)};
        }

        /** @brief Preserves the independently reported finite binary display scale as an exact reduced rational. */
        [[nodiscard]] Result<Runtime::Ui::UiCanvasDeviceScale> RuntimeUiDisplayScale(const float scale) {
            using Scale = Result<Runtime::Ui::UiCanvasDeviceScale>;
            if (!std::isfinite(scale) || scale <= 0.0F)
                return Scale::Failure(MakeError(Runtime::Ui::UiErrors::CanvasSpaceInvalid, SDL_GetError()));
            int exponent{};
            const auto fraction = std::frexp(scale, &exponent);
            exponent -= std::numeric_limits<float>::digits;
            std::uint64_t numerator = static_cast<std::uint32_t>(std::ldexp(fraction, std::numeric_limits<float>::digits));
            while ((numerator & 1U) == 0U) {
                numerator >>= 1;
                ++exponent;
            }
            if (exponent < -32 || exponent > 32)
                return Scale::Failure(MakeError(Runtime::Ui::UiErrors::CanvasSpaceOverflow));
            std::uint64_t denominator = 1;
            if (exponent >= 0)
                numerator <<= exponent;
            else
                denominator <<= -exponent;
            const auto common = std::gcd(numerator, denominator);
            numerator /= common;
            denominator /= common;
            if (numerator > (std::numeric_limits<std::uint32_t>::max)() || denominator > (std::numeric_limits<std::uint32_t>::max)())
                return Scale::Failure(MakeError(Runtime::Ui::UiErrors::CanvasSpaceOverflow));
            return Scale::Success({static_cast<std::uint32_t>(numerator), static_cast<std::uint32_t>(denominator)});
        }

        /** @brief Converts actual logical-window safe bounds into the same committed drawable's physical coordinate space. */
        [[nodiscard]] Result<Runtime::Ui::UiCanvasViewportEvidence> CaptureRuntimeUiOutput(SDL_Window &window,
                                                                                           const Render::FramebufferExtent pixels) {
            using Output = Result<Runtime::Ui::UiCanvasViewportEvidence>;
            int width{};
            int height{};
            SDL_Rect safe{};
            if (!SDL_GetWindowSize(&window, &width, &height) || !SDL_GetWindowSafeArea(&window, &safe))
                return Output::Failure(MakeError(Runtime::Ui::UiErrors::CanvasSpaceInvalid, SDL_GetError()));
            if (width <= 0 || height <= 0 || safe.x < 0 || safe.y < 0 || safe.w <= 0 || safe.h <= 0 || safe.x > width || safe.y > height ||
                safe.w > width - safe.x || safe.h > height - safe.y)
                return Output::Failure(MakeError(Runtime::Ui::UiErrors::CanvasSpaceInvalid));
            const auto scale = RuntimeUiDisplayScale(SDL_GetWindowDisplayScale(&window));
            if (scale.HasError())
                return Output::Failure(scale.ErrorValue());
            const auto inset = [](const int logical, const std::uint32_t physical, const int extent) {
                return static_cast<std::uint32_t>((static_cast<std::uint64_t>(logical) * physical + extent - 1) / extent);
            };
            return Output::Success(
                {{pixels.width, pixels.height},
                 scale.Value(),
                 {inset(safe.x, pixels.width, width), inset(safe.y, pixels.height, height),
                  inset(width - safe.x - safe.w, pixels.width, width), inset(height - safe.y - safe.h, pixels.height, height)}});
        }

        /** @brief Retains both native renderer owners before initialization so failures use explicit host retirement. */
        template <typename GuiRenderer, typename ViewportRenderer>
        [[nodiscard]] Result<void> InstallEditorRenderers(EditorRenderComposition &composition, std::unique_ptr<GuiRenderer> guiRenderer,
                                                          std::unique_ptr<ViewportRenderer> viewportRenderer) {
            ViewportRenderer *viewport = viewportRenderer.get();
            // Publish partial owners before initialization: failure may still leave native uploads in flight.
            composition.guiRenderer = std::move(guiRenderer);
            composition.viewportRenderer = std::move(viewportRenderer);
            if (const Result<void> initialized = composition.guiRenderer->Initialize(); initialized.HasError())
                return initialized;
            if (const Result<void> initialized = viewport->Initialize(); initialized.HasError())
                return initialized;
            return Result<void>::Success();
        }

        /** @brief Registers only the explicitly selected host backend and retains its native presentation parents. */
        [[nodiscard]] Result<void> RegisterEditorPresentationBackend(SDL_Window &window, const EditorGuiOptions &options,
                                                                     EditorRenderComposition &composition,
                                                                     Render::RenderBackendRegistry &registry) {
            if (options.rendererBackend == "opengl") {
#if defined(HORO_HAS_RENDER_OPENGL)
                auto port = std::make_unique<SdlOpenGLPresentationPort>(window);
                if (const Result<void> registered = Render::RegisterOpenGLRenderBackend(registry, *port); registered.HasError()) {
                    return Result<void>::Failure(registered.ErrorValue());
                }
                composition.openGlPresentationPort = std::move(port);
#else
                return Result<void>::Failure(MakeEditorRendererError("editor.renderer.component_missing",
                                                                     "OpenGL renderer component is not present in this build."));
#endif
            } else if (options.rendererBackend == "metal") {
#if defined(HORO_HAS_RENDER_METAL)
                if (!SDL_SetHint(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, "0") ||
                    SDL_GetHintBoolean(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, true)) {
                    return Result<void>::Failure(MakeEditorRendererError("editor.renderer.resize_policy",
                                                                         "Metal requires backend-owned drawable sizing; the SDL "
                                                                         "host policy could not be installed."));
                }
                auto port = std::make_unique<SdlMetalPresentationPort>(window);
                auto graphicsBridge = std::make_unique<Render::MetalEditorGraphicsBridge>();
                if (const Result<void> registered = Render::RegisterMetalRenderBackend(registry, *port, *graphicsBridge);
                    registered.HasError()) {
                    return Result<void>::Failure(registered.ErrorValue());
                }
                composition.metalPresentationPort = std::move(port);
                composition.metalGraphicsBridge = std::move(graphicsBridge);
#else
                return Result<void>::Failure(
                    MakeEditorRendererError("editor.renderer.component_missing", "Metal renderer component is not present in this build."));
#endif
            } else if (options.rendererBackend == "vulkan") {
#if defined(HORO_HAS_RENDER_VULKAN)
                auto port = std::make_unique<SdlVulkanPresentationPort>(window);
                auto bridge = std::make_unique<Render::Detail::VulkanEditorGraphicsBridge>();
                Render::IVulkanLoaderPort *selectedPort = port.get();
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
                if (composition.xrSession) {
                    if (auto attached = composition.xrSession->AttachGraphics(*port, *bridge); attached.HasError())
                        return attached;
                    selectedPort = &composition.xrSession->LoaderPort();
                }
#endif
                if (const auto registered = Render::RegisterVulkanRenderBackend(registry, *selectedPort, bridge.get());
                    registered.HasError())
                    return registered;
                composition.vulkanPresentationPort = std::move(port);
                composition.vulkanGraphicsBridge = std::move(bridge);
#else
                return Result<void>::Failure(MakeEditorRendererError("editor.renderer.component_missing",
                                                                     "Vulkan renderer component is not present in this build."));
#endif
            } else {
                return Result<void>::Failure(
                    MakeEditorRendererError("editor.renderer.unsupported_backend", "Requested renderer backend is unsupported."));
            }
            return Result<void>::Success();
        }

        /** @brief Installs the concrete GUI/viewport pair for the already selected and owned frontend. */
        [[nodiscard]] Result<void> InstallSelectedEditorRenderers(SDL_Window &window, const EditorGuiOptions &options,
                                                                  EditorRenderComposition &composition) {
            if (options.rendererBackend == "opengl") {
#if defined(HORO_HAS_RENDER_OPENGL)
                const auto &port = static_cast<const SdlOpenGLPresentationPort &>(*composition.openGlPresentationPort);
                auto guiRenderer = std::make_unique<EditorGuiRendererOpenGL>(window, port.Context(), *composition.frontend);
                auto viewportRenderer = std::make_unique<EditorViewportRendererOpenGL>(*composition.frontend);
                if (const Result<void> initialized =
                        InstallEditorRenderers(composition, std::move(guiRenderer), std::move(viewportRenderer));
                    initialized.HasError()) {
                    return Result<void>::Failure(initialized.ErrorValue());
                }
#endif
            } else if (options.rendererBackend == "metal") {
#if defined(HORO_HAS_RENDER_METAL)
                auto &graphicsBridge = *composition.metalGraphicsBridge;
                auto guiRenderer = std::make_unique<EditorGuiRendererMetal>(window, graphicsBridge, *composition.frontend);
                auto viewportRenderer = std::make_unique<EditorViewportRendererMetal>(*composition.frontend, graphicsBridge);
                if (const Result<void> initialized =
                        InstallEditorRenderers(composition, std::move(guiRenderer), std::move(viewportRenderer));
                    initialized.HasError()) {
                    return Result<void>::Failure(initialized.ErrorValue());
                }
#endif
            } else if (options.rendererBackend == "vulkan") {
#if defined(HORO_HAS_RENDER_VULKAN)
                auto cooked = LoadPackagedVulkanViewportShader(EditorAssetRoot());
                if (cooked.HasError())
                    return Result<void>::Failure(cooked.ErrorValue());
                auto &bridge = *composition.vulkanGraphicsBridge;
                return InstallEditorRenderers(composition, std::make_unique<EditorGuiRendererVulkan>(window, *composition.frontend, bridge),
                                              std::make_unique<EditorViewportRendererVulkan>(*composition.frontend, bridge,
                                                                                             std::move(cooked).Value()));
#endif
            }
            return Result<void>::Success();
        }

        /** @brief Keeps selected XR native/program composition outside the parent's unchanged desktop renderer installation. */
        [[nodiscard]] Result<void> InstallSelectedEditorHostRenderers(SDL_Window &window, const EditorGuiOptions &options,
                                                                      EditorRenderComposition &composition) {
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
            if (composition.xrSession) {
                if (options.rendererBackend != "vulkan" || !composition.vulkanGraphicsBridge)
                    return Result<void>::Failure(
                        MakeEditorRendererError("editor.xr.renderer_unavailable",
                                                "Selected XR requires the same admitted Vulkan renderer and device bridge."));
                auto cooked = LoadPackagedVulkanViewportShader(EditorAssetRoot());
                if (cooked.HasError())
                    return Result<void>::Failure(cooked.ErrorValue());
                auto multiview = LoadPackagedVulkanMultiviewShader(EditorAssetRoot());
                if (multiview.HasError())
                    return Result<void>::Failure(multiview.ErrorValue());
                auto &bridge = *composition.vulkanGraphicsBridge;
                auto viewport = std::make_unique<EditorViewportRendererVulkan>(*composition.frontend, bridge, std::move(cooked).Value());
                composition.xrViewport = viewport.get();
                if (const auto installed =
                        InstallEditorRenderers(composition,
                                               std::make_unique<EditorGuiRendererVulkan>(window, *composition.frontend, bridge),
                                               std::move(viewport));
                    installed.HasError())
                    return installed;
                return composition.xrViewport->PrepareXrProgram(std::move(multiview).Value());
            }
#endif
            return InstallSelectedEditorRenderers(window, options, composition);
        }

        /** @brief Starts explicit native readiness preparation only after the caller owns the complete frontend. */
        [[nodiscard]] Result<void> PrepareInitialEditorPresentation(SDL_Window &window, Render::RenderFrontend &frontend) {
            int initialWidth = 0;
            int initialHeight = 0;
            if (!SDL_GetWindowSizeInPixels(&window, &initialWidth, &initialHeight))
                return Result<void>::Failure(MakeEditorRendererError("editor.renderer.extent_unavailable", SDL_GetError()));
            if (initialWidth > 0 && initialHeight > 0) {
                const Render::FramebufferExtent extent{static_cast<std::uint32_t>(initialWidth), static_cast<std::uint32_t>(initialHeight)};
                if (const auto resized = frontend.Resize(extent); resized.HasError())
                    return resized;
                const auto prepared = frontend.PollPresentationAvailability(extent);
                if (prepared.HasError())
                    return Result<void>::Failure(prepared.ErrorValue());
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CreateEditorRenderComposition(SDL_Window &window, const EditorGuiOptions &options,
                                                                 EditorRenderComposition &composition) {
            Render::RenderBackendRegistry registry;

            if (const auto registered = RegisterEditorPresentationBackend(window, options, composition, registry); registered.HasError())
                return registered;

            if (const Result<void> sealed = registry.Seal(); sealed.HasError()) {
                return Result<void>::Failure(sealed.ErrorValue());
            }
            bool immediateOutput = options.exitAfterFirstFrame || options.exitAfterFrames > 0;
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
            immediateOutput = immediateOutput || composition.xrSession != nullptr;
#endif
            auto frontendResult =
                Render::RenderFrontend::Create(registry, Render::RenderBackendId{options.rendererBackend},
                                               Render::RenderBackendConfig{.requirePresentation = true,
                                                                           .enableValidation = false,
                                                                           .maxFramesInFlight = 2,
                                                                           .presentMode = immediateOutput ? Render::PresentMode::Immediate
                                                                                                          : Render::PresentMode::Fifo},
                                               Render::RenderResourceUploadLimits{},
                                               Render::RenderFrontendMemoryConfig{.budget = {.hardCapBytes = 1024U * 1024U * 1024U,
                                                                                             .defaultBlockBytes = 16U * 1024U * 1024U,
                                                                                             .maximumBlockBytes = 128U * 1024U * 1024U},
                                                                                  .defaultResourceScope =
                                                                                      RenderMemoryScopes::HostResources});
            if (frontendResult.HasError()) {
                return Result<void>::Failure(frontendResult.ErrorValue());
            }
            composition.frontend = std::move(frontendResult).Value();

            if (const auto prepared = PrepareInitialEditorPresentation(window, *composition.frontend); prepared.HasError())
                return prepared;

            if (const auto installed = InstallSelectedEditorHostRenderers(window, options, composition); installed.HasError())
                return installed;

            if (const Result<void> attached = composition.frontend->AttachStaticMeshPassExecutor(*composition.viewportRenderer);
                attached.HasError()) {
                return Result<void>::Failure(attached.ErrorValue());
            }
            return Result<void>::Success();
        }

        struct EditorPresentationPorts {
            SDL_Window *window;
            ImGuiIO &io;
            Render::RenderFrontend &renderFrontend;
            IEditorGuiRenderer &guiRenderer;
            IEditorViewportRenderer &viewportRenderer;
            Render::RenderTargetHandle viewportTarget;
            EditorRenderComposition &composition;
            std::optional<Application::Internal::RuntimeUiPaintPort> &uiRetirement;
        };

        struct EditorOperationServices {
            BuildOutputStore &buildOutputStore;
            OperationStore &operationStore;
            Application::ShaderBuildService &shaderBuild;
            std::shared_ptr<const Render::ShaderCompilerTargetDescriptor> uiPaintTarget;
        };

        struct EditorBackgroundServices {
            JobSystem &jobs;
            const EditorUpdateHostServices *updates;
            const EditorOpenXRHostServices *xrHost;
        };

        struct RunEditorMainLoopParams {
            const EditorGuiOptions &options;
            EditorTelemetry &telemetry;
            EditorPresentationPorts presentation;
            const Fonts &fonts;
            const EditorTextures &textures;
            ProjectCreationService &projectCreationService;
            EditorBackgroundServices background;
            EditorSettingsService &settings;
            EngineDataBus &engineEvents;
            EditorDataBus &editorEvents;
            LocalizationService &localization;
            const RendererAvailabilitySnapshot &rendererAvailability;
            GuiRoute initialRoute;
            EditorModalHost &modalHost;
            Input::SdlInputBackend &inputBackend;
            Input::InputRouter &inputRouter;
            const Log::IStructuredLogQuery &logQuery;
            EditorOperationServices operationServices;
            bool healthy{false}; /**< Set only after runtime startup and a normal, completed main-loop closure. */
        };

        /** @brief Owner-thread pacing state bound to the current editor host and its selected backend. */
        struct EditorPacingState {
            explicit EditorPacingState(Clock &hostClock) noexcept : clock(hostClock) {}

            Clock &clock;
            std::optional<CancellationToken> hostCancellation;
            Render::FramePacer pacer;
            std::optional<EditorPresentationSurface> surface;
            std::uint64_t presentingFrameNumber{};
            bool focused{};
            bool nativeFeedbackSupported{};
            Render::FramebufferExtent committedOutputExtent{};
        };

        /** @brief Borrowed lifecycle authorities; all remain bound until participant shutdown completes. */
        struct EditorRuntimeAuthorities final {
            Runtime::RuntimeHost *runtime;
            Application::Internal::PackageProjectHost *packages;
            EditorNavigationBakeHost *navigation;
            Application::Internal::RuntimeUiPaintPort ui;
        };

        /** @brief Value-owned prepared viewport/GUI workload and the mesh generation used to assemble it. */
        struct EditorPreparedPasses final {
            std::array<Render::RenderPassDescriptor, 2> descriptors{};
            std::size_t count{};
            std::uint64_t viewportMeshResourceGeneration{};
        };

        /** @brief Accepts process Quit or Close belonging to the explicitly borrowed primary host window. */
        [[nodiscard]] bool IsHostCloseRequest(const SDL_Event &event, SDL_Window *window) noexcept {
            if (event.type == SDL_EVENT_QUIT)
                return true;
            return event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window);
        }

        /** @brief Distinguishes unavailable native size queries from minimized/zero-size output without changing owner state. */
        Result<std::optional<Render::FramebufferExtent>> QuerySurfaceExtent(SDL_Window *window) {
            int width = 0;
            int height = 0;
            if (!SDL_GetWindowSizeInPixels(window, &width, &height))
                return Result<std::optional<Render::FramebufferExtent>>::Failure(
                    MakeEditorRendererError("editor.renderer.extent_unavailable", SDL_GetError()));
            if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0 || width <= 0 || height <= 0)
                return Result<std::optional<Render::FramebufferExtent>>::Success(std::nullopt);
            return Result<std::optional<Render::FramebufferExtent>>::Success(
                Render::FramebufferExtent{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)});
        }

        class EditorRuntimeParticipant final : public Runtime::RuntimeLifecycleParticipant, public EditorSurfaceRealizer {
        public:
            EditorRuntimeParticipant(RunEditorMainLoopParams &params, GuiScreenHost &screenHost,
                                     EditorViewportSceneState &viewportSceneState, EditorSettingsSnapshot &settingsSnapshot,
                                     EditorGuiLiveReload &liveGui, std::optional<EditorRendererRestartRequest> &rendererRestart,
                                     Clock &clock, const EditorRuntimeAuthorities &authorities)
                : p_(&params), screenHost_(&screenHost), viewportSceneState_(&viewportSceneState), settingsSnapshot_(&settingsSnapshot),
                  themeReload_(params.background.jobs, params.editorEvents), liveGui_(liveGui), rendererRestart_(&rendererRestart),
                  authorities_(authorities), pacing_{clock}
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
                  ,
                  qualification_{params.options.qualifySurfaceLifecycle}
#endif
            {
            }

            void BindHost(Runtime::RuntimeHost &host) noexcept {
                authorities_.runtime = &host;
            }

            Result<void> Startup(const CancellationToken &cancellation) override {
                if (auto liveUi = liveGui_.Start(BuildEditorGuiLiveReloadConfiguration()); liveUi.HasError())
                    return liveUi;
                pacing_.hostCancellation = cancellation;
                const auto timing = p_->presentation.renderFrontend.PollNativePresentTiming();
                if (timing.HasError() &&
                    timing.ErrorValue().code.Value() != Render::FramePacingErrors::NativeTimingUnsupported.code.Value())
                    return Result<void>::Failure(timing.ErrorValue());
                // The selected backend remains fixed for this participant's lifetime.
                pacing_.nativeFeedbackSupported = timing.HasValue();
                p_->telemetry.nativeFeedbackSupported.Set(pacing_.nativeFeedbackSupported ? 1.0 : 0.0);
                auto created = EditorPresentationSurface::Create(1, *this);
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                pacing_.surface.emplace(std::move(created).Value());
                return UpdatePacingSurface();
            }

            /** @brief Services the host cap before the scheduler samples its independent clock. */
            Result<void> WaitForFrame() {
                for (;;) {
                    SDL_PumpEvents();
                    if (SDL_HasEvent(SDL_EVENT_QUIT) || SDL_HasEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED))
                        return Result<void>::Success();  // Runtime event phase performs the actual close operation.
                    if (auto updated = UpdatePacingSurface(); updated.HasError())
                        return updated;
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
                    if (const auto *xr = p_->presentation.composition.xrPlay.get();
                        xr && xr->PresentationState() == Application::Internal::OpenXRPlayPresentation::FrameAdmitted)
                        return Result<void>::Success();  // SAME native xrWaitFrame later supplies selected-XR pacing.
#endif
                    const auto decision = pacing_.pacer.Poll(pacing_.clock.MonotonicNow(), *pacing_.hostCancellation,
                                                             screenHost_->IsApplicationCloseRequested());
                    if (decision.HasError())
                        return Result<void>::Failure(decision.ErrorValue());
                    if (decision.Value().disposition == Render::FramePacingDisposition::Ready)
                        return Result<void>::Success();
                    // A suspended surface still pumps runtime/events; it never admits native rendering.
                    const auto wait = decision.Value().disposition == Render::FramePacingDisposition::Suspended
                                          ? Duration::FromMilliseconds(2)
                                          : decision.Value().wait;
                    SDL_DelayNS(static_cast<Uint64>(wait.ToNanoseconds()));
                    if (decision.Value().disposition == Render::FramePacingDisposition::Suspended)
                        return Result<void>::Success();
                }
            }

            Result<void> OnPhase(const Runtime::RuntimePhase phase, const Runtime::FrameContext &context) override {
                switch (phase) {
                    case Runtime::RuntimePhase::BeginFrame:
#if !defined(NDEBUG)
                        themeReload_.Update(static_cast<double>(context.variableDelta.ToNanoseconds()) / 1'000'000'000.0);
#endif
                        liveGui_.Update(static_cast<double>(context.variableDelta.ToNanoseconds()) / 1'000'000'000.0);
                        p_->inputBackend.BeginFrame(frameState_.inputFrameNumber++);
                        return Result<void>::Success();
                    case Runtime::RuntimePhase::PollPlatformEvents:
                        return PollPlatformEvents();
                    case Runtime::RuntimePhase::BuildInputSnapshot:
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
                        if (auto *xr = p_->presentation.composition.xrPlay.get()) {
                            const auto committed = xr->BuildInputSnapshot(context, p_->inputBackend.Commit());
                            if (committed.HasError())
                                return committed;
                        } else
#endif
                        {
                            p_->inputRouter.BeginFrame(p_->inputBackend.Commit());
                        }
                        screenHost_->OnInputSnapshot();
                        return Result<void>::Success();
                    case Runtime::RuntimePhase::ApplyQueuedOwnerThreadCommands:
                        return Result<void>::Success();
                    case Runtime::RuntimePhase::VariableUpdate:
                        return UpdatePresentation(context);
                    case Runtime::RuntimePhase::RenderExtraction:
                        return ExtractFrame();
                    case Runtime::RuntimePhase::RenderExecution:
                        return ExecuteFrame(context);
                    case Runtime::RuntimePhase::RenderGui:
                        if (!frameState_.frame.has_value())
                            return Result<void>::Success();
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
                        qualification_.RecordFrameWork(EditorSurfaceLifecycleQualification::FrameWork::GuiRender);
#endif
                        return ComposeGuiFrame();
                    case Runtime::RuntimePhase::Presentation:
                        return PresentFrame();
                    case Runtime::RuntimePhase::CommitDeferredLifecycleChanges:
                        return Result<void>::Success();
                    case Runtime::RuntimePhase::EndFrame:
                        return EndRuntimeFrame(context);
                    case Runtime::RuntimePhase::NetworkPoll:
                    case Runtime::RuntimePhase::NetworkFlush:
                    case Runtime::RuntimePhase::FixedUpdate:
                        break;
                }
                return Result<void>::Success();
            }

            Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override {
                screenHost_->OnFixedUpdate(context.simulationTick,
                                           static_cast<double>(context.fixedDelta.ToNanoseconds()) / 1'000'000'000.0);
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                themeReload_.Shutdown();
                try {
                    static_cast<void>(authorities_.packages->Close());
                } catch (...) {
                    // Retain the package owner until scheduler cancellation joins borrowed callbacks.
                    std::fputs("Package close raised before scheduler cancellation; retaining the owner.\n", stderr);
                }
                p_->background.jobs.StopAccepting();
                static_cast<void>(pacing_.pacer.Stop());
                try {
                    const auto canceled = CancelPreparedXrFrame();
                    if (canceled.HasError())
                        LOG_ERROR("editor.xr", "XR shutdown retains retirement debt: %s", canceled.ErrorValue().message.c_str());
                } catch (...) {
                    // Keep the app-owned XR/device parents for checked presentation teardown; never fake retirement here.
                    frameState_.frame.reset();
                    std::fputs("XR cancellation raised during shutdown; retaining native retirement ownership.\n", stderr);
                }
                if (frameState_.guiFrameStarted) {
                    ImGui::EndFrame();
                    frameState_.guiFrameStarted = false;
                }
                frameState_.extractedFrameReady = false;
                if (pacing_.surface) {
                    const auto closed = pacing_.surface->Close();
                    surfaceClosed_ = closed.HasValue();
                    if (closed.HasError()) {
                        const Error &error = closed.ErrorValue();
                        LOG_ERROR("editor.renderer.surface", "Surface Close failed [%s/%s]: %s", error.domain.Value().c_str(),
                                  error.code.Value().c_str(), error.message.c_str());
                    }
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
                    qualificationPassed_ = closed.HasValue() && qualification_.Complete(closed.Value());
                    if (p_->options.qualifySurfaceLifecycle && qualificationPassed_)
                        std::puts("HoroEditor surface lifecycle qualification passed");
#endif
                }
                *rendererRestart_ = screenHost_->RendererRestartRequest();
                p_->inputRouter.CancelCapture(Input::CaptureCancellationReason::OwnerDestroyed);
                p_->modalHost.ForceDetachAllForShutdown();
                screenHost_->Shutdown();
                p_->operationServices.shaderBuild.Shutdown();
                p_->background.jobs.Shutdown(ShutdownPolicy::Cancel);
                (void)screenHost_->GameplayRetirements()->Close();
                try {
                    if (authorities_.packages->Close().HasError())
                        std::terminate();
                } catch (...) {
                    std::terminate();
                }
            }

            /** @brief Reports actual successful native presentation, excluding acquisition-pending runtime ticks. */
            [[nodiscard]] bool HasPresentedFrame() const noexcept {
                return presentedFrames_ != 0;
            }

            /** @brief Reports whether shutdown published Close before backend teardown. */
            [[nodiscard]] bool SurfaceClosed() const noexcept {
                return surfaceClosed_;
            }

#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
            /** @brief Returns the real shutdown qualification result after RuntimeHost::Shutdown. */
            [[nodiscard]] bool QualificationPassed() const noexcept {
                return !p_->options.qualifySurfaceLifecycle || qualificationPassed_;
            }
#endif

        private:
            /** @brief Qualifies the completed frame before telemetry and bounded-process exit admission. */
            Result<void> EndRuntimeFrame(const Runtime::FrameContext &context) {
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
                if (auto *xr = p_->presentation.composition.xrPlay.get()) {
                    const auto tail = xr->ProgressRenderTail();
                    if (tail.HasError())
                        return Result<void>::Failure(tail.ErrorValue());
                }
#endif
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
                if (const auto checked = QualifySurfaceFrame(); checked.HasError())
                    return checked;
#endif
                if (context.frameNumber % 60U == 1U) {
                    p_->telemetry.frameNumber.Set(static_cast<double>(context.frameNumber));
                    p_->telemetry.frameDuration.Set(static_cast<double>(context.variableDelta.ToNanoseconds()) / 1'000'000'000.0);
                    const Telemetry::Statistics statistics = Telemetry::Runtime::GetStatistics();
                    p_->telemetry.droppedRecords.Set(static_cast<double>(statistics.droppedRecords));
                    p_->telemetry.sinkFailures.Set(static_cast<double>(statistics.sinkFailures));
                }
                if ((p_->options.exitAfterFirstFrame && HasPresentedFrame()) ||
                    (p_->options.exitAfterFrames > 0 && presentedFrames_ >= p_->options.exitAfterFrames)) {
                    static_cast<void>(screenHost_->RequestCloseApplication());
                }
                return Result<void>::Success();
            }

            /** @brief Invokes the retained frontend resize only from the frozen surface safe point. */
            Result<void> Resize(const Render::FramebufferExtent extent) override {
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
                if (const auto admitted = qualification_.BeforeResize(); admitted.HasError())
                    return admitted;
#endif
                return p_->presentation.renderFrontend.Resize(extent);
            }

#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
            /** @brief Inspects the actual participant after all presentation phases without creating a parallel host. */
            Result<void> QualifySurfaceFrame() {
                const auto snapshot = pacing_.surface->Snapshot();
                if (snapshot.HasError())
                    return Result<void>::Failure(snapshot.ErrorValue());
                return qualification_.EndFrame(snapshot.Value());
            }
#endif

            Result<void> PollPlatformEvents() {
                while (const std::optional<EditorMenuInvocation> invocation = PollNativeEditorMenuAction()) {
                    screenHost_->DispatchMenuInvocation(*invocation);
                }
                p_->projectCreationService.PumpMainThread();
                p_->engineEvents.DispatchQueued();
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
                if (const auto prepared = qualification_.BeforeEvents(*p_->presentation.window); prepared.HasError())
                    return prepared;
#endif
                SDL_Event event;
                while (SDL_PollEvent(&event))
                    DispatchPlatformEvent(event);

#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
                if (auto *xr = p_->presentation.composition.xrPlay.get()) {
                    if (auto events = xr->PollEvents(); events.HasError())
                        return events;
                }
#endif
                if (!nativeMenuInstalled_) {
                    InstallNativeEditorMenuBar(GetEditorMenuModel(), p_->localization);
                    nativeMenuInstalled_ = true;
                }
                return Result<void>::Success();
            }

            /** @brief Routes one event while retaining window-specific recovery and close ownership. */
            void DispatchPlatformEvent(const SDL_Event &event) {
                ObserveSurfaceRecoveryEvent(event);
                ImGui_ImplSDL3_ProcessEvent(&event);
                p_->inputBackend.ProcessEvent(event);

                // SDL3 drag-and-drop: forward dropped file paths to the active import modal.
                if (event.type == SDL_EVENT_DROP_FILE) {
                    const char *droppedPath = event.drop.data;
                    if (droppedPath && droppedPath[0]) {
                        screenHost_->HandleDropFiles({std::filesystem::path(droppedPath)});
                    }
                }

                if (IsHostCloseRequest(event, p_->presentation.window)) {
                    static_cast<void>(screenHost_->RequestCloseApplication());
                    authorities_.runtime->RequestShutdown();
                }
            }

            /** @brief Admits one recovery attempt only after an owner-window presentation observation. */
            void ObserveSurfaceRecoveryEvent(const SDL_Event &event) {
                switch (event.type) {
                    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    case SDL_EVENT_WINDOW_RESTORED:
                    case SDL_EVENT_WINDOW_EXPOSED:
                        if (event.window.windowID == SDL_GetWindowID(p_->presentation.window))
                            surfaceRecovery_ = EditorSurfaceRecovery::Retry;
                        break;
                    default:
                        break;
                }
            }

            Result<void> UpdatePresentation(const Runtime::FrameContext &context) {
                const auto surface = Theme::Bg0();
                const auto text = Theme::Text();
                windowChrome_.Update(*p_->presentation.window, {{surface.x, surface.y, surface.z}, {text.x, text.y, text.z}});
                if (const auto updated = UpdatePacingSurface(); updated.HasError())
                    return updated;
                const auto snapshot = pacing_.surface->Snapshot();
                if (snapshot.HasError())
                    return Result<void>::Failure(snapshot.ErrorValue());
                frameState_.extractedFrameReady = false;
                frameState_.focusedWidgetInputContext.Reset();
                if (!frameState_.nativePresentationPending && snapshot.Value().state == Render::RenderSurfaceState::Ready) {
                    // Consume the preceding viewport request before Draw captures its current image identity.
                    if (const auto prepared = PrepareViewportResources(); prepared.HasError())
                        return prepared;
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
                    qualification_.RecordFrameWork(EditorSurfaceLifecycleQualification::FrameWork::GuiBegin);
#endif
                    if (const Result<void> guiBegun = p_->presentation.guiRenderer.BeginFrame(); guiBegun.HasError()) {
                        return guiBegun;
                    }
                    ImGui::NewFrame();
                    frameState_.guiFrameStarted = true;

                    if (p_->presentation.io.WantTextInput) {
                        frameState_.focusedWidgetInputContext = p_->inputRouter.PushContext(Input::InputContextId{"editor.focused_widget"},
                                                                                            Input::InputContextKind::FocusedGuiWidget);
                    }
                }

                *settingsSnapshot_ = p_->settings.Snapshot();
                if (settingsSnapshot_->revision != appliedKeymapRevision_) {
                    const auto applied = p_->inputRouter.SetProfile(BuiltinEditorInputProfile(settingsSnapshot_->settings.keymap.profile));
                    if (applied.HasError())
                        LOG_ERROR("editor.input", "Unable to activate editor keymap: %s", applied.ErrorValue().message.c_str());
                    appliedKeymapRevision_ = settingsSnapshot_->revision;
                }
                const float deltaSeconds = static_cast<float>(context.variableDelta.ToNanoseconds()) / 1'000'000'000.0F;
                p_->modalHost.OnUpdate(deltaSeconds);
                screenHost_->OnUpdate(deltaSeconds);
                // Independent owner pumping continues after modal closure or project route departure.
                static_cast<void>(authorities_.packages->Pump(Packages::PackageActivationBoundary::Running));
                authorities_.navigation->Pump();
                return Result<void>::Success();
            }

            /** @brief Draws only the modal-owned dim surface without changing extraction or modal ownership. */
            void DrawModalBackdrop() {
                if (const EditorModal *const modal = p_->modalHost.TopModal(); modal != nullptr && modal->Presentation().dimWorkspace) {
                    const ImGuiViewport *viewport = ImGui::GetMainViewport();
                    ImGui::SetNextWindowPos(viewport->Pos);
                    ImGui::SetNextWindowSize(viewport->Size);
                    ImGui::SetNextWindowBgAlpha(0.58F);
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
                    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.0F, 0.0F, 0.0F, 1.0F});
                    ImGui::Begin("##ModalWorkspaceBackdrop", nullptr,
                                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
                    ImGui::End();
                    ImGui::PopStyleColor();
                    ImGui::PopStyleVar();
                }
            }

            Result<void> ExtractFrame() {
                if (!frameState_.guiFrameStarted)
                    return Result<void>::Success();

                GuiLive::Invoke<&GuiScreenHost::Draw>(liveGui_.Registry(), *screenHost_);
                DrawModalBackdrop();
                p_->modalHost.Draw(liveGui_.Registry());
                ImGui::Render();
                frameState_.guiFrameStarted = false;
                // Draw may synchronously start Play; its cold Scene/UI preparation still precedes native admission.
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
                qualification_.RecordFrameWork(EditorSurfaceLifecycleQualification::FrameWork::Extraction);
#endif
                frameState_.preparedPasses.count = 0;
                const EditorViewportSceneView viewportScene = viewportSceneState_->View();
                if (const EditorViewportExtent viewportExtent = p_->presentation.viewportRenderer.RequestedExtent();
                    !UsesXrPresentation() &&
                    frameState_.preparedPasses.viewportMeshResourceGeneration == viewportSceneState_->MeshResourceGeneration() &&
                    CanSubmitViewportPass(viewportExtent)) {
                    if (const Result<void> resized = ResizeLegacyViewportTarget(viewportExtent); resized.HasError())
                        return resized;
                    frameState_.preparedPasses.descriptors[frameState_.preparedPasses.count++] =
                        Render::RenderPassDescriptor{.id = Render::RenderPassId{1},
                                                     .kind = Render::RenderPassKind::Graphics,
                                                     .staticMesh = Render::
                                                         StaticMeshPassDescriptor{.target = p_->presentation.viewportTarget,
                                                                                  .extent = {viewportExtent.width, viewportExtent.height},
                                                                                  .scene =
                                                                                      Render::RenderSceneView{.camera = ToRenderCamera(
                                                                                                                  viewportScene.camera),
                                                                                                              .meshResources =
                                                                                                                  viewportScene
                                                                                                                      .meshResources,
                                                                                                              .instances =
                                                                                                                  viewportScene.instances,
                                                                                                              .lights =
                                                                                                                  viewportScene.lights}}};
                }
                frameState_.preparedPasses.descriptors[frameState_.preparedPasses.count++] =
                    Render::RenderPassDescriptor{.id = Render::RenderPassId{2},
                                                 .kind = Render::RenderPassKind::Graphics,
                                                 .primaryOutput =
                                                     Render::PrimaryOutputAttachment{.loadOperation =
                                                                                         Render::AttachmentLoadOperation::Clear,
                                                                                     .storeOperation =
                                                                                         Render::AttachmentStoreOperation::Store,
                                                                                     .clearColor =
                                                                                         Render::ClearColor{Theme::Bg0().x, Theme::Bg0().y,
                                                                                                            Theme::Bg0().z, 1.0F}}};
                frameState_.extractedFrameReady = true;
                return Result<void>::Success();
            }

            /** @brief Resets scheduling when focus changes without changing renderer or simulation clock. */
            Result<void> UpdateSurfaceFocus() {
                const auto flags = SDL_GetWindowFlags(p_->presentation.window);
                if (const bool focused = (flags & SDL_WINDOW_INPUT_FOCUS) != 0; focused != pacing_.focused) {
                    pacing_.focused = focused;
                    if (auto reset = pacing_.pacer.Reset(); reset.HasError())
                        return reset;
                }
                return Result<void>::Success();
            }

            /** @brief Resolves the immutable request already admitted by the selected native backend at startup. */
            Render::PresentMode SurfacePresentMode() const noexcept {
                using enum Render::PresentMode;
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
                if (p_->presentation.composition.xrSession)
                    return Immediate;  // Negotiated before acquisition; desktop FIFO cannot pace an XR mirror.
#endif
                return p_->options.exitAfterFirstFrame || p_->options.exitAfterFrames > 0 ? Immediate : Fifo;
            }

            /** @brief Samples cancellation before active-frame ownership at the native safe point. */
            EditorSurfaceAdmission SurfaceAdmission() const noexcept {
                using enum EditorSurfaceAdmission;
                if (pacing_.hostCancellation && pacing_.hostCancellation->IsCancellationRequested())
                    return Cancelled;
                if (frameState_.frame)
                    return FrameActive;
                return Idle;
            }

            /** @brief Reports the unchanged native diagnostic; qualification may reject lost error/cause evidence. */
            Result<void> ReportNativeSurfaceFailure(const Error &error) {
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
                if (const auto checked = qualification_.ObserveFailure(error); checked.HasError())
                    return checked;
#endif
                LOG_ERROR("editor.renderer.surface", "Native surface lost [%s/%s]: %s; awaiting an owner-window recovery event",
                          error.domain.Value().c_str(), error.code.Value().c_str(), error.message.c_str());
                return Result<void>::Success();
            }

            /** @brief Keeps native surface loss event-pumpable while preserving its exact typed diagnostic. */
            Result<void> UpdatePacingSurface() {
                if (const auto focused = UpdateSurfaceFocus(); focused.HasError())
                    return focused;
                const auto extent = QuerySurfaceExtent(p_->presentation.window);
                if (extent.HasError())
                    return Result<void>::Failure(extent.ErrorValue());
                frameState_.nativePresentationPending = false;
                const auto mode = SurfacePresentMode();
                auto observed = pacing_.surface->Observe(extent.Value(), {mode, mode, Render::PresentModeResolution::Exact, 0},
                                                         SurfaceAdmission(), std::exchange(surfaceRecovery_, EditorSurfaceRecovery::Defer));
                if (observed.HasError())
                    return Result<void>::Failure(observed.ErrorValue());
                if (observed.Value().nativeFailure) {
                    if (const auto reported = ReportNativeSurfaceFailure(*observed.Value().nativeFailure); reported.HasError())
                        return reported;
                }
                if (observed.Value().snapshot.active)
                    pacing_.committedOutputExtent = observed.Value().snapshot.active->extent;
                // Realize the surface's frozen generation before borrowing a native drawable.
                // Pending resize retirement must never acquire from the superseded chain.
                if (observed.Value().snapshot.state == Render::RenderSurfaceState::Ready && observed.Value().snapshot.active &&
                    SurfaceAdmission() == EditorSurfaceAdmission::Idle) {
                    const auto available =
                        p_->presentation.renderFrontend.PollPresentationAvailability(observed.Value().snapshot.active->extent);
                    if (available.HasError()) {
                        const auto &error = available.ErrorValue();
                        if (error.domain.Value() != Render::RenderPresentationErrors::DrawableUnavailable.domain.Value() ||
                            error.code.Value() != Render::RenderPresentationErrors::DrawableUnavailable.code.Value())
                            return Result<void>::Failure(error);
                        frameState_.nativePresentationPending = true;
                    } else {
                        frameState_.nativePresentationPending = !available.Value();
                    }
                }
                if (observed.Value().snapshot.state == Render::RenderSurfaceState::Ready &&
                    SurfaceAdmission() == EditorSurfaceAdmission::Idle && !frameState_.nativePresentationPending &&
                    (authorities_.ui.Phase() == Application::Internal::RuntimeUiPaintHostPhase::Active ||
                     authorities_.ui.Phase() == Application::Internal::RuntimeUiPaintHostPhase::NoSource)) {
                    const auto output = CaptureRuntimeUiOutput(*p_->presentation.window, pacing_.committedOutputExtent);
                    if (output.HasError())
                        return Result<void>::Failure(output.ErrorValue());
                    const auto updated = authorities_.ui.UpdateOutput(output.Value());
                    if (updated.HasError())
                        return updated;
                }
                return pacing_.pacer.Configure({p_->options.frameRateLimit}, observed.Value().snapshot);
            }

            Result<bool> BeginRenderFrame(const Runtime::FrameContext &context) {
                // The output was frozen before GUI CPU work. Recheck only availability, never resize/reproject after extraction.
                const auto available = p_->presentation.renderFrontend.PollPresentationAvailability(pacing_.committedOutputExtent);
                if (available.HasError()) {
                    const auto &error = available.ErrorValue();
                    if (error.domain.Value() == Render::RenderPresentationErrors::DrawableUnavailable.domain.Value() &&
                        error.code.Value() == Render::RenderPresentationErrors::DrawableUnavailable.code.Value())
                        return Result<bool>::Success(false);
                    return Result<bool>::Failure(error);
                }
                if (!available.Value())
                    return Result<bool>::Success(false);
                const Render::FramebufferExtent outputExtent = pacing_.committedOutputExtent;
                pacing_.presentingFrameNumber = context.frameNumber;

#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
                qualification_.RecordFrameWork(EditorSurfaceLifecycleQualification::FrameWork::NativeBegin);
#endif
                auto begun = p_->presentation.renderFrontend.BeginFrame(
                    Render::FrameDescriptor{.frameNumber = context.frameNumber, .outputExtent = outputExtent});
                if (begun.HasError() && IsDeferredFrameAdmission(begun.ErrorValue()))
                    return Result<bool>::Success(false);
                if (begun.HasError())
                    return Result<bool>::Failure(begun.ErrorValue());
                frameState_.frame.emplace(std::move(begun).Value());
                return Result<bool>::Success(true);
            }

            /** @brief Observes existing app-owned Scene/debt selection; never infers native readiness from a GUI projection. */
            [[nodiscard]] bool UsesXrPresentation() const noexcept {
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
                const auto *xr = p_->presentation.composition.xrPlay.get();
                return xr && xr->PresentationState() != Application::Internal::OpenXRPlayPresentation::Inactive;
#else
                return false;
#endif
            }

            Result<void> PrepareViewportResources() {
                if (UsesXrPresentation())
                    return Result<void>::Success();
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
                qualification_.RecordFrameWork(EditorSurfaceLifecycleQualification::FrameWork::ViewportResources);
#endif
                const EditorViewportSceneView viewportScene = viewportSceneState_->View();
                auto prepared =
                    p_->presentation.viewportRenderer.PrepareResources(p_->presentation.renderFrontend,
                                                                       Render::RenderSceneView{.camera =
                                                                                                   ToRenderCamera(viewportScene.camera),
                                                                                               .meshResources = viewportScene.meshResources,
                                                                                               .instances = viewportScene.instances,
                                                                                               .lights = viewportScene.lights});
                if (prepared.HasError())
                    return Result<void>::Failure(prepared.ErrorValue());
                frameState_.preparedPasses.viewportMeshResourceGeneration = viewportSceneState_->MeshResourceGeneration();
                if (prepared.Value().has_value())
                    p_->presentation.viewportTarget = *prepared.Value();
                return Result<void>::Success();
            }

            [[nodiscard]] bool CanSubmitViewportPass(const EditorViewportExtent extent) const noexcept {
                return extent.IsValid() && p_->presentation.viewportTarget.IsValid() && p_->presentation.viewportRenderer.IsReady();
            }

            Result<void> ResizeLegacyViewportTarget(const EditorViewportExtent extent) {
                if (p_->presentation.renderFrontend.Capabilities().supportsRenderTargetResources)
                    return Result<void>::Success();
                return p_->presentation.renderFrontend.ResizeOffscreenTarget(p_->presentation.viewportTarget,
                                                                             {extent.width, extent.height});
            }

            /** @brief Cancels only this prepared acquisition; the retained teardown owner keeps all native/queue debt. */
            Result<void> CancelPreparedXrFrame() {
                Result<void> canceled = Result<void>::Success();
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
                if (std::exchange(frameState_.xrFramePrepared, false))
                    canceled = p_->presentation.composition.xrPlay->FinishGui(false);
#endif
                frameState_.frame.reset();
                return canceled;
            }

            /** @brief Preserves the triggering error when runtime failure immediately enters Shutdown instead of EndFrame. */
            Result<void> FailRenderFrame(const Error &error) {
                const auto canceled = CancelPreparedXrFrame();
                return Result<void>::Failure(canceled.HasError() ? WithCause(error, canceled.ErrorValue()) : error);
            }

            Result<void> ExecuteFrame(const Runtime::FrameContext &context) {
                const bool extracted = std::exchange(frameState_.extractedFrameReady, false);
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
                if (UsesXrPresentation()) {
                    auto &xr = *p_->presentation.composition.xrPlay;
                    if (xr.PresentationState() != Application::Internal::OpenXRPlayPresentation::FrameAdmitted)
                        return Result<void>::Success();  // Preparation/retirement/native Idle is never a desktop camera fallback.
                    const auto prepared = xr.PrepareRenderFrame(context.frameNumber);
                    if (prepared.HasError())
                        return Result<void>::Failure(prepared.ErrorValue());
                    if (!prepared.Value())
                        return Result<void>::Success();
                    frameState_.xrFramePrepared = true;
                }
#endif
                if (!extracted)
                    return CancelPreparedXrFrame();  // Minimized/unavailable mirror still services native pacing with a zero-layer tail.
                auto begun = BeginRenderFrame(context);
                if (begun.HasError())
                    return FailRenderFrame(begun.ErrorValue());
                if (!begun.Value())
                    return CancelPreparedXrFrame();
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
                qualification_.RecordFrameWork(EditorSurfaceLifecycleQualification::FrameWork::Execution);
#endif
                Result<void> scene = Result<void>::Success();
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
                if (frameState_.xrFramePrepared) {
                    if (frameState_.preparedPasses.count == 0 ||
                        !frameState_.preparedPasses.descriptors[frameState_.preparedPasses.count - 1].primaryOutput)
                        return FailRenderFrame(MakeEditorRendererError("editor.xr.output_unavailable",
                                                                       "The selected XR mirror has no admitted primary output."));
                    scene = p_->presentation.composition.xrPlay->ExecuteRenderFrame(*frameState_.frame,
                                                                                    *frameState_.preparedPasses
                                                                                         .descriptors[frameState_.preparedPasses.count - 1]
                                                                                         .primaryOutput);
                } else
#endif
                {
                    scene = p_->presentation.viewportRenderer
                                .ExecutePreparedFrame(*frameState_.frame,
                                                      std::span<const Render::RenderPassDescriptor>{frameState_.preparedPasses.descriptors
                                                                                                        .data(),
                                                                                                    frameState_.preparedPasses.count});
                }
                if (scene.HasError())
                    return FailRenderFrame(scene.ErrorValue());
                const auto ui = authorities_.ui.ComposeUi(*frameState_.frame, context.frameNumber);
                return ui.HasError() ? FailRenderFrame(ui.ErrorValue()) : Result<void>::Success();
            }

            /** @brief Resolves the CPU draw identity against this exact acquired eye before the SAME GUI queue tail. */
            Result<void> ComposeGuiFrame() {
                auto texture = p_->presentation.viewportRenderer.TextureView();
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
                if (frameState_.xrFramePrepared) {
                    texture = p_->presentation.composition.xrPlay->MirrorView(pacing_.presentingFrameNumber);
                    if (!texture.IsValid() || !texture.sampledView.IsValid())
                        return FailRenderFrame(MakeEditorRendererError("editor.xr.mirror_unavailable",
                                                                       "The selected XR mirror acquisition is stale or unavailable."));
                }
#endif
                p_->presentation.guiRenderer.BindViewportTexture(texture);
                if (const auto gui =
                        frameState_.frame->ComposeGui(p_->presentation.guiRenderer, pacing_.hostCancellation.value_or(CancellationToken{}));
                    gui.HasError())
                    return FailRenderFrame(gui.ErrorValue());
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
                if (std::exchange(frameState_.xrFramePrepared, false)) {
                    const auto accepted = p_->presentation.composition.xrPlay->FinishGui(true);
                    if (accepted.HasError())
                        return FailRenderFrame(accepted.ErrorValue());
                }
#endif
                return Result<void>::Success();
            }

            /** @brief Observes the actual native present at its owner boundary, before host timing is committed. */
            Result<void> PresentNativeFrame(const Render::PresentationTimingRequest *timing) {
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
                qualification_.RecordFrameWork(EditorSurfaceLifecycleQualification::FrameWork::Presentation);
#endif
                Result<void> result = frameState_.frame->Present(timing);
                if (const auto uiReceipt = authorities_.ui.Presented(pacing_.presentingFrameNumber, result.HasValue());
                    uiReceipt.HasError())
                    return result.HasError() ? Result<void>::Failure(WithCause(uiReceipt.ErrorValue(), result.ErrorValue())) : uiReceipt;
                if (result.HasValue() && presentedFrames_ != (std::numeric_limits<std::uint64_t>::max)())
                    ++presentedFrames_;
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
                qualification_.RecordPresent(result.HasValue());
#endif
                return result;
            }

            Result<void> PresentFrame() {
                if (!frameState_.frame.has_value())
                    return Result<void>::Success();
                const auto surface = pacing_.surface->Snapshot();
                if (surface.HasError())
                    return Result<void>::Failure(surface.ErrorValue());
                const Render::PresentationTimingRequest timingRequest{surface.Value().surface, pacing_.presentingFrameNumber,
                                                                      pacing_.clock};
                const Duration start = pacing_.clock.MonotonicNow();
                Result<void> result = PresentNativeFrame(pacing_.nativeFeedbackSupported ? &timingRequest : nullptr);
                const Duration end = pacing_.clock.MonotonicNow();
                frameState_.frame.reset();
                std::optional<Render::NativePresentTiming> native;
                if (result.HasValue() && pacing_.nativeFeedbackSupported) {
                    auto timing = p_->presentation.renderFrontend.PollNativePresentTiming();
                    if (timing.HasError())
                        return Result<void>::Failure(timing.ErrorValue());
                    native = timing.Value();
                }
                if (auto recorded = pacing_.pacer.RecordPresent(pacing_.presentingFrameNumber, start, end, result.HasValue(), native,
                                                                pacing_.clock.MonotonicNow());
                    recorded.HasError())
                    return result.HasError() ? Result<void>::Failure(WithCause(recorded.ErrorValue(), result.ErrorValue())) : recorded;
                const auto statistics = pacing_.pacer.Statistics();
                if (statistics.HasError())
                    return Result<void>::Failure(statistics.ErrorValue());
                p_->telemetry.presentCallDuration.Set(static_cast<double>(statistics.Value().lastPresentCallDuration.ToNanoseconds()) /
                                                      1'000'000'000.0);
                p_->telemetry.pacingMissedDeadlines.Set(static_cast<double>(statistics.Value().missedDeadlines));
                return result;
            }

            RunEditorMainLoopParams *p_{};
            GuiScreenHost *screenHost_{};
            EditorViewportSceneState *viewportSceneState_{};
            EditorSettingsSnapshot *settingsSnapshot_{};
            std::uint64_t appliedKeymapRevision_{};
            Theme::LiveReload themeReload_;
            EditorWindowChrome windowChrome_;
            EditorGuiLiveReload &liveGui_;
            std::optional<EditorRendererRestartRequest> *rendererRestart_{};
            EditorRuntimeAuthorities authorities_;
            EditorPacingState pacing_;
            bool surfaceClosed_{};
            std::uint64_t presentedFrames_{};
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
            EditorSurfaceLifecycleQualification qualification_;
            bool qualificationPassed_{};
#endif
            EditorSurfaceRecovery surfaceRecovery_{EditorSurfaceRecovery::Defer};

            /** @brief One frame's native admission, GUI extraction and prepared scope, retaining the original lifetime order. */
            struct FrameState final {
                bool nativePresentationPending{false};
                bool guiFrameStarted{};     /**< ImGui CPU frame requiring Render or EndFrame, independent of native scope. */
                bool xrFramePrepared{};     /**< Exact acquired-frame GUI notification debt; no separate native authority. */
                bool extractedFrameReady{}; /**< Completed extraction eligible for one native admission. */
                Input::FrameNumber inputFrameNumber{1};
                Input::InputContextToken focusedWidgetInputContext;
                EditorPreparedPasses preparedPasses;
                std::optional<Render::RenderFrameScope> frame;
            };

            FrameState frameState_;
            bool nativeMenuInstalled_{false};
        };

        /** @brief Applies host admission before the runtime samples its independent simulation clock. */
        [[nodiscard]] Result<void> RunPacedEditorFrame(EditorRuntimeParticipant &participant, Runtime::RuntimeHost &runtime) {
            if (const Result<void> paced = participant.WaitForFrame(); paced.HasError())
                return paced;
            return runtime.RunFrame();
        }

        /** @brief Binds an authenticated installed-host update context before the settings view is created. */
        void ConfigureUpdateExperience(RunEditorMainLoopParams &p, EditorGuiContext &guiContext,
                                       std::optional<UpdateExperienceSession> &session) {
            if (!p.background.updates)
                return;
            session.emplace(p.background.jobs, p.background.updates->backend);
            if (const auto outcome = p.background.updates->verifiedOutcome; outcome) {
                const EditorUpdatePhase phase =
                    *outcome == EditorVerifiedUpdateOutcome::Active ? EditorUpdatePhase::Active : EditorUpdatePhase::RolledBack;
                static_cast<void>(session->ReportVerifiedHostOutcome(phase));
            }
            guiContext.updates = &*session;
        }

        /** @brief Requests only a staged helper handoff during an orderly editor exit. */
        void RequestUpdateActivationOnExit(std::optional<UpdateExperienceSession> &session, const bool closing,
                                           const bool restartingRenderer) {
            if (!session || !closing || restartingRenderer)
                return;
            const Result<bool> handoff = session->ActivateOnExit();
            if (handoff.HasError())
                LOG_ERROR("editor.update", "Install-on-exit handoff failed: %s", handoff.ErrorValue().message.c_str());
        }

        /** @brief Composes cached graph inspection at the application host boundary. */
        void RegisterInspectionWorkspacePanels(WorkspacePanelRegistry &registry, const Render::RenderFrontend &frontend) {
            const std::array<GlobalDockPaneFactory, 1> graphInspectionPanes{[&frontend] {
                return MakeRenderGraphInspectionPane([&frontend] {
                    return frontend.GraphInspectionSnapshot();
                });
            }};
            RegisterDefaultWorkspacePanels(registry, graphInspectionPanes);
        }

        /** @brief Shuts down the runtime and collects revoked UI owners before preserving the host's prior health. */
        bool CompleteEditorRuntimeShutdown(Runtime::RuntimeHost &runtime, const EditorRuntimeParticipant &participant, const bool healthy,
                                           const Application::Internal::RuntimeUiPaintPort &uiPort) {
            runtime.Shutdown();
            bool completed = participant.SurfaceClosed();
#if defined(HORO_EDITOR_SURFACE_QUALIFICATION)
            completed = completed && participant.QualificationPassed();
#endif
            if (const auto uiDrained = uiPort.CollectRetired(); uiDrained.HasError() || !uiDrained.Value()) {
                LOG_ERROR("editor.runtime", "Runtime UI retirement has not reached quiescence.");
                return false;
            }
            return healthy && completed;
        }

        /** @brief Creates one complete native workspace read pair at the application composition boundary. */
        Result<WorkspaceReadSources> CaptureNativeWorkspaceSources(const std::filesystem::path &root) {
            std::array<std::uint8_t, 16> bytes{};
            const auto entropy = Platform::CreateNativeSecureRandomSource();
            if (const auto generated = entropy->Fill(std::as_writable_bytes(std::span(bytes))); generated.HasError())
                return Result<WorkspaceReadSources>::Failure(generated.ErrorValue());
            const auto owner = Assets::AssetId::FromBytes(bytes);
            if (!owner.IsValid())
                return Result<WorkspaceReadSources>::Failure(MakeError(Platform::ProjectReadErrors::Invalid));
            auto registry = CreateNativeAssetRegistrySource(root, owner.ToString(), 1);
            if (registry.HasError())
                return Result<WorkspaceReadSources>::Failure(registry.ErrorValue());
            auto world = Platform::CreateNativeProjectReadFiles(root, owner.ToString(), 1);
            if (world.HasError())
                return Result<WorkspaceReadSources>::Failure(world.ErrorValue());
            return Result<WorkspaceReadSources>::Success({owner, std::move(registry).Value(), std::move(world).Value()});
        }

        /** @brief Captures explicit cooked selection and Physics mode before constructing Scene owners. */
        [[nodiscard]] Result<Application::Internal::EditorProjectScenePolicy> CaptureEditorScenePolicy(const EditorGuiOptions &options) {
            Application::Internal::EditorProjectScenePolicy scenePolicy{.physicsMode = options.physicsMode.value(),
                                                                        .prefabGranted = options.prefabGranted.value()};
            if (!options.cookedRoot.empty()) {
                auto target = Assets::AssetCookTargetId::Parse(options.cookTarget);
                if (target.HasError()) {
                    LOG_ERROR("editor.runtime", "Invalid explicit cook target: %s", target.ErrorValue().message.c_str());
                    return Result<Application::Internal::EditorProjectScenePolicy>::Failure(target.ErrorValue());
                }
                auto root = Application::Internal::ParseSceneHostAbsolutePath(options.cookedRoot);
                if (root.HasError()) {
                    LOG_ERROR("editor.runtime", "Invalid cook root: %s", root.ErrorValue().message.c_str());
                    return Result<Application::Internal::EditorProjectScenePolicy>::Failure(root.ErrorValue());
                }
                scenePolicy.cooked.emplace(std::move(root).Value(), std::move(target).Value());
            }
            return Result<Application::Internal::EditorProjectScenePolicy>::Success(std::move(scenePolicy));
        }

        /** @brief Parses an optional startup project path without granting implicit project scope. */
        [[nodiscard]] Result<std::filesystem::path> CaptureEditorPreviewRoot(const EditorGuiOptions &options) {
            std::filesystem::path previewProjectRoot;
            if (!options.projectRoot.empty()) {
                auto root = Application::Internal::ParseSceneHostAbsolutePath(options.projectRoot);
                if (root.HasError()) {
                    LOG_ERROR("editor.runtime", "Invalid project root: %s", root.ErrorValue().message.c_str());
                    return Result<std::filesystem::path>::Failure(root.ErrorValue());
                }
                previewProjectRoot = std::move(root).Value();
            }
            return Result<std::filesystem::path>::Success(std::move(previewProjectRoot));
        }

        /** @brief Runs paced frames and retains cancellation, fatal-shutdown and requested-frame completion semantics. */
        [[nodiscard]] bool RunEditorFrames(const EditorGuiOptions &options, EditorRuntimeParticipant &participant,
                                           Runtime::RuntimeHost &runtime, GuiScreenHost &screenHost) {
            bool healthy = true;
            bool completedFrame = false;
            while (!screenHost.IsApplicationCloseRequested() && (runtime.State() == Runtime::RuntimeLifecycleState::Running ||
                                                                 runtime.State() == Runtime::RuntimeLifecycleState::Suspended)) {
                if (const Result<void> frame = RunPacedEditorFrame(participant, runtime); frame.HasError()) {
                    if (frame.ErrorValue().code.Value() != "runtime.host.cancelled" &&
                        frame.ErrorValue().code.Value() != Render::FramePacingErrors::Cancelled.code.Value()) {
                        LOG_ERROR("editor.runtime", "Host frame failed: %s", frame.ErrorValue().message.c_str());
                        screenHost.RequestFatalShutdown();
                        healthy = false;
                    }
                    break;
                }
                completedFrame = participant.HasPresentedFrame();
            }
            return healthy && screenHost.IsApplicationCloseRequested() &&
                   ((!options.exitAfterFirstFrame && options.exitAfterFrames == 0) || completedFrame);
        }

        /** @brief Registers scene and UI source owners before any editor consumer starts; retains revoked UI storage through native drain.
         */
        Result<Application::Internal::RuntimeUiPaintPort> RegisterEditorRuntimeSources(
            Runtime::RuntimeHost &runtime, std::unique_ptr<Runtime::RuntimeLifecycleParticipant> scene,
            EditorPresentationPorts &presentation, Application::Internal::RuntimeUiPaintComposition ui) {
            using Preparation = Result<Application::Internal::RuntimeUiPaintPort>;
            if (const Result<void> addedScene = runtime.AddParticipant(std::move(scene)); addedScene.HasError()) {
                LOG_ERROR("editor.runtime", "Runtime scene service registration failed: %s", addedScene.ErrorValue().message.c_str());
                return Preparation::Failure(addedScene.ErrorValue());
            }
            const auto port = ui.port;
            // Presentation owns revoked UI storage until its same native completion generation has drained.
            presentation.uiRetirement = port;
            if (const auto registered = runtime.AddParticipant(std::move(ui.participant)); registered.HasError()) {
                LOG_ERROR("editor.runtime", "Runtime UI registration failed: %s", registered.ErrorValue().message.c_str());
                return Preparation::Failure(registered.ErrorValue());
            }
            return Preparation::Success(port);
        }

        /** @brief Admits the initial physical output before any project UI source can publish geometry. */
        bool InitializeRuntimeUiOutput(SDL_Window &window, const Application::Internal::RuntimeUiPaintPort &port) {
            int width{};
            int height{};
            if (!SDL_GetWindowSizeInPixels(&window, &width, &height))
                return false;
            if (width <= 0 || height <= 0)
                return true;
            const auto output = CaptureRuntimeUiOutput(window, {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)});
            return output.HasValue() && port.UpdateOutput(output.Value()).HasValue();
        }

#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
        /** @brief Composes selected XR Play from the same runtime scheduler and installed host before source registration. */
        bool ComposeSelectedEditorXrPlay(RunEditorMainLoopParams &p, const Runtime::RuntimeHost &runtime, const Duration fixedStep) {
            if (auto &xrComposition = p.presentation.composition; xrComposition.xrSession) {
                if (!p.background.xrHost || !p.background.xrHost->bindings || !xrComposition.xrViewport) {
                    LOG_ERROR("editor.xr", "Selected XR Play composition is missing its installed bindings or same Vulkan viewport");
                    return false;
                }
                const auto trackingRoot = SelectedXrTrackingRoot(p.options, *p.background.xrHost);
                if (trackingRoot.HasError()) {
                    LOG_ERROR("editor.xr", "%s", trackingRoot.ErrorValue().message.c_str());
                    return false;
                }
                auto composed = Application::Internal::OpenXRPlayHost::Create(
                    {*xrComposition.xrSession, p.inputRouter, *xrComposition.xrViewport, p.presentation.renderFrontend,
                     p.background.xrHost->bindings, runtime.DispatchSource(), fixedStep, trackingRoot.Value()});
                if (composed.HasError()) {
                    LOG_ERROR("editor.xr", "Selected XR Play composition failed: %s", composed.ErrorValue().message.c_str());
                    return false;
                }
                xrComposition.xrPlay = std::move(composed).Value();
            }
            return true;
        }
#endif

        /** @brief Keeps an optional fallback host and the selected package service in one startup lifetime. */
        struct EditorPackageComposition final {
            std::unique_ptr<Application::Internal::PackageProjectHost> owned;
            Application::Internal::PackageProjectHost *selected{};
        };

        /** @brief Selects the existing XR package owner or composes the fallback against the same borrowed startup services. */
        Result<EditorPackageComposition> PrepareEditorPackages(RunEditorMainLoopParams &p, NativeDurableFileSystem &durableFiles,
                                                               const SteadyClock &clock, const SystemWallClock &wallClock,
                                                               const Application::Internal::UnconfiguredPackageTrust &trust) {
            using Output = Result<EditorPackageComposition>;
            auto platform = Application::Internal::NativePackagePlatform();
            if (platform.HasError())
                return Output::Failure(platform.ErrorValue());
            EditorPackageComposition packages;
#if defined(HORO_HAS_OPENXR_HOST)
            if (p.background.xrHost && p.background.xrHost->packages)
                packages.selected = p.background.xrHost->packages.get();
#endif
            if (!packages.selected) {
                const auto cache = ResolveEditorSettingsHomeDirectory() / ".cache" / "horo" / "packages";
                auto created = Application::Internal::PackageProjectHost::
                    Create({p.background.jobs, durableFiles, clock, wallClock, trust},
                           {.cacheRoot = cache,
                            .platform = std::move(platform).Value(),
                            .lifecycle = {.temporaryRoot = cache / "native",
                                          .profile = Extensions::ExtensionHostProfile::Interactive,
                                          .artifactGate = std::make_shared<Application::Internal::UnconfiguredPackageGate>()}},
                           Duration::FromMilliseconds(1'000));
                if (created.HasError())
                    return Output::Failure(created.ErrorValue());
                packages.owned = std::move(created).Value();
                packages.selected = packages.owned.get();
            }
            return Output::Success(std::move(packages));
        }

        /** @brief Runs the actual editor startup and reports failures to the process boundary. */
        std::optional<EditorRendererRestartRequest> RunEditorMainLoop(RunEditorMainLoopParams &p) {
            EditorGuiLiveReload liveGui;
            ThemeContext themeContext{p.fonts, liveGui.Registry()};
            EditorSettingsSnapshot settingsSnapshot = p.settings.Snapshot();
            EditorGuiContext guiContext{p.engineEvents, p.editorEvents, p.localization, themeContext, settingsSnapshot};
            std::optional<UpdateExperienceSession> updateSession;
            ConfigureUpdateExperience(p, guiContext, updateSession);

            // Borrowed screen services must outlive the host that invokes screen OnLeave().
            EditorViewportSceneState viewportSceneState;
            NativeDurableFileSystem durableFiles;
            NativeExternalProcessRunner externalProcesses;
            Application::GameplayBuildService gameplayBuildService{externalProcesses, p.background.jobs, durableFiles,
                                                                   &p.operationServices.buildOutputStore,
                                                                   &p.operationServices.operationStore};
            Application::GameplayBuildEnvironment gameplayBuildEnvironment{
                .gameplaySdkPackage = HORO_GAMEPLAY_SDK_PACKAGE_DIR,
                .cmakeExecutable = HORO_GAMEPLAY_CMAKE_EXECUTABLE,
                .cxxCompiler = std::filesystem::path{HORO_GAMEPLAY_CXX_COMPILER},
            };
            SystemWallClock wallClock;
            SteadyClock packageClock;
            Application::Internal::UnconfiguredPackageTrust packageTrust;
            auto preparedPackages = PrepareEditorPackages(p, durableFiles, packageClock, wallClock, packageTrust);
            if (preparedPackages.HasError())
                return std::nullopt;
            auto packageComposition = std::move(preparedPackages).Value();
            auto *packages = packageComposition.selected;
            ProjectMutationCoordinator mutationCoordinator{durableFiles};
            ProjectMigrationTransactionService migrationTransactions{durableFiles, wallClock, mutationCoordinator, p.background.jobs};
            ProjectOpenPreflightService projectOpenPreflight{migrationTransactions};
            RecentProjectInspectionService recentProjectInspection{p.background.jobs, projectOpenPreflight};
            const WorkspaceReadSourceFactory workspaceReadSources{CaptureNativeWorkspaceSources};
            SteadyClock clock;
            const Runtime::FrameSchedulerConfig schedulerConfig{};
            auto createdRuntime = Runtime::RuntimeHost::Create(clock, schedulerConfig);
            if (createdRuntime.HasError()) {
                LOG_ERROR("editor.runtime", "Runtime host creation failed: %s", createdRuntime.ErrorValue().message.c_str());
                return std::nullopt;
            }
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
            auto &xrComposition = p.presentation.composition;
            if (!ComposeSelectedEditorXrPlay(p, *createdRuntime.Value(), schedulerConfig.fixedStep))
                return std::nullopt;

            struct XrInputOwnerScope final {
                explicit XrInputOwnerScope(Application::Internal::OpenXRPlayHost *owner) noexcept : host(owner) {}

                XrInputOwnerScope(const XrInputOwnerScope &) = delete;
                XrInputOwnerScope &operator=(const XrInputOwnerScope &) = delete;

                Application::Internal::OpenXRPlayHost *host;

                ~XrInputOwnerScope() {
                    if (host)
                        host->ReleaseInputOwner();
                }
            };

            XrInputOwnerScope xrInputOwner{xrComposition.xrPlay.get()};
#endif
            auto uiComposition =
                Application::Internal::RuntimeUiPaintHost::Compose(p.presentation.renderFrontend, createdRuntime.Value()->DispatchSource(),
                                                                   {.ownership = Runtime::Ui::UiOwnershipGeneration::Create(1).Value(),
                                                                    .maximumInstances = 8,
                                                                    .maximumRetiredInstances = 8,
                                                                    .maximumPreparedTransitions = 2});
            if (uiComposition.HasError())
                return std::nullopt;
            auto allocator = Runtime::Ui::UiElementSlotAllocator::Create(Runtime::Ui::UiOwnershipGeneration::Create(1).Value());
            if (allocator.HasError())
                return std::nullopt;
            Application::Internal::RuntimeUiPaintProjectPolicy uiPolicy;
            if (p.presentation.renderFrontend.Capabilities().backend == Render::RenderBackendId{"metal"})
                uiPolicy.source =
                    Application::Internal::RuntimeUiPaintPackagedAssets{EditorAssetRoot(), Render::ShaderTargetBackend::Metal};
            if (p.operationServices.uiPaintTarget)
                uiPolicy.source = Application::Internal::RuntimeUiPaintProjectCompiler{p.background.jobs, p.operationServices.shaderBuild,
                                                                                       AssetPath("shaders/runtime/ui_paint.hlsl"),
                                                                                       *p.operationServices.uiPaintTarget};
            if (!InitializeRuntimeUiOutput(*p.presentation.window, uiComposition.Value().port))
                return std::nullopt;
            auto projectUi =
                Application::Internal::RuntimeUiPaintProjectSources::Create(uiComposition.Value().port,
                                                                            std::make_shared<Runtime::Ui::UiElementSlotAllocator>(
                                                                                std::move(allocator).Value()),
                                                                            std::move(uiPolicy));
            if (projectUi.HasError())
                return std::nullopt;
            auto capturedPolicy = CaptureEditorScenePolicy(p.options);
            if (capturedPolicy.HasError())
                return std::nullopt;
            auto scenePolicy = std::move(capturedPolicy).Value();
            scenePolicy.prefabMetrics = p.telemetry.prefab;
            scenePolicy.uiSources = std::move(projectUi).Value();
            std::optional<Application::WorldBakeDependencySource> worldBakeDependencies;
            if (scenePolicy.cooked)
                worldBakeDependencies.emplace(scenePolicy.cooked->targetRoot, scenePolicy.cooked->target,
                                              std::make_shared<NativeDurableFileSystem>());
            auto capturedRoot = CaptureEditorPreviewRoot(p.options);
            if (capturedRoot.HasError())
                return std::nullopt;
            auto previewProjectRoot = std::move(capturedRoot).Value();
            auto preparedPreview = Application::Internal::PrepareEditorProjectPreview(previewProjectRoot, p.background.jobs, scenePolicy);
            if (preparedPreview.HasError()) {
                LOG_ERROR("editor.runtime", "Scene project capture rejected: %s", preparedPreview.ErrorValue().message.c_str());
                return std::nullopt;
            }
            // RuntimeHost receives the sole preview service; captured assets/native authorities
            // remain alive here through RuntimeHost and screen teardown. Play owns distinct services.
            auto preview = std::move(preparedPreview).Value();
            auto runtimeScene = std::move(preview.scenes);
            Runtime::RuntimeSceneService *runtimeSceneService = runtimeScene.get();
            auto playPolicy = scenePolicy;
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
            playPolicy.xrHost = xrComposition.xrPlay.get();
#endif
            auto playSceneFactory = Application::Internal::MakeEditorProjectSceneFactory(p.background.jobs, std::move(playPolicy));
            auto prefabStageFactory = Application::Internal::MakeEditorProjectPrefabStageFactory(p.background.jobs, scenePolicy);
            // Null is explicit; canonical activation needs an exact owner receipt.
            CollisionActivationEvidence collisionActivation{.revision = 1};
            if (preview.physicsMode == Physics::PhysicsRuntimeMode::Null)
                collisionActivation.state = CollisionActivationState::NoPhysics;
            const EditorPreviewCollisionReceiptSource previewCollisionReceipt{[&preview]() noexcept {
                return preview.owners->CollisionReceipt();
            }};
            EditorAudioPreviewHost audioPreview{MakeSdl3AudioPreviewOutput};
            Assets::AssetRegistry assetRegistry;
            CollisionInventoryEvidence collisionInventory;
            AssetRegistryProjectOpenContributor assetRegistryContributor{assetRegistry, collisionInventory};
            SceneScopeProjectOpenContributor sceneScopeContributor{previewProjectRoot, preview.physicsMode};
            std::array<IProjectOpenDerivedStateContributor *, 2> projectOpenContributors{&sceneScopeContributor, &assetRegistryContributor};
            ProjectOpenService projectOpenService{p.background.jobs,      durableFiles,          projectOpenPreflight,
                                                  mutationCoordinator,    migrationTransactions, p.rendererAvailability,
                                                  projectOpenContributors};
            ScreenRegistry screenRegistry;
            RegisterWelcomeScreen(screenRegistry);
            RegisterProjectCreationScreen(screenRegistry);
            RegisterProjectLoadingScreen(screenRegistry);
            RegisterEditorWorkspaceScreen(screenRegistry, {.acquire = [&packages](const std::filesystem::path &root) {
                return packages->Acquire(root);
            }, .pump = [&packages](const std::filesystem::path &root, Packages::PackageActivationBoundary boundary) {
                return packages->PumpProject(root, boundary);
            }, .retire = [&packages](const std::filesystem::path &root) {
                return packages->Retire(root);
            }});
            EditorNavigationBakeHost navigationBakes{p.operationServices.operationStore};
            const auto navigationBakeAccess = navigationBakes.Access();
            const NavigationViewportAccess navigationOverlayAccess{.prepare = navigationBakeAccess.prepare,
                                                                   .query = navigationBakeAccess.overlays};
            WorkspacePanelRegistry workspacePanelRegistry;
            RegisterInspectionWorkspacePanels(workspacePanelRegistry, p.presentation.renderFrontend);
            RegisterNavigationWorkspacePanel(workspacePanelRegistry, navigationBakeAccess, p.modalHost);
            Extensions::ExtensionInventory extensionInventory;
            const Result<void> extensionInventoryRefresh = extensionInventory.Refresh();
            if (extensionInventoryRefresh.HasError()) {
                LOG_ERROR("editor.extensions", "Unable to refresh extension inventory: %s",
                          extensionInventoryRefresh.ErrorValue().message.c_str());
            }
            Extensions::ExtensionMarketplaceService extensionMarketplace{p.background.jobs, extensionInventory,
                                                                         Extensions::ExtensionMarketplaceService::DefaultRegistryUrl()};
            PfdNativeDialogs nativeDialogs;
            Application::NetworkDebuggerService networkDebugger;

            // Covers constructor failure as well as startup failure: dependencies precede both join guards.
            const EditorWorkerShutdown constructionRollback{p.background.jobs, p.operationServices.shaderBuild, nullptr, packages};
            auto gameplayRetirements = std::make_shared<Gameplay::GameModuleRetirementQueue>();
            auto uiInspectorResourcesFactory =
                MakeEditorUiInspectorResourcesFactory(p.background.jobs, assetRegistry, mutationCoordinator, durableFiles);
            WorldStreamingPreviewHost streamingPreview;
            auto streamingPolicy = MakeWorldStreamingPreviewRuntimePolicy();
            if (streamingPolicy.HasError()) {
                LOG_ERROR("editor.streaming", "Cannot compose streaming inspection policy: %s",
                          streamingPolicy.ErrorValue().message.c_str());
                return std::nullopt;
            }
            GuiScreenHost
                screenHost{guiContext,
                           GuiScreenHostComposition{.modalHost = p.modalHost,
                                                    .settingsService = p.settings,
                                                    .localization = p.localization,
                                                    .engineEvents = p.engineEvents,
                                                    .creationService = p.projectCreationService,
                                                    .jobs = p.background.jobs,
                                                    .inputRouter = p.inputRouter,
                                                    .rendererAvailability = p.rendererAvailability,
                                                    .screenRegistry = std::move(screenRegistry),
                                                    .workspacePanelRegistry = std::move(workspacePanelRegistry),
                                                    .logoTexture = p.textures.logo,
                                                    .extensionInventory =
                                                        extensionInventoryRefresh.HasValue() ? &extensionInventory : nullptr,
                                                    .extensionMarketplace =
                                                        extensionInventoryRefresh.HasValue() ? &extensionMarketplace : nullptr,
                                                    .nativeDialogs = &nativeDialogs,
                                                    .extensionSettingsFiles = &durableFiles,
                                                    .extensionUserSettingsRoot = ResolveEditorSettingsPath().parent_path(),
                                                    .extensionUiPolicy =
                                                        {.revision = 1,
                                                         .availableCapabilities = {Extensions::EditorCustomCompositionCapability}},
                                                    .extensionPagePolicy = {.revision = 1,
                                                                            .knownPermissions = {{"editor.settings"},
                                                                                                 {"editor.modal"},
                                                                                                 {"configuration.user.write"},
                                                                                                 {"configuration.project.write"},
                                                                                                 {"configuration.workspace.write"}},
                                                                            .approvedPermissions = {{"editor.settings"},
                                                                                                    {"editor.modal"},
                                                                                                    {"configuration.user.write"},
                                                                                                    {"configuration.project.write"},
                                                                                                    {"configuration.workspace.write"}},
                                                                            .availableCapabilities = {{"horo.editor.settings"},
                                                                                                      {"horo.editor.modal"}}},
                                                    .extensionInspectorPolicy =
                                                        {.revision = 1,
                                                         .knownPermissions = {{"editor.inspector"}, {"editor.property-drawer"}},
                                                         .approvedPermissions = {{"editor.inspector"}, {"editor.property-drawer"}},
                                                         .availableCapabilities = {{"horo.editor.inspector"},
                                                                                   {"horo.editor.property-drawer"},
                                                                                   Extensions::EditorCustomCompositionCapability}},
                                                    .extensionViewportPolicy = {.revision = 1,
                                                                                .knownPermissions = {{"editor.viewport-overlay"},
                                                                                                     {"editor.gizmo"}},
                                                                                .approvedPermissions = {{"editor.viewport-overlay"},
                                                                                                        {"editor.gizmo"}},
                                                                                .availableCapabilities = {{"horo.editor.viewport-overlay"},
                                                                                                          {"horo.editor.gizmo"}}},
                                                    .gameplayRetirements = gameplayRetirements}};
            // Closes route/extension scopes and joins before destroying the successfully constructed screen host.
            const EditorWorkerShutdown jobShutdown{p.background.jobs, p.operationServices.shaderBuild, &screenHost, packages};
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
            if (xrComposition.xrPlay)
                screenHost.Services().Register<IEditorPlayXrHost>(*xrComposition.xrPlay);
#endif
            screenHost.Services().Register<IEditorViewportRenderer>(p.presentation.viewportRenderer);
            screenHost.Services().Register<IEditorGuiRenderer>(p.presentation.guiRenderer);
            screenHost.Services().Register<EditorViewportSceneState>(viewportSceneState);
            screenHost.Services().Register<WorldStreamingPreviewHost>(streamingPreview);
            screenHost.Services().RegisterConst<WorldStreamingPreviewRuntimePolicy>(streamingPolicy.Value());
            screenHost.Services().Register<Runtime::RuntimeSceneService>(*runtimeSceneService);
            screenHost.Services().Register<EditorProjectPlaySceneFactory>(playSceneFactory);
            screenHost.Services().Register<EditorProjectPrefabStageFactory>(prefabStageFactory);
            screenHost.Services().RegisterConst<Prefab::PrefabMetricHandles>(p.telemetry.prefab);
            screenHost.Services().Register<Assets::AssetRegistry>(assetRegistry);
            screenHost.Services().Register<UiInspectorResourcesFactory>(uiInspectorResourcesFactory);
            screenHost.Services().Register<IAudioComponentPreview>(audioPreview);
            screenHost.Services().RegisterConst<NavigationViewportAccess>(navigationOverlayAccess);
            screenHost.Services().Register<CollisionInventoryEvidence>(collisionInventory);
            screenHost.Services().Register<CollisionActivationEvidence>(collisionActivation);
            screenHost.Services().RegisterConst<EditorPreviewCollisionReceiptSource>(previewCollisionReceipt);
            screenHost.Services().RegisterConst<WorkspaceReadSourceFactory>(workspaceReadSources);
            screenHost.Services().Register<ProjectMutationCoordinator>(mutationCoordinator);
            screenHost.Services().Register<DurableFileSystem>(durableFiles);
            screenHost.Services().Register<Application::GameplayBuildService>(gameplayBuildService);
            screenHost.Services().Register<Application::GameplayBuildEnvironment>(gameplayBuildEnvironment);
            screenHost.Services().Register<ProjectOpenService>(projectOpenService);
            screenHost.Services().Register<RecentProjectInspectionService>(recentProjectInspection);
            screenHost.Services().RegisterConst<Log::IStructuredLogQuery>(p.logQuery);
            screenHost.Services().RegisterConst<IBuildOutputQuery>(p.operationServices.buildOutputStore);
            screenHost.Services().Register<BuildOutputStore>(p.operationServices.buildOutputStore);
            if (worldBakeDependencies)
                screenHost.Services().RegisterConst<Application::WorldBakeDependencySource>(*worldBakeDependencies);
            screenHost.Services().Register<Application::ShaderBuildService>(p.operationServices.shaderBuild);
            screenHost.Services().Register<OperationStore>(p.operationServices.operationStore);
            screenHost.Services().RegisterConst<IOperationQuery>(p.operationServices.operationStore);
            screenHost.Services().Register<IOperationControl>(p.operationServices.operationStore);
            screenHost.Services().Register<Application::NetworkDebuggerService>(networkDebugger);
            screenHost.Services().RegisterConst<Application::INetworkDebuggerQuery>(networkDebugger);
            screenHost.Services().Register<Application::INetworkDebuggerControl>(networkDebugger);
            std::optional<EditorRendererRestartRequest> rendererRestart;
            std::unique_ptr<Runtime::RuntimeHost> runtime = std::move(createdRuntime).Value();
            auto runtimeSources =
                RegisterEditorRuntimeSources(*runtime, std::move(runtimeScene), p.presentation, std::move(uiComposition).Value());
            if (runtimeSources.HasError()) {
                screenHost.RequestFatalShutdown();
                screenHost.Shutdown();
                return std::nullopt;
            }
            if (const auto started = screenHost.Start(std::move(p.initialRoute)); started.HasError()) {
                LOG_ERROR("editor.screens", "Initial screen startup failed: %s", started.ErrorValue().message.c_str());
                screenHost.RequestFatalShutdown();
                screenHost.Shutdown();
                return std::nullopt;
            }
            auto participant = std::make_unique<EditorRuntimeParticipant>(p, screenHost, viewportSceneState, settingsSnapshot, liveGui,
                                                                          rendererRestart, clock,
                                                                          EditorRuntimeAuthorities{nullptr, packages, &navigationBakes,
                                                                                                   runtimeSources.Value()});
            participant->BindHost(*runtime);
            EditorRuntimeParticipant *pacingParticipant = participant.get();
            Result<void> startup = runtime->AddParticipant(std::move(participant));
            if (startup.HasValue()) {
                startup = runtime->Startup();
            }
            if (startup.HasError()) {
                LOG_ERROR("editor.runtime", "Runtime host startup failed [%s]: %s", startup.ErrorValue().code.Value().c_str(),
                          startup.ErrorValue().message.c_str());
                screenHost.RequestFatalShutdown();
                runtime->Shutdown();
                return std::nullopt;
            }

            const bool healthy = RunEditorFrames(p.options, *pacingParticipant, *runtime, screenHost);
            p.healthy = CompleteEditorRuntimeShutdown(*runtime, *pacingParticipant, healthy, runtimeSources.Value());
            RequestUpdateActivationOnExit(updateSession, screenHost.IsApplicationCloseRequested(), rendererRestart.has_value());
            return rendererRestart;
        }
    }  // namespace

    [[nodiscard]] static bool ConfigureStartupProject(EditorGuiOptions &options, std::string &projectName) {
        if (options.projectRoot.empty())
            return true;

        const Application::ProjectCompatibilitySnapshot compatibility = Application::InspectProjectCompatibility(options.projectRoot);
        if (const bool mayOpen = compatibility.status == Application::ProjectCompatibilityStatus::Current ||
                                 compatibility.status == Application::ProjectCompatibilityStatus::CompatibleReleaseLine;
            !mayOpen || !compatibility.metadata.has_value()) {
            LOG_CRITICAL("editor.project", "Project startup preflight failed for '%s': %s", options.projectRoot.c_str(),
                         compatibility.diagnostic.has_value() ? compatibility.diagnostic->message.c_str()
                                                              : "Project version is not compatible with this editor.");
            return false;
        }
        options.rendererBackend = compatibility.metadata->renderBackend;
        projectName = compatibility.metadata->name;
        return true;
    }

    /** @brief Composes editor modules with the settings loaded for this startup. */
    [[nodiscard]] static Result<std::unique_ptr<ModuleHost>> ComposeEditorModules(const Application::Internal::HostRenderer renderer,
                                                                                  const EditorSettings &initialSettings) {
        const std::vector settingsContributions{MakeEditorSettingsContribution(initialSettings)};
        return Application::Internal::ComposeHostModules({.host = Application::Internal::HostKind::Editor, .renderer = renderer},
                                                         settingsContributions);
    }

    void ConfigureEditorInput(Input::InputRouter &inputRouter, const EditorSettings &settings) {
        if (const auto installed = inputRouter.SetActionMap(BuildEditorInputActions(), BuiltinEditorInputProfile(settings.keymap.profile));
            installed.HasError())
            LOG_CRITICAL("editor.input", "Built-in input action map is invalid: %s", installed.ErrorValue().message.c_str());
    }

    struct PreparedEditorStartup final {
        EditorGuiOptions options;
        std::string projectName;
        const Render::RenderBackendModuleInfo *moduleInfo;
        RendererAvailabilitySnapshot rendererAvailability;
        EditorSettings settings;
        std::unique_ptr<ModuleHost> modules;
        // Both canonical authorities are created before native presentation and survive the complete session/drain.
        std::unique_ptr<EngineDataBus> engineEvents;
        std::unique_ptr<ConfigurationService> configuration;
#if defined(HORO_HAS_OPENXR_HOST)
        std::unique_ptr<EditorOpenXRBootstrap> xrBootstrap;
#endif
        const EditorOpenXRHostServices *xrHost{};
#if defined(HORO_HAS_OPENXR_HOST)
        std::optional<Application::Internal::OpenXRInstalledProductCandidate> xrProduct;
#endif
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
        std::unique_ptr<Application::Internal::OpenXRApplicationSession> xrNative;
#endif
    };

    /** @brief Validates explicit application Physics selection before project/native frontend creation. */
    [[nodiscard]] static bool ValidateEditorPhysicsOptions(const EditorGuiOptions &options) {
        if (!options.physicsMode || !options.prefabGranted.has_value() || !options.cookOptionsValid ||
            (*options.physicsMode == Physics::PhysicsRuntimeMode::Canonical && options.projectRoot.empty())) {
            const auto error =
                MakeError(Physics::PhysicsErrors::DescriptorInvalid,
                          "Select --physics null or --physics canonical. Canonical Scene activation requires an explicit --project root.");
            LOG_CRITICAL("editor.physics", "%s", error.message.c_str());
            return false;
        }
        return true;
    }

    /** @brief Canonical configuration/event lifetime pair transferred intact into one prepared startup. */
    struct EditorStartupConfiguration final {
        std::unique_ptr<EngineDataBus> events;
        std::unique_ptr<ConfigurationService> service;
    };

    /** @brief Registers inert descriptors and resolves explicit captured inputs before any native presentation. */
    [[nodiscard]] static std::optional<EditorStartupConfiguration> PrepareEditorConfiguration(const EditorSettings &settings,
                                                                                              const ModuleHost &modules,
                                                                                              const EditorOpenXRHostServices *xrHost) {
        auto moduleSchema = modules.BuildConfigurationSchema();
        if (moduleSchema.HasError()) {
            LOG_CRITICAL("editor.startup", "Module configuration failed: %s", moduleSchema.ErrorValue().message.c_str());
            return std::nullopt;
        }
        auto configurationSchema = std::move(moduleSchema).Value();
#if defined(HORO_HAS_OPENXR_HOST)
        // Explicit application composition; descriptor registration performs no native discovery or activation.
        for (const auto &descriptor : XR::XRProjectSettingDescriptors()) {
            if (const auto registered = configurationSchema.Register(descriptor); registered.HasError()) {
                LOG_CRITICAL("editor.xr", "XR configuration composition failed: %s", registered.ErrorValue().message.c_str());
                return std::nullopt;
            }
        }
#endif
        auto engineEvents = std::make_unique<EngineDataBus>();
        auto configuration =
            std::make_unique<ConfigurationService>(CreateEditorConfigurationSchema(settings, std::move(configurationSchema)),
                                                   engineEvents.get());
#if defined(HORO_HAS_OPENXR_HOST)
        if (xrHost) {
            if (const auto resolved = configuration->ResolveAndCommit(xrHost->configuration); resolved.HasError()) {
                LOG_CRITICAL("editor.xr", "Project configuration rejected: %s", resolved.ErrorValue().message.c_str());
                return std::nullopt;
            }
        }
#else
        if (xrHost) {
            LOG_CRITICAL("editor.xr", "This executable does not contain the selected XR host component.");
            return std::nullopt;
        }
#endif
        return EditorStartupConfiguration{std::move(engineEvents), std::move(configuration)};
    }

    /** @brief Retains only the exact canonical installed-product selection requested by resolved XR policy. */
    [[nodiscard]] static bool PrepareEditorXrProduct(PreparedEditorStartup &startup) {
#if defined(HORO_HAS_OPENXR_HOST)
        const auto xrSettings = XR::ResolveXRProjectSettings(startup.configuration->Snapshot());
        if (xrSettings.HasError()) {
            LOG_CRITICAL("editor.xr", "%s", xrSettings.ErrorValue().message.c_str());
            return false;
        }
        if (xrSettings.Value().Enabled()) {
            if (!startup.xrHost || !startup.xrHost->packages || !startup.xrHost->bindings || !startup.xrHost->validateAdmission ||
                !startup.xrHost->jobs) {
                const auto error = MakeError(XR::XRErrors::OperationUnavailable,
                                             "No installed XR product owner and physical binding catalog are configured");
                LOG_CRITICAL("editor.xr", "%s", error.message.c_str());
                return false;
            }
            if (auto current = startup.xrHost->validateAdmission(); current.HasError()) {
                LOG_CRITICAL("editor.xr", "%s", current.ErrorValue().message.c_str());
                return false;
            }
            if (const auto trackingRoot = SelectedXrTrackingRoot(startup.options, *startup.xrHost); trackingRoot.HasError()) {
                LOG_CRITICAL("editor.xr", "%s", trackingRoot.ErrorValue().message.c_str());
                return false;
            }
            auto product = Application::Internal::SelectInstalledOpenXRProduct(*startup.xrHost->packages, startup.options.projectRoot,
                                                                               startup.xrHost->packages->Generation(),
                                                                               &startup.xrHost->policy, CancellationToken{});
            if (product.HasError()) {
                LOG_CRITICAL("editor.xr", "%s", product.ErrorValue().message.c_str());
                return false;
            }
            startup.xrProduct.emplace(std::move(product).Value());
        }
#endif
        return true;
    }

    /** @brief Validates startup inputs and composes the editor's installed modules. */
    [[nodiscard]] static std::optional<PreparedEditorStartup> PrepareEditorStartup(EditorGuiOptions options,
                                                                                   const EditorOpenXRHostServices *xrHost,
                                                                                   const std::span<char *> arguments) {
        if (!ValidateEditorPhysicsOptions(options))
            return std::nullopt;
        std::string projectName;
        if (!ConfigureStartupProject(options, projectName))
            return std::nullopt;
        if (!options.xrTrackingRootValid) {
            LOG_CRITICAL("editor.xr", "Explicit XR tracking-root selection is malformed or duplicated.");
            return std::nullopt;
        }
#if defined(HORO_HAS_OPENXR_HOST)
        std::unique_ptr<EditorOpenXRBootstrap> bootstrap;
        if (const bool hostArguments = std::ranges::any_of(arguments.subspan(std::min<std::size_t>(1, arguments.size())),
                                                           [](const char *value) {
            return std::string_view{value}.starts_with("--xr-host-");
        });
            xrHost && hostArguments) {
            LOG_CRITICAL("editor.xr", "Injected installer services and explicit host bootstrap inputs cannot select two authorities.");
            return std::nullopt;
        }
        if (!xrHost) {
            auto installed = EditorOpenXRBootstrap::Create(arguments, options.projectRoot,
                                                           ResolveEditorSettingsHomeDirectory() / ".cache" / "horo" / "packages",
                                                           options.xrTrackingRoot.value_or(Runtime::SceneObjectId{}));
            if (installed.HasError()) {
                LOG_CRITICAL("editor.xr", "Installed XR bootstrap failed: %s", installed.ErrorValue().message.c_str());
                return std::nullopt;
            }
            bootstrap = std::move(installed).Value();
            if (bootstrap)
                xrHost = &bootstrap->Services();
        }
#else
        if (std::ranges::any_of(arguments.subspan(std::min<std::size_t>(1, arguments.size())), [](const char *value) {
            return std::string_view{value}.starts_with("--xr-host-");
        })) {
            LOG_CRITICAL("editor.xr", "This executable contains no installed OpenXR host component.");
            return std::nullopt;
        }
#endif
        const Render::RenderBackendModuleInfo *moduleInfo = ResolveCompiledBackendModuleInfo(options.rendererBackend);
        if (moduleInfo == nullptr || !moduleInfo->supportsInteractivePresentation) {
            LOG_CRITICAL("editor.renderer", "Requested renderer component '%s' is unavailable.", options.rendererBackend.c_str());
            return std::nullopt;
        }
        options.rendererBackend = moduleInfo->id.Value();
        const RendererAvailabilitySnapshot rendererAvailability = BuildRendererAvailabilitySnapshot(options.rendererBackend);
        auto selectedRenderer = Application::Internal::HostRendererFromBackendId(options.rendererBackend);
        if (selectedRenderer.HasError()) {
            LOG_CRITICAL("editor.renderer", "%s", selectedRenderer.ErrorValue().message.c_str());
            return std::nullopt;
        }
        EditorSettings settings = LoadEditorSettingsDocument().settings;
        auto composedModules = ComposeEditorModules(selectedRenderer.Value(), settings);
        if (composedModules.HasError()) {
            LOG_CRITICAL("editor.startup", "Module composition failed: %s", composedModules.ErrorValue().message.c_str());
            return std::nullopt;
        }
        std::unique_ptr<ModuleHost> modules = std::move(composedModules).Value();
        auto configuration = PrepareEditorConfiguration(settings, *modules, xrHost);
        if (!configuration)
            return std::nullopt;
        PreparedEditorStartup startup{std::move(options),
                                      std::move(projectName),
                                      moduleInfo,
                                      rendererAvailability,
                                      std::move(settings),
                                      std::move(modules),
                                      std::move(configuration->events),
                                      std::move(configuration->service),
#if defined(HORO_HAS_OPENXR_HOST)
                                      std::move(bootstrap),
#endif
                                      xrHost};
        if (!PrepareEditorXrProduct(startup))
            return std::nullopt;
        return startup;
    }

    /** @brief Runs actual selected-runtime discovery after inert startup succeeds and before creating a window. */
    [[nodiscard]] static Result<void> PrepareSelectedEditorXrStartup(PreparedEditorStartup &startup) {
#if defined(HORO_HAS_OPENXR_HOST)
        if (!startup.xrProduct)
            return Result<void>::Success();
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
        if (startup.options.rendererBackend != "vulkan")
            return Result<void>::Failure(
                MakeError(XR::XRErrors::OperationUnsupported, "Selected XR product requires the same Vulkan editor pair"));
        auto settings = XR::ResolveXRProjectSettings(startup.configuration->Snapshot());
        if (settings.HasError())
            return Result<void>::Failure(settings.ErrorValue());
        startup.xrNative = std::make_unique<
            Application::Internal::OpenXRApplicationSession>(Application::Internal::
                                                                 OpenXRApplicationAuthorities{*startup.configuration,
                                                                                              *startup.xrHost->packages,
                                                                                              startup.options.projectRoot,
                                                                                              startup.xrHost->packages->Generation(),
                                                                                              startup.xrHost->validateAdmission},
                                                             std::move(settings).Value(), std::move(*startup.xrProduct),
                                                             startup.xrHost->policy);
        startup.xrProduct.reset();
        return startup.xrNative->PrepareDiscovery();
#else
        return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnsupported, "No selected XR/Vulkan host composition is installed"));
#endif
#else
        static_cast<void>(startup);
        return Result<void>::Success();
#endif
    }

    /** @brief Retains failed pre-window native destruction in explicit shutdown recovery, never a frame callback. */
    static void RetireEditorXrStartup(PreparedEditorStartup &startup) {
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
        if (!startup.xrNative)
            return;
        bool reported{};
        for (;;) {
            const auto closed = startup.xrNative->Close();
            if (closed.HasValue())
                return;
            if (!reported) {
                LOG_ERROR("editor.xr.retirement", "Native retirement failed: %s; retaining installation and native parents for recovery",
                          closed.ErrorValue().message.c_str());
                reported = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{8});
        }
#else
        static_cast<void>(startup);
#endif
    }

    /** @brief Keeps theme, locale, and native menu state synchronized with committed settings. */
    [[nodiscard]] static Subscription ObserveEditorSettings(EditorDataBus &events, const EditorSettingsService &settings,
                                                            LocalizationService &localization) {
        return events.Subscribe<EditorSettingsChangedEvent>([&settings, &localization](const EditorSettingsChangedEvent &event) {
            if (event.phase == SettingsChangePhase::Committed || event.phase == SettingsChangePhase::Reverted) {
                const EditorSettings committed = settings.Snapshot().settings;
                if (committed.themeId.empty())
                    Theme::SelectThemeByIndex(static_cast<int>(committed.themePreset));
                else if (!Theme::SelectThemeById(committed.themeId))
                    LOG_WARN("editor.theme", "Selected package theme is unavailable: %s", committed.themeId.c_str());
                if (const auto loc = LocaleTag::Parse(committed.languageTag);
                    loc.has_value() && localization.ActiveLocale() != *loc && localization.Prepare(*loc)) {
                    (void)localization.ActivatePrepared();
                    InstallNativeEditorMenuBar(GetEditorMenuModel(), localization);
                }
            }
        });
    }

    /** @brief Installs editor input actions and a saved profile on the active SDL window. */
    static void ConfigureEditorInput(SDL_Window &window, Input::SdlInputBackend &backend, Input::InputRouter &router,
                                     const EditorSettings &settings) {
        backend.BindWindow(SDL_GetWindowID(&window));
        ConfigureEditorInput(router, settings);
    }

    /** @brief Chooses the initial persistent route from validated startup options. */
    [[nodiscard]] static GuiRoute InitialEditorRoute(const PreparedEditorStartup &startup) {
        if (startup.options.projectRoot.empty())
            return {GuiRouteKind::Welcome, WelcomeRouteParameters{}};
        return {GuiRouteKind::ProjectLoading, ProjectLoadingRouteParameters{startup.options.projectRoot, startup.projectName}};
    }

    struct EditorSessionLaunch final {
        PreparedEditorStartup &startup;
        SDL_Window &window;
        ImGuiIO &io;
        const Fonts &fonts;
        const EditorTextures &textures;
        EditorRenderComposition &composition;
        EditorTelemetry &telemetry;
        const Log::IStructuredLogQuery &logs;
        BuildOutputStore &buildOutput;
        OperationStore &operations;
        const EditorUpdateHostServices *updateHost;
        std::shared_ptr<const Render::IShaderCompilerAdapter> shaderCompiler;
        std::shared_ptr<const Render::ShaderCompilerTargetDescriptor> uiPaintTarget;
    };

    struct EditorSessionResult final {
        std::optional<EditorRendererRestartRequest> rendererRestart;
        bool healthy;
    };

    /** @brief Owns runtime services and routes for one graphical editor session. */
    [[nodiscard]] static EditorSessionResult RunEditorSession(EditorSessionLaunch launch) {
        EngineDataBus &engineEvents = *launch.startup.engineEvents;
        std::unique_ptr<JobSystem> ownedJobs;
        JobSystem *selectedJobs{};
#if defined(HORO_HAS_OPENXR_HOST)
        if (launch.startup.xrHost)
            selectedJobs = launch.startup.xrHost->jobs;
#endif
        if (!selectedJobs) {
            ownedJobs = std::make_unique<JobSystem>(
                JobSystemConfig{.workerCount = 2, .maxQueuedJobs = 256, .ioWorkerCount = 1, .reservedInteractiveJobs = 8});
            selectedJobs = ownedJobs.get();
        }
        JobSystem &jobSystem = *selectedJobs;
        Application::ShaderBuildService shaderBuild{std::move(launch.shaderCompiler), launch.buildOutput};
        ProjectCreationService projectCreationService{jobSystem, engineEvents};
        EditorDataBus editorEvents;
        LOG_INFO("editor.startup", "Loaded language tag from disk: '%s'", launch.startup.settings.languageTag.c_str());
        LocalizationService localization{LocaleTag{"en-US"}};
        const bool loadedCatalogs = LoadEditorCatalogResources(localization);
        LOG_INFO("editor.startup", "Catalog resources loaded: %s", loadedCatalogs ? "true" : "false");
        ActivateInitialLocale(launch.startup.settings, localization);
        ConfigurationService &configuration = *launch.startup.configuration;
        EditorSettingsService settings{launch.startup.settings, configuration, editorEvents, localization};
        const Subscription settingsSub = ObserveEditorSettings(editorEvents, settings, localization);

        Input::SdlInputBackend inputBackend;
        Input::InputRouter inputRouter;
        ConfigureEditorInput(launch.window, inputBackend, inputRouter, launch.startup.settings);
        EditorModalHost modalHost{editorEvents, inputRouter};
        GuiRoute initialRoute = InitialEditorRoute(launch.startup);
        RunEditorMainLoopParams loopParams{launch.startup.options,
                                           launch.telemetry,
                                           {&launch.window, launch.io, *launch.composition.frontend, *launch.composition.guiRenderer,
                                            *launch.composition.viewportRenderer, launch.composition.viewportTarget, launch.composition,
                                            launch.composition.uiRetirement},
                                           launch.fonts,
                                           launch.textures,
                                           projectCreationService,
                                           {jobSystem, launch.updateHost, launch.startup.xrHost},
                                           settings,
                                           engineEvents,
                                           editorEvents,
                                           localization,
                                           launch.startup.rendererAvailability,
                                           std::move(initialRoute),
                                           modalHost,
                                           inputBackend,
                                           inputRouter,
                                           launch.logs,
                                           {launch.buildOutput, launch.operations, shaderBuild, launch.uiPaintTarget}};
        auto rendererRestart = RunEditorMainLoop(loopParams);
        return {std::move(rendererRestart), loopParams.healthy};
    }

    /** @brief Repairs legacy user-state placement before any editor settings are loaded. */
    [[nodiscard]] static bool MigrateStartupUserState() {
        NativeDurableFileSystem files;
        const auto settingsRoot = ResolveEditorSettingsPath().parent_path();
        const auto cacheRoot = ResolveEditorSettingsHomeDirectory() / ".cache" / "horo";
        const auto migration = MigrateLegacyEditorUserState(settingsRoot, cacheRoot, files);
        if (migration.HasValue())
            return true;
        LOG_ERROR("editor.user_state", "User-state migration needs repair before editor startup: %s",
                  migration.ErrorValue().message.c_str());
        std::fprintf(stderr, "User-state migration needs repair before editor startup: %s\n", migration.ErrorValue().message.c_str());
        return false;
    }

    struct EditorPresentation final {
        SDL_Window *window;
        Fonts fonts;
        EditorRenderComposition composition;
        EditorTextures textures;
    };

#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
    /** @brief Retires only selected XR children while the same device, loader and application parents remain retained. */
    [[nodiscard]] static Result<Render::RenderBackendDrainProgress> RetireSelectedEditorXrHost(EditorRenderComposition &composition) {
        using Progress = Render::RenderBackendDrainProgress;
        // XR closes only after its canonical frame/image receipts retire, while the selected device and loader still exist.
        if (composition.xrPlay) {
            const auto closed = composition.xrPlay->Close();
            if (closed.HasError())
                return Result<Progress>::Failure(closed.ErrorValue());
            if (!closed.Value())
                return Result<Progress>::Success(Progress::Pending);
            composition.xrPlay.reset();
        }
        if (composition.xrSession) {
            if (auto closed = composition.xrSession->Close(); closed.HasError())
                return Result<Progress>::Failure(closed.ErrorValue());
        }
        return Result<Progress>::Success(Progress::Drained);
    }
#endif

    /** @brief Releases graphical resources before a renderer restart or module teardown. */
    [[nodiscard]] static Result<Render::RenderBackendDrainProgress> ShutdownEditorPresentation(SDL_Window *window,
                                                                                               EditorRenderComposition &composition,
                                                                                               EditorTextures &textures) {
        using Progress = Render::RenderBackendDrainProgress;
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
        if (auto xr = RetireSelectedEditorXrHost(composition); xr.HasError() || xr.Value() != Progress::Drained)
            return xr;
#endif
        if (composition.frontend) {
            if (!composition.retirementBegun) {
                composition.retirementBegun = true;
                if (const auto begun = composition.frontend->BeginShutdown(); begun.HasError())
                    return Result<Progress>::Failure(begun.ErrorValue());
            }
            const auto drained = composition.frontend->PollDrain();
            if (drained.HasError() || drained.Value() != Progress::Drained)
                return drained;
        }
        if (composition.uiRetirement) {
            const auto collected = composition.uiRetirement->CollectRetired();
            if (collected.HasError())
                return Result<Progress>::Failure(collected.ErrorValue());
            if (!collected.Value())
                return Result<Progress>::Success(Progress::Pending);
            composition.uiRetirement.reset();
        }
        if (composition.guiRenderer)
            DestroyEditorTextures(textures, *composition.guiRenderer);
        if (composition.frontend && composition.viewportRenderer)
            composition.frontend->DetachStaticMeshPassExecutor(*composition.viewportRenderer);
        if (composition.frontend && !composition.frontend->Capabilities().supportsRenderTargetResources)
            (void)composition.frontend->ReleaseOffscreenTarget(composition.viewportTarget);
        composition.viewportRenderer.reset();
        composition.guiRenderer.reset();
        ImGui::DestroyContext();
        composition.frontend.reset();
#if defined(HORO_HAS_RENDER_OPENGL)
        composition.openGlPresentationPort.reset();
#endif
#if defined(HORO_HAS_RENDER_METAL)
        composition.metalPresentationPort.reset();
        composition.metalGraphicsBridge.reset();
#endif
#if defined(HORO_HAS_RENDER_VULKAN)
        // Terminal frontend drain already invoked native device children, destroyed
        // the same-instance surface and released its loader; retire borrowed SDL parents last.
        composition.vulkanGraphicsBridge.reset();
        composition.vulkanPresentationPort.reset();
#endif
        SDL_DestroyWindow(window);
        SDL_Quit();
        return Result<Progress>::Success(Progress::Drained);
    }

    /** @brief Owns recovery between finite retirement polls; all presentation and prepared-module parents stay retained.
     * @details Five seconds is a reporting budget, never proof of drain or an implicit device-loss transition. Closing
     * the window cancels a requested restart but keeps this recovery owner alive until the original generation drains.
     * Each poll and event wait is finite. A wedged native acquisition may leave the process in this explicit recovery
     * state indefinitely; it cannot allocate another generation, detach its worker or report successful shutdown.
     */
    [[nodiscard]] static bool RetireEditorPresentation(SDL_Window *window, EditorRenderComposition &composition, EditorTextures &textures,
                                                       bool &allowRestart) {
        std::uint64_t lastReport = SDL_GetTicks();
        for (;;) {
            const auto retired = ShutdownEditorPresentation(window, composition, textures);
            if (retired.HasValue() && retired.Value() == Render::RenderBackendDrainProgress::Drained)
                return !composition.retirementFailure.has_value();
            if (retired.HasError() && !composition.retirementFailure) {
                composition.retirementFailure = retired.ErrorValue();
                LOG_ERROR("editor.renderer.retirement", "Renderer retirement failed [%s/%s]: %s; retaining owners for recovery.",
                          retired.ErrorValue().domain.Value().c_str(), retired.ErrorValue().code.Value().c_str(),
                          retired.ErrorValue().message.c_str());
                allowRestart = false;
            }
            if (const auto now = SDL_GetTicks(); now - lastReport >= 5'000) {
                LOG_WARN("editor.renderer.retirement",
                         "Renderer remains Pending after its finite retirement budget; retaining window, composition and modules.");
                lastReport = now;
            }
            SDL_Event event;
            if (SDL_WaitEventTimeout(&event, 8) && (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                                                                                     event.window.windowID == SDL_GetWindowID(window))))
                allowRestart = false;
        }
    }

    /** @brief Contains construction failures while the caller retains partial native owners for explicit retirement.
     * @details The caller admits an owned failure before acquiring native resources. Translating exception diagnostics may
     * allocate; if that fails, moving the admitted failure still returns to the caller's explicit retirement path.
     */
    [[nodiscard]] static Result<void> PrepareEditorPresentationResources(SDL_Window &window, const EditorGuiOptions &options,
                                                                         EditorRenderComposition &composition, EditorTextures &textures,
                                                                         Result<void> constructionFailure) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<Error>);
        static_assert(std::is_nothrow_move_constructible_v<Result<void>>);
        try {
            if (auto composed = CreateEditorRenderComposition(window, options, composition); composed.HasError())
                return composed;
            textures = LoadEditorTextures(*composition.guiRenderer);
            return Result<void>::Success();
        } catch (const std::exception &exception) {
            try {
                return Result<void>::Failure(MakeEditorRendererError("editor.renderer.construction_failed", exception.what()));
            } catch (...) {
                // Diagnostic allocation must not bypass retirement of the caller's partial native owners.
                return constructionFailure;
            }
        } catch (...) {  // NOSONAR(cpp:S2738) Host ownership survives native construction failure.
            return constructionFailure;
        }
    }

    /** @brief Creates the SDL, ImGui, renderer, and texture presentation in dependency order. */
    [[nodiscard]] static std::optional<EditorPresentation> StartEditorPresentation(const PreparedEditorStartup &startup) {
        std::optional<Result<void>> constructionFailure;
        try {
            // Reserve a complete typed failure before any window, context or native renderer can require retirement.
            constructionFailure.emplace(Result<void>::Failure(
                MakeEditorRendererError("editor.renderer.construction_failed", "Renderer construction raised an unknown exception.")));
        } catch (const std::bad_alloc &) {
            std::fputs("Renderer startup cannot reserve its construction failure before acquiring native resources.\n", stderr);
            return std::nullopt;
        }
        SDL_Window *window = nullptr;
        if (!InitializeSdlAndCreateWindow(window, startup.moduleInfo->windowRequirements))
            return std::nullopt;
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        auto fonts = LoadEditorFonts(io, QueryRasterizerDensity(window));
        Theme::Apply(ImGui::GetStyle());
        ApplySavedThemePreference();
        EditorRenderComposition composition;
#if defined(HORO_HAS_OPENXR_VULKAN_HOST)
        composition.xrSession = startup.xrNative.get();
#endif
        EditorTextures textures;
        if (auto composed =
                PrepareEditorPresentationResources(*window, startup.options, composition, textures, std::move(*constructionFailure));
            composed.HasError()) {
            try {
                LOG_CRITICAL("editor.renderer", "Renderer startup failed for '%s': %s", startup.options.rendererBackend.c_str(),
                             composed.ErrorValue().message.c_str());
            } catch (const std::bad_alloc &) {
                Log::Logger::WriteEmergency("editor.renderer", Log::Level::Critical, composed.ErrorValue().message);
            }
            bool allowRestart = false;
            static_cast<void>(RetireEditorPresentation(window, composition, textures, allowRestart));
            return std::nullopt;
        }
        return EditorPresentation{window, std::move(fonts), std::move(composition), std::move(textures)};
    }

    /** @brief Preserves the session's shutdown and renderer restart outcomes at the process boundary. */
    [[nodiscard]] static int CompleteEditorSession(PreparedEditorStartup &startup, const EditorSessionResult &session,
                                                   const char *executablePath) {
        if (!session.healthy) {
            startup.modules->DeactivateAll();
            Log::Logger::Shutdown();
            return 1;
        }
        if (session.rendererRestart.has_value()) {
            LOG_INFO("editor.renderer", "Restarting editor with project renderer '%s' for '%s'.",
                     session.rendererRestart->backendId.c_str(), session.rendererRestart->projectRoot.c_str());
            Log::Logger::Shutdown();
            return RelaunchEditorForProject(executablePath, *session.rendererRestart, startup.options);
        }
        startup.modules->DeactivateAll();
        Log::Logger::Shutdown();
        return 0;
    }

    /** @brief Retires presentation before publishing the session outcome or permitting a renderer restart. */
    static void RetireEditorSessionPresentation(EditorPresentation &presentation, EditorSessionResult &session) {
        bool allowRestart = session.rendererRestart.has_value();
        session.healthy =
            RetireEditorPresentation(presentation.window, presentation.composition, presentation.textures, allowRestart) && session.healthy;
        if (!allowRestart)
            session.rendererRestart.reset();
    }

    // ── public entry ─────────────────────────────────────────────────────────

    /** @copydoc RunEditorGuiApp */
    int RunEditorGuiApp(const int argc, char **argv, const EditorUpdateHostServices *updateHost,
                        std::shared_ptr<const Render::IShaderCompilerAdapter> shaderCompiler,
                        std::shared_ptr<const Render::ShaderCompilerTargetDescriptor> uiPaintTarget,
                        const EditorOpenXRHostServices *xrHost) {
        // ── Bootstrap logging before any subsystem ───────────────────────
        auto observabilitySession = InitializeEditorObservability();
        auto saveTelemetryResult = Runtime::SaveTelemetryRegistration::Create();
        if (saveTelemetryResult.HasError())
            return 1;
        auto saveTelemetry = std::move(saveTelemetryResult).Value();
        EditorTelemetry editorTelemetry = RegisterEditorTelemetry();
        auto structuredLogStore = std::make_shared<Log::StructuredLogStore>(4096U);
        Log::Logger::SetStructuredLogStore(structuredLogStore);
        BuildOutputStore buildOutputStore{2048U};
        OperationStore operationStore{64U, 200U, std::make_shared<LoggingOperationHistorySink>()};

        // Setup base MDC for the whole application run
        Log::LogContext appCtx("app", "horo-editor", "run_id", "1");

        Log::Logger::DumpStartupInfo();

        if (!MigrateStartupUserState()) {
            saveTelemetry.reset();
            Log::Logger::Shutdown();
            return 1;
        }

        auto opts = ParseOptions(std::span{argv, static_cast<std::size_t>(argc)});
        std::vector<RecentProjectEntry> recentProjects = LoadRecentProjectsFromDisk();
        WelcomeScreenController ctrl{recentProjects};
        auto vm = ctrl.BuildViewModel();

        if (opts.textPreview) {
            std::fputs(RenderWelcomeScreenText(vm).c_str(), stdout);
            return 0;
        }

        auto prepared = PrepareEditorStartup(std::move(opts), xrHost, std::span{argv, static_cast<std::size_t>(argc)});
        if (!prepared) {
            saveTelemetry.reset();
            Log::Logger::Shutdown();
            return 1;
        }

        if (auto xrPrepared = PrepareSelectedEditorXrStartup(*prepared); xrPrepared.HasError()) {
            LOG_CRITICAL("editor.xr", "%s", xrPrepared.ErrorValue().message.c_str());
            RetireEditorXrStartup(*prepared);
            saveTelemetry.reset();
            Log::Logger::Shutdown();
            return 1;
        }
        auto presentation = StartEditorPresentation(*prepared);
        if (!presentation) {
            RetireEditorXrStartup(*prepared);
            saveTelemetry.reset();
            Log::Logger::Shutdown();
            return 1;
        }

        LOG_INFO("editor.startup", "Editor initialised with renderer '%s' — entering main loop", prepared->options.rendererBackend.c_str());

        EditorSessionResult session =
            RunEditorSession({*prepared, *presentation->window, ImGui::GetIO(), presentation->fonts, presentation->textures,
                              presentation->composition, editorTelemetry, *structuredLogStore, buildOutputStore, operationStore, updateHost,
                              std::move(shaderCompiler), std::move(uiPaintTarget)});

        RetireEditorSessionPresentation(*presentation, session);
        RetireEditorXrStartup(*prepared);
        saveTelemetry.reset();
        return CompleteEditorSession(*prepared, session, argv[0]);
    }
}  // namespace Horo::Editor
