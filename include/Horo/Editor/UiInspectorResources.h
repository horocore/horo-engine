#pragma once
/** @file UiInspectorResources.h
 * @brief Host-composed immutable asset/style evidence used before UI inspector document publication.
 */
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Runtime/Ui/UiDocument.h"

#include <filesystem>
#include <functional>
#include <memory>

namespace Horo::Editor {
    /** @brief Pinned project assets and optional real Runtime UI style registry; never an inspector-created registry. */
    struct UiInspectorResourceSnapshot final {
        Assets::AssetRegistrySnapshot assets;
        std::shared_ptr<const Runtime::Ui::RuntimeStyleRegistry> styles;
        std::uint64_t publication{}; /**< Capability-owner monotonic fence, including removal/reinstall of the same style generation. */
        Runtime::Ui::RuntimeStyleAssetId theme; /**< Actual host-selected fallback theme for elements without an explicit style asset. */
        /** @brief Returns the active style generation, or invalid when absent/retiring. @return Exact generation. */
        [[nodiscard]] Runtime::Ui::RuntimeStyleGeneration StyleGeneration() const noexcept;
        /** @brief Validate style references/categories and image/font dependencies against actual pinned owners.
         * @param document Current canonical dependency manifest. @param presentation Complete requested values.
         * @return Typed missing/type/category/lifecycle error before document mutation, or success.
         */
        [[nodiscard]] Result<void> Validate(const Runtime::Ui::UiDocument &document,
                                            const Runtime::Ui::UiElementPresentation &presentation) const;
    };

    /** @brief Application-owned owner-thread composition over the canonical Assets registry and an explicit style lease.
     * @details The application outlives workspaces. No discovery, file I/O, synthetic schema or backend selection occurs.
     * Missing style composition rejects style edits and state changes affecting an existing style cascade.
     * Layout/text and state edits without style references need no style registry.
     */
    class UiInspectorResources final {
    public:
        /** @brief Bind the actual project Assets publisher. @param assets Host-owned registry outliving this capability. */
        explicit UiInspectorResources(const Assets::AssetRegistry &assets) noexcept;
        /** @brief Pin current actual resource generations. @return Owned immutable read evidence. */
        [[nodiscard]] UiInspectorResourceSnapshot Capture() const;
        /** @brief Explicitly publish a real active game/preview registry on the same owner thread.
         * @param styles Registry lease from the actual style producer, or null to close admission.
         * @param theme Real selected fallback style asset; required with a non-null registry.
         * @return Success or typed lifecycle/reference error; no cloud/source configuration changes.
         */
        [[nodiscard]] Result<void> PublishStyles(std::shared_ptr<const Runtime::Ui::RuntimeStyleRegistry> styles,
                                                 Runtime::Ui::RuntimeStyleAssetId theme = {});

    private:
        const Assets::AssetRegistry &assets_;
        std::shared_ptr<const Runtime::Ui::RuntimeStyleRegistry> styles_;
        std::uint64_t publication_{1};
        Runtime::Ui::RuntimeStyleAssetId theme_;
    };

    /** @brief Application composition factory producing one resource capability per exact workspace activation.
     * @details The root and never-reused workspace owner are supplied at activation; a real project style producer may
     * publish its registry/theme before returning. Result ownership stays with the workspace host, outliving its controller.
     */
    using UiInspectorResourcesFactory =
        std::function<Result<std::unique_ptr<UiInspectorResources>>(const std::filesystem::path &, Assets::AssetId)>;
}  // namespace Horo::Editor
