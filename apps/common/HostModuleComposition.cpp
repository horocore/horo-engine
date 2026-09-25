#include "HostModuleComposition.h"

#include "Horo/Foundation/ErrorCode.h"
#include "Horo/PlatformServices/PlatformServiceErrors.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        constexpr ModuleContractVersion kContractVersion{1, 0, 0};

        /** @brief Explicit backend identities supported by host module composition. */
        constexpr std::array<std::pair<std::string_view, HostRenderer>, 2> kHostRenderers{{
            {"opengl", HostRenderer::OpenGL},
            {"metal", HostRenderer::Metal},
        }};

        /**
         * @brief Builds inert metadata for one module and its required dependencies.
         * @param id Canonical module identity.
         * @param dependencies Canonical identities of required provider modules.
         * @return Complete inert descriptor using the current internal contract version.
         */
        [[nodiscard]] ModuleDescriptor Describe(std::string id, const std::initializer_list<std::string_view> dependencies = {}) {
            ModuleDescriptor descriptor{.id = ModuleId{std::move(id)}, .version = kContractVersion};
            descriptor.dependencies.reserve(dependencies.size());
            for (const std::string_view dependency : dependencies) {
                descriptor.dependencies.push_back(
                    ModuleDependency{.module = ModuleId{std::string{dependency}}, .minimumVersion = kContractVersion});
            }
            return descriptor;
        }

        [[nodiscard]] ModuleDescriptor DescribePlatformServices() {
            ModuleDescriptor descriptor = Describe("horo.platform.services", {"horo.foundation", "horo.platform"});
            descriptor.errorDomains.push_back(PlatformServices::PlatformServiceErrorDomain());
            return descriptor;
        }

        /**
         * @brief Creates a typed failure for an unsupported host composition selection.
         * @tparam T Success value type expected by the caller.
         * @param message Actionable explanation of the invalid selection.
         * @return Failed result in the application host error domain.
         */
        template <typename T> [[nodiscard]] Result<T> InvalidSelection(std::string message) {
            return Result<T>::Failure(Error{.code = ErrorCode{"application.host.invalid_module_selection"},
                                            .domain = ErrorDomainId{"horo.application.host"},
                                            .severity = ErrorSeverity::Critical,
                                            .message = std::move(message)});
        }

        /**
         * @brief Describes the renderer-independent modules linked into the editor host.
         * @return Inert descriptors for the editor's shared module closure.
         */
        [[nodiscard]] std::vector<ModuleDescriptor> DescribeEditorCoreModules() {
            return {
                Describe("horo.foundation"),
                Describe("horo.application", {"horo.foundation"}),
                Describe("horo.application.project_migrations", {"horo.application"}),
                Describe("horo.platform", {"horo.foundation"}),
                DescribePlatformServices(),
                Describe("horo.runtime", {"horo.foundation"}),
                Describe("horo.assets", {"horo.foundation"}),
                Describe("horo.input", {"horo.foundation"}),
                Describe("horo.input.sdl", {"horo.input"}),
                Describe("horo.gameplay.api", {"horo.foundation"}),
                Describe("horo.render.api", {"horo.foundation"}),
                Describe("horo.scene.model", {"horo.foundation", "horo.render.api"}),
                Describe("horo.runtime.scene", {"horo.foundation", "horo.runtime", "horo.assets", "horo.gameplay.api", "horo.scene.model"}),
                Describe("horo.gameplay.runtime", {"horo.gameplay.api", "horo.runtime.scene"}),
                Describe("horo.gameplay.module_host", {"horo.gameplay.runtime", "horo.platform"}),
                Describe("horo.gameplay.build", {"horo.foundation", "horo.platform", "horo.gameplay.module_host"}),
                Describe("horo.gameplay.lua", {"horo.gameplay.runtime"}),
                Describe("horo.render.backend_registry", {"horo.render.api"}),
                Describe("horo.render.frontend", {"horo.render.api", "horo.render.backend_registry"}),
                Describe("horo.editor.model", {"horo.foundation", "horo.scene.model", "horo.runtime.scene"}),
                Describe("horo.editor.viewport_scene", {"horo.editor.model"}),
                Describe("horo.editor.render_extraction", {"horo.editor.model", "horo.editor.viewport_scene"}),
                Describe("horo.editor.services",
                         {"horo.foundation", "horo.application", "horo.platform", "horo.editor.model", "horo.gameplay.module_host",
                          "horo.gameplay.build", "horo.gameplay.lua", "horo.input", "horo.application.project_migrations", "horo.assets"}),
                Describe("horo.extensions", {"horo.foundation", "horo.platform", "horo.assets"}),
                Describe("horo.gui", {"horo.editor.services", "horo.foundation", "horo.editor.render_extraction", "horo.extensions"}),
            };
        }
    }  // namespace

    /** @copydoc HostRendererFromBackendId */
    Result<HostRenderer> HostRendererFromBackendId(const std::string_view backendId) {
        const auto selected = std::ranges::find(kHostRenderers, backendId, &std::pair<std::string_view, HostRenderer>::first);
        if (selected == kHostRenderers.end())
            return InvalidSelection<HostRenderer>("Unsupported interactive renderer backend identity: " + std::string{backendId});
        return Result<HostRenderer>::Success(selected->second);
    }

    /** @copydoc DescribeHostModules */
    Result<std::vector<ModuleDescriptor>> DescribeHostModules(const HostModuleSelection &selection) {
        if (selection.host == HostKind::Headless) {
            if (selection.renderer != HostRenderer::None || selection.includeOpenTelemetry) {
                return InvalidSelection<std::vector<ModuleDescriptor>>(
                    "The current headless host does not link an interactive renderer or OpenTelemetry module.");
            }
            std::vector<ModuleDescriptor> modules;
            modules.reserve(8);
            modules.push_back(Describe("horo.foundation"));
            modules.push_back(Describe("horo.security", {"horo.foundation"}));
            modules.push_back(Describe("horo.platform", {"horo.foundation", "horo.security"}));
            modules.push_back(DescribePlatformServices());
            modules.push_back(Describe("horo.assets", {"horo.foundation"}));
            modules.push_back(Describe("horo.application", {"horo.foundation"}));
            modules.push_back(Describe("horo.extensions", {"horo.foundation", "horo.platform", "horo.assets", "horo.security"}));
            modules.push_back(Describe("horo.host.cli", {"horo.application", "horo.extensions"}));
            return Result<std::vector<ModuleDescriptor>>::Success(std::move(modules));
        }

        if (selection.renderer == HostRenderer::None) {
            return InvalidSelection<std::vector<ModuleDescriptor>>(
                "The graphical editor host requires one concrete interactive renderer module.");
        }

        std::vector<ModuleDescriptor> modules = DescribeEditorCoreModules();

        /** @brief Canonical module identities selected for one interactive renderer. */
        struct RendererModules {
            std::string_view renderer;
            std::string_view viewport;
        };

        const RendererModules selectedModules = selection.renderer == HostRenderer::OpenGL
                                                    ? RendererModules{"horo.render.opengl", "horo.editor.viewport.opengl"}
                                                    : RendererModules{"horo.render.metal", "horo.editor.viewport.metal"};
        modules.push_back(Describe(std::string{selectedModules.renderer}, {"horo.render.backend_registry"}));
        modules.push_back(Describe(std::string{selectedModules.viewport}, {"horo.editor.viewport_scene", selectedModules.renderer}));
        if (selection.includeOpenTelemetry)
            modules.push_back(Describe("horo.observability.opentelemetry", {"horo.foundation"}));

        ModuleDescriptor host =
            Describe("horo.host.editor", {"horo.gui", "horo.editor.services", "horo.editor.render_extraction", "horo.render.frontend",
                                          "horo.runtime", "horo.runtime.scene", "horo.extensions", "horo.platform", "horo.input.sdl",
                                          "horo.application.project_migrations", selectedModules.viewport});
        host.dependencies.push_back(ModuleDependency{.module = ModuleId{"horo.observability.opentelemetry"},
                                                     .minimumVersion = kContractVersion,
                                                     .kind = ModuleDependencyKind::Optional});
        modules.push_back(std::move(host));
        return Result<std::vector<ModuleDescriptor>>::Success(std::move(modules));
    }

    /** @copydoc ComposeHostModules */
    Result<std::unique_ptr<ModuleHost>> ComposeHostModules(const HostModuleSelection &selection) {
        auto described = DescribeHostModules(selection);
        if (described.HasError())
            return Result<std::unique_ptr<ModuleHost>>::Failure(described.ErrorValue());

        auto host = std::make_unique<ModuleHost>();
        for (const ModuleDescriptor &descriptor : described.Value()) {
            if (const Result<void> registered = host->Register(descriptor); registered.HasError())
                return Result<std::unique_ptr<ModuleHost>>::Failure(registered.ErrorValue());
        }
        if (const Result<std::size_t> activated = host->ActivateRegistered(nullptr); activated.HasError())
            return Result<std::unique_ptr<ModuleHost>>::Failure(activated.ErrorValue());
        return Result<std::unique_ptr<ModuleHost>>::Success(std::move(host));
    }
}  // namespace Horo::Application::Internal
