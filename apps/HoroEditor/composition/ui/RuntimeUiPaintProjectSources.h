#pragma once
/** @file RuntimeUiPaintProjectSources.h
 * @brief Application-owned committed project UI loading and canonical Scene activation composition. */
#include "Horo/Application/ShaderBuildService.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "Horo/Runtime/Ui/UiAssetLoading.h"
#include "Horo/Runtime/Ui/UiPresentationReceipt.h"
#include "RuntimeUiPaintHost.h"

#include <filesystem>
#include <functional>
#include <variant>

namespace Horo::Application::Internal {
    /** @brief Resolves a centered local XY canvas using the current Scene's exact bounded parent transforms.
     * @param scene Current view of the actual selected Scene owner.
     * @param entity Exact owner of the registered canvas reference.
     * @param loaded Exact registry-pinned cooked document matching the entity's Canvas reference.
     * @param maximumRasterAxis Selected render owner's admitted texture-dimension ceiling.
     * @return World placement and offscreen extent, or typed identity, capacity or projection failure.
     * @details Logical (0,0) is local (-0.5,+0.5,0). Full width is +X, full height is -Y, and inherited scale sets physical size.
     * The result grants no renderer, input or Scene publication authority. */
    [[nodiscard]] Result<RuntimeUiPaintWorldCanvas> ResolveRuntimeUiPaintWorldCanvas(Runtime::RuntimeSceneView scene,
                                                                                     Runtime::EntityRef entity,
                                                                                     const Runtime::Ui::UiRuntimeAssetLoadResult &loaded,
                                                                                     std::uint32_t maximumRasterAxis);
    /** @brief Refreshes exact Scene geometry without reloading assets or changing the admitted raster projection.
     * @param scene Current owner-lane Scene view for the actual frame.
     * @param canvas World canvas binding prepared from the same Scene and registered document.
     * @return Current local-to-Scene plane, or typed stale/foreign/invalid geometry.
     * @pre Caller checks the binding against its own current Runtime UI document publication before encoding. */
    [[nodiscard]] Result<Runtime::Ui::UiWorldCanvasProjection> CaptureRuntimeUiPaintWorldCanvasPlane(
        Runtime::RuntimeSceneView scene, const RuntimeUiPaintWorldCanvas &canvas);

    /** @brief Converts the same plane retained for hit testing to the renderer's centered quad coordinates.
     * @param plane Current World canvas plane in the selected scene's coordinate system.
     * @return Affine local-to-scene matrix, or a typed failure for a degenerate or nonfinite plane. */
    [[nodiscard]] Result<Math::Mat4> BuildRuntimeUiPaintWorldCanvasTransform(const Runtime::Ui::UiWorldCanvasProjection &plane);

    /** @brief Explicit admitted launcher route, used only for a real authored source at a load-time Scene safe point.
     * @details Both borrowed services outlive project factories and their synchronous preparation jobs. No tool discovery occurs. */
    struct RuntimeUiPaintProjectCompiler final {
        std::reference_wrapper<JobSystem> jobs;
        std::reference_wrapper<ShaderBuildService> builds;
        std::filesystem::path source;
        Render::ShaderCompilerTargetDescriptor target;
    };

    /** @brief Explicit shipped package selected by the application, not discovered by Scene or feature code. */
    struct RuntimeUiPaintPackagedAssets final {
        std::filesystem::path assetRoot;
        Render::ShaderTargetBackend backend{Render::ShaderTargetBackend::Null};
    };

    /** @brief Explicit product admission; an absent kernel is lawful only while no authored UI source is requested. */
    struct RuntimeUiPaintProjectPolicy final {
        std::shared_ptr<const Render::CookedUiPaintKernel> kernel;
        std::variant<std::monostate, RuntimeUiPaintProjectCompiler, RuntimeUiPaintPackagedAssets> source;
        /**< Exactly one explicit load-time producer; absent remains a typed tool-unavailable boundary. */
        std::uint32_t maximumElements{512};
        std::uint32_t previousInstanceSlot{};
        std::uint32_t previousCanvasSlot{};
        std::optional<std::filesystem::path> vulkanSceneCanvasAssets; /**< Exact package root admitted by the selected viewport. */
        bool enableVulkanCameraCanvas{true};                          /**< Whether the viewport can draw an authored camera canvas. */
    };

    /** @brief One application source namespace, shared by real preview/Play factories and pinned through source retirement.
     * @details The allocator is the sole issuer for this ownership generation. Modules receive only scoped RuntimeUiClient,
     * never this host source factory. No document, provider or backend is discovered from ambient state. */
    class RuntimeUiPaintProjectSources final {
        struct Storage;

        class ConstructionKey final {
            friend class RuntimeUiPaintProjectSources;
            ConstructionKey() = default;
        };

    public:
        /** @brief Factory-only construction after validated storage preparation. */
        explicit RuntimeUiPaintProjectSources(ConstructionKey, std::shared_ptr<Storage> storage) : storage_(std::move(storage)) {}

        ~RuntimeUiPaintProjectSources();
        /** @brief Captures the actual application port, sole element allocator and admitted shader/source policy.
         * @param port Canonical fixed lifecycle composition. @param allocator Actual sole namespace issuer.
         * @param policy Explicit finite product and prepared shader evidence.
         * @return Application source factory or typed invalid ownership/policy failure. */
        [[nodiscard]] static Result<std::shared_ptr<RuntimeUiPaintProjectSources>> Create(
            RuntimeUiPaintPort port, std::shared_ptr<Runtime::Ui::UiElementSlotAllocator> allocator, RuntimeUiPaintProjectPolicy policy);
        /** @brief Creates the last generic Scene participant over this exact project's captured registry/artifact loader.
         * @param scenes Actual Scene service, alive through participant teardown.
         * @param registry Exact committed project registry. @param loads Owned Runtime UI loader over full artifact envelopes.
         * @param target Explicit captured cook target. @return Participant borrowing only these longer-lived project owners. */
        [[nodiscard]] std::unique_ptr<Runtime::SceneActivationParticipant> MakeParticipant(Runtime::RuntimeSceneService &scenes,
                                                                                           Assets::AssetRegistry &registry,
                                                                                           Runtime::Ui::UiRuntimeAssetLoadService &loads,
                                                                                           std::optional<Assets::AssetCookTargetId> target);

    private:
        std::shared_ptr<Storage> storage_;
    };
}  // namespace Horo::Application::Internal
