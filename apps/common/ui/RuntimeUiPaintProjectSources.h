#pragma once
/** @file RuntimeUiPaintProjectSources.h
 * @brief Application-owned committed project UI loading and canonical Scene activation composition. */
#include "Horo/Application/ShaderBuildService.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "RuntimeUiPaintHost.h"

#include <filesystem>
#include <functional>
#include <variant>

namespace Horo::Application::Internal {
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
    };

    /** @brief One application source namespace, shared by real preview/Play factories and pinned through source retirement.
     * @details The allocator is the sole issuer for this ownership generation. Modules receive only scoped RuntimeUiClient,
     * never this host source factory. No document, provider or backend is discovered from ambient state. */
    class RuntimeUiPaintProjectSources final {
        struct Storage;

    public:
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
        explicit RuntimeUiPaintProjectSources(std::shared_ptr<Storage> storage) : storage_(std::move(storage)) {}

        std::shared_ptr<Storage> storage_;
    };
}  // namespace Horo::Application::Internal
