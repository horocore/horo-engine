#include "Horo/Editor/UiInspectorResources.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Editor {
    namespace {
        using namespace Runtime::Ui;

        /** @brief Preserve resource-domain failures through the editor boundary. */
        Result<void> Failure(const ErrorCodeDescriptor &error) {
            return Result<void>::Failure(MakeError(error));
        }

        /** @brief Check actual residency and the source document's typed dependency declaration. */
        Result<void> Asset(const UiInspectorResourceSnapshot &snapshot, const UiDocument &document, const Assets::AssetId id) {
            const auto *record = snapshot.assets.Find(id);
            if (!record)
                return Failure(UiErrors::AssetMissing);
            const auto dependency = std::ranges::find(document.Dependencies(), id, &UiAssetDependency::asset);
            if (dependency == document.Dependencies().end())
                return Failure(UiErrors::DependencyInvalid);
            if (record->type != dependency->expectedType)
                return Failure(UiErrors::AssetTypeMismatch);
            return Result<void>::Success();
        }

        /** @brief Only the existing imagery/typography variants carry asset references. */
        Result<void> Value(const UiInspectorResourceSnapshot &snapshot, const UiDocument &document, const UiStyleValue &value) {
            if (const auto *image = std::get_if<UiStyleImage>(&value))
                return Asset(snapshot, document, image->asset);
            if (const auto *font = std::get_if<UiStyleTypography>(&value))
                return Asset(snapshot, document, font->family);
            return Result<void>::Success();
        }

        /** @brief Resolve real token values before checking their asset references. */
        Result<void> Assignments(const UiInspectorResourceSnapshot &snapshot, const UiDocument &document,
                                 const std::span<const UiStyleAssignment> assignments) {
            auto valid = ValidateUiStyleAssignments(*snapshot.styles, assignments);
            if (valid.HasError())
                return valid;
            for (const auto &assignment : assignments) {
                auto value = assignment.value.referencesToken ? snapshot.styles->ResolveToken(assignment.value.token)
                                                              : Result<UiStyleValue>::Success(assignment.value.literal);
                if (value.HasError())
                    return Result<void>::Failure(value.ErrorValue());
                if (const auto asset = Value(snapshot, document, value.Value()); asset.HasError())
                    return asset;
            }
            return Result<void>::Success();
        }

        /** @brief Validate state-dependent resources even when that state is not currently selected. */
        Result<void> States(const UiInspectorResourceSnapshot &snapshot, const UiDocument &document,
                            const std::span<const UiStyleStateOverride> states) {
            for (const auto &state : states)
                if (const auto valid = Assignments(snapshot, document, state.assignments); valid.HasError())
                    return valid;
            return Result<void>::Success();
        }

        /** @brief Walk the real bounded asset inheritance chain without synthesizing a style owner. */
        Result<void> StyleAsset(const UiInspectorResourceSnapshot &snapshot, const UiDocument &document, RuntimeStyleAssetId id) {
            for (std::size_t depth = 0; id.IsValid() && depth < MaximumUiStyleInheritanceDepth; ++depth) {
                const auto assets = snapshot.styles->Assets();
                const auto found = std::ranges::find(assets, id, &UiStyleAssetDefinition::id);
                if (found == assets.end())
                    return Failure(UiErrors::StyleReferenceInvalid);
                if (const auto valid = Assignments(snapshot, document, found->assignments); valid.HasError())
                    return valid;
                if (const auto valid = States(snapshot, document, found->states); valid.HasError())
                    return valid;
                id = found->base.value_or(RuntimeStyleAssetId{});
            }
            return id.IsValid() ? Failure(UiErrors::StyleCycle) : Result<void>::Success();
        }

        /** @brief Walk real selected class inheritance; unselected classes create no false resource requirement. */
        Result<void> Class(const UiInspectorResourceSnapshot &snapshot, const UiDocument &document, UiStyleClassReference reference) {
            for (std::size_t depth = 0; reference.IsValid() && depth < MaximumUiStyleInheritanceDepth; ++depth) {
                const auto assets = snapshot.styles->Assets();
                const auto asset = std::ranges::find(assets, reference.asset, &UiStyleAssetDefinition::id);
                if (asset == assets.end())
                    return Failure(UiErrors::StyleReferenceInvalid);
                const auto found = std::ranges::find(asset->classes, reference.id, &UiStyleClassDefinition::id);
                if (found == asset->classes.end())
                    return Failure(UiErrors::StyleReferenceInvalid);
                if (const auto valid = StyleAsset(snapshot, document, reference.asset); valid.HasError())
                    return valid;
                if (const auto valid = Assignments(snapshot, document, found->assignments); valid.HasError())
                    return valid;
                if (const auto valid = States(snapshot, document, found->states); valid.HasError())
                    return valid;
                reference = found->base.value_or(UiStyleClassReference{});
            }
            return reference.IsValid() ? Failure(UiErrors::StyleCycle) : Result<void>::Success();
        }
    }  // namespace

    /** @copydoc UiInspectorResourceSnapshot::StyleGeneration */
    Runtime::Ui::RuntimeStyleGeneration UiInspectorResourceSnapshot::StyleGeneration() const noexcept {
        return styles && styles->State() == Runtime::Ui::RuntimeStyleRegistryState::Active && theme.IsValid() && styles->HasAsset(theme)
                   ? styles->Generation()
                   : Runtime::Ui::RuntimeStyleGeneration{};
    }

    /** @copydoc UiInspectorResourceSnapshot::Validate */
    Result<void> UiInspectorResourceSnapshot::Validate(const UiDocument &document, const UiElementPresentation &presentation) const {
        if (!presentation.IsValid())
            return Failure(UiErrors::StyleInvalid);
        const bool hasStyles = presentation.styleAsset.IsValid() || presentation.typeClass.IsValid() || !presentation.classes.empty() ||
                               !presentation.inlineStyle.empty();
        if (!hasStyles)
            return Result<void>::Success();
        if (!StyleGeneration().IsValid())
            return Failure(UiErrors::StyleLifecycleUnavailable);
        auto selected = presentation;
        if (!selected.styleAsset.IsValid())
            selected.styleAsset = theme;
        if (const auto composition = ValidateUiElementPresentationStyles(*styles, selected); composition.HasError())
            return composition;
        if (const auto asset = StyleAsset(*this, document, selected.styleAsset); asset.HasError())
            return asset;
        if (presentation.typeClass.IsValid())
            if (const auto valid = Class(*this, document, presentation.typeClass); valid.HasError())
                return valid;
        for (const auto &reference : presentation.classes)
            if (const auto valid = Class(*this, document, reference); valid.HasError())
                return valid;
        return Assignments(*this, document, presentation.inlineStyle);
    }

    /** @copydoc UiInspectorResources::UiInspectorResources */
    UiInspectorResources::UiInspectorResources(const Assets::AssetRegistry &assets) noexcept : assets_(assets) {}

    /** @copydoc UiInspectorResources::Capture */
    UiInspectorResourceSnapshot UiInspectorResources::Capture() const {
        auto assets = assets_.Snapshot();
        const bool current = !producer_ || (!pending_ && assets.Revision() == preparedAssets_);
        return {std::move(assets), current ? styles_ : nullptr, publication_, theme_, themes_, pending_, resources_};
    }

    /** @copydoc UiInspectorResources::PublishStyles */
    Result<void> UiInspectorResources::PublishStyles(std::shared_ptr<const RuntimeStyleRegistry> styles, const RuntimeStyleAssetId theme) {
        if (styles && styles->State() != RuntimeStyleRegistryState::Active)
            return Failure(UiErrors::StyleLifecycleUnavailable);
        if (styles ? (!theme.IsValid() || !styles->HasAsset(theme)) : theme.IsValid())
            return Failure(UiErrors::StyleReferenceInvalid);
        if (styles == styles_ && theme == theme_)
            return Result<void>::Success();
        if (publication_ == std::numeric_limits<std::uint64_t>::max())
            return Failure(UiErrors::GenerationExhausted);
        styles_ = std::move(styles);
        theme_ = theme;
        ++publication_;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor

namespace Horo::Editor {
    /** @copydoc UiInspectorResources::~UiInspectorResources */
    UiInspectorResources::~UiInspectorResources() {
        if (producer_ && producer_->shutdown)
            producer_->shutdown();
    }

    /** @copydoc UiInspectorResources::BindStyleProducer */
    Result<void> UiInspectorResources::BindStyleProducer(UiInspectorStyleProducer producer) {
        if (producer_ || !producer.themes || !producer.submit || !producer.poll || !producer.shutdown)
            return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::StyleLifecycleUnavailable));
        producer_ = std::move(producer);
        return Refresh();
    }

    /** @copydoc UiInspectorResources::SelectTheme */
    Result<void> UiInspectorResources::SelectTheme(const Runtime::Ui::RuntimeStyleAssetId theme) {
        if (!producer_ || pending_)
            return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::StyleLifecycleUnavailable));
        if (publication_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::GenerationExhausted));
        const auto assets = assets_.Snapshot();
        const auto found =
            themes_ ? std::ranges::find(*themes_, theme, &UiInspectorTheme::id) : std::vector<UiInspectorTheme>::const_iterator{};
        if (!themes_ || found == themes_->end() || assets.Revision() != themesAssets_)
            return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::StyleSourceStale));
        const auto admitted = producer_->submit(assets, theme);
        if (admitted.HasError())
            return admitted;
        pending_ = true;
        ++publication_;
        return Result<void>::Success();
    }

    /** @copydoc UiInspectorResources::Refresh */
    Result<void> UiInspectorResources::Refresh() {
        try {
            if (!producer_)
                return Result<void>::Success();
            const auto assets = assets_.Snapshot();
            if (!themes_ || themesAssets_ != assets.Revision()) {
                auto themes = producer_->themes(assets);
                if (themes.HasError())
                    return Result<void>::Failure(themes.ErrorValue());
                if (themes.Value().size() > Runtime::Ui::MaximumUiStyleAssets)
                    return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::CapacityExceeded));
                themes_ = std::make_shared<const std::vector<UiInspectorTheme>>(std::move(themes).Value());
                themesAssets_ = assets.Revision();
            }
            if (!pending_)
                return Result<void>::Success();
            auto polled = producer_->poll(assets.Revision());
            if (polled.HasError()) {
                pending_ = false;
                preparedAssets_ = {};
                return Result<void>::Failure(polled.ErrorValue());
            }
            if (!polled.Value())
                return Result<void>::Success();
            auto candidate = std::move(*std::move(polled).Value());
            pending_ = false;
            if (candidate.assets != assets.Revision()) {
                preparedAssets_ = {};
                return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::AssetRegistryStale));
            }
            const auto published = PublishStyles(std::move(candidate.styles), candidate.theme);
            if (published.HasError()) {
                preparedAssets_ = {};
                return published;
            }
            resources_ = std::move(candidate.resources);
            preparedAssets_ = candidate.assets;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Editor
