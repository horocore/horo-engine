#pragma once
/** @file UiInspectorResources.h
 * @brief Host-composed immutable asset/style evidence used before UI inspector document publication.
 */
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Runtime/Ui/UiDocument.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Editor {
    /** @brief Registered real project namespace shown before a theme is active. */
    struct UiInspectorTheme final {
        Runtime::Ui::RuntimeStyleAssetId id;
        std::string label;
    };

    /** @brief Complete style preparation result returned by the explicit application adapter. */
    struct UiInspectorStyleCandidate final {
        std::shared_ptr<const Runtime::Ui::RuntimeStyleRegistry> styles;
        Runtime::Ui::RuntimeStyleAssetId theme;
        Assets::AssetRegistryRevision assets;
        std::shared_ptr<const void> resources;
        std::shared_ptr<const void> sourceLease;
    };

    /** @brief Narrow owner-thread callbacks; submit starts owned tooling work, poll never performs I/O. */
    struct UiInspectorStyleProducer final {
        std::function<Result<std::vector<UiInspectorTheme>>(const Assets::AssetRegistrySnapshot &)> themes;
        std::function<Result<void>(Assets::AssetRegistrySnapshot, Runtime::Ui::RuntimeStyleAssetId)> submit;
        std::function<Result<std::optional<UiInspectorStyleCandidate>>(Assets::AssetRegistryRevision)> poll;
        std::function<void()> shutdown;
    };

    /** @brief Pinned project assets and optional real Runtime UI style registry; never an inspector-created registry. */
    struct UiInspectorResourceSnapshot final {
        Assets::AssetRegistrySnapshot assets;
        std::shared_ptr<const Runtime::Ui::RuntimeStyleRegistry> styles;
        std::uint64_t publication{}; /**< Capability-owner monotonic fence, including removal/reinstall of the same style generation. */
        Runtime::Ui::RuntimeStyleAssetId theme; /**< Actual host-selected fallback theme for elements without an explicit style asset. */
        std::shared_ptr<const std::vector<UiInspectorTheme>> themes;
        bool pending{};
        std::shared_ptr<const void> resourceLease;
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
        /** @brief Cancel/drain the bound application preparation before releasing its borrowed services. */
        ~UiInspectorResources();
        UiInspectorResources(const UiInspectorResources &) = delete;
        UiInspectorResources &operator=(const UiInspectorResources &) = delete;
        /** @brief Install an explicit application producer without starting work. @param producer Owned callbacks. @return Admission
         * result. */
        [[nodiscard]] Result<void> BindStyleProducer(UiInspectorStyleProducer producer);
        /** @brief Poll source-event theme metadata and terminal publication, without filesystem work. @return Typed terminal error or
         * success. */
        [[nodiscard]] Result<void> Refresh();
        /** @brief Start preparation for one actual registered namespace. @param theme Explicit user choice. @return Admission result. */
        [[nodiscard]] Result<void> SelectTheme(Runtime::Ui::RuntimeStyleAssetId theme);
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
        std::optional<UiInspectorStyleProducer> producer_;
        std::shared_ptr<const std::vector<UiInspectorTheme>> themes_;
        std::shared_ptr<const void> resources_;
        Assets::AssetRegistryRevision themesAssets_, preparedAssets_;
        bool pending_{};
    };

    /** @brief Application composition factory producing one resource capability per exact workspace activation.
     * @details The root and never-reused workspace owner are supplied at activation; the application binds its actual
     * asynchronous style producer and theme metadata before returning. Explicit user theme selection starts preparation. Result ownership
     * stays with the workspace host, outliving its controller.
     */
    using UiInspectorResourcesFactory =
        std::function<Result<std::unique_ptr<UiInspectorResources>>(const std::filesystem::path &, Assets::AssetId)>;
}  // namespace Horo::Editor
