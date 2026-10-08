#include "UiStyleInternal.h"
#include "UiThemeInternal.h"

#include <algorithm>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Runtime::Ui {
    struct UiThemeRuntime::Storage final {
        UiThemeRuntimeDescriptor descriptor;
        UiThemeRuntimeState state{UiThemeRuntimeState::Active};
        RuntimeStyleRegistry active;
        RuntimeStyleAssetId theme;
        RuntimeStyleGeneration issued;
        std::optional<UiThemeChangeId> pending;
        std::optional<RuntimeStyleRegistry> prepared;
        RuntimeStyleAssetId preparedTheme;
        std::uint32_t preparedFallbacks{};
        std::uint32_t activeFallbacks{};
        UiStyleResolver styles;
        UiLayoutEngine layouts;
        UiRenderExtractor renders;
        std::optional<UiThemeSnapshot> current;
        std::vector<UiStyleElementInput> styleInputs;
        std::vector<UiStyleClassReference> classes;
        std::vector<UiLayoutElementDescriptor> layoutInputs;
        std::vector<UiLayoutElementDescriptor> previousLayoutInputs;
        std::vector<UiDrawCommand> paints;

        /** @brief Owned revision counters for the coordinated layout and render snapshots. */
        struct SnapshotRevisions final {
            UiLayoutStyleRevision layout{UiLayoutStyleRevision::Create(1).Value()};
            UiRenderSnapshotRevision render{UiRenderSnapshotRevision::Create(1).Value()};
        };

        SnapshotRevisions revisions;

        Storage(const UiThemeRuntimeDescriptor &source, RuntimeStyleRegistry registry, const RuntimeStyleAssetId selection,
                UiStyleResolver resolver, UiLayoutEngine layout, UiRenderExtractor render)
            : descriptor(source), active(std::move(registry)), theme(selection), issued(active.Generation()), styles(std::move(resolver)),
              layouts(std::move(layout)), renders(std::move(render)) {
            styleInputs.reserve(source.style.elementCapacity);
            classes.reserve(source.classReferenceCapacity);
            layoutInputs.reserve(source.style.elementCapacity);
            previousLayoutInputs.reserve(source.style.elementCapacity);
            paints.reserve(source.style.elementCapacity);
        }

        [[nodiscard]] Result<void> CheckChange(const UiThemeChangeId change) const {
            if (state != UiThemeRuntimeState::Active)
                return StyleInternal::Failure(UiErrors::StyleLifecycleUnavailable);
            if (!pending || *pending != change)
                return StyleInternal::Failure(UiErrors::StyleSourceStale);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> BuildStyleInputs(const std::span<const UiThemeSurfaceInput> surfaces,
                                                    const RuntimeStyleAssetId selection) {
            if (surfaces.empty() || surfaces.size() > descriptor.style.elementCapacity)
                return StyleInternal::Failure(UiErrors::CapacityExceeded);
            styleInputs.clear();
            classes.clear();
            for (const auto &surface : surfaces) {
                auto input = surface.style;
                if (input.classes.size() > descriptor.classReferenceCapacity - classes.size())
                    return StyleInternal::Failure(UiErrors::CapacityExceeded);
                if (!input.asset.IsValid())
                    input.asset = selection;
                if (!input.typeClass.asset.IsValid() && input.typeClass.id.IsValid())
                    input.typeClass.asset = selection;
                const auto offset = classes.size();
                for (auto reference : input.classes) {
                    if (!reference.asset.IsValid() && reference.id.IsValid())
                        reference.asset = selection;
                    classes.push_back(reference);
                }
                input.classes = std::span<const UiStyleClassReference>{classes}.subspan(offset, input.classes.size());
                styleInputs.push_back(input);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> Project(const UiComputedStyleSnapshot &snapshot, const std::span<const UiThemeSurfaceInput> surfaces) {
            layoutInputs.resize(surfaces.size());
            paints.resize(surfaces.size());
            for (std::size_t index = 0; index < surfaces.size(); ++index)
                if (const auto result =
                        ThemeInternal::ProjectSurface(snapshot, surfaces[index], descriptor.properties, layoutInputs[index], paints[index]);
                    result.HasError())
                    return result;
            std::ranges::sort(layoutInputs, {}, &UiLayoutElementDescriptor::element);
            return Result<void>::Success();
        }

        /** @brief Arranges the projected surface inputs and correlates paint rectangles with the private layout candidate. */
        [[nodiscard]] Result<UiLayoutSnapshot> ArrangeSurfaces(const UiElementTree &tree, const UiThemeUpdateRequest &request) {
            auto evaluator = UiDeclarativeLayoutEvaluator::Create(layoutInputs, request.intrinsic);
            if (evaluator.HasError())
                return Result<UiLayoutSnapshot>::Failure(evaluator.ErrorValue());
            // Track the last inputs submitted to the private layout cache, including failed outer transactions.
            // Paint-only theme changes reuse their logical boxes; the public snapshot still advances as one unit.
            if (layoutInputs != previousLayoutInputs) {
                const auto next = revisions.layout.Next();
                if (next.HasError())
                    return Result<UiLayoutSnapshot>::Failure(next.ErrorValue());
                revisions.layout = next.Value();
                previousLayoutInputs = layoutInputs;
            }
            auto layoutRequest = request.layout;
            layoutRequest.sources.style = revisions.layout;
            layoutRequest.evaluator = &evaluator.Value();
            auto arranged = layouts.Update(tree, layoutRequest);
            if (arranged.HasError())
                return Result<UiLayoutSnapshot>::Failure(arranged.ErrorValue());
            for (auto &paint : paints) {
                const auto record = arranged.Value().Get(paint.element);
                if (record.HasError())
                    return Result<UiLayoutSnapshot>::Failure(record.ErrorValue());
                paint.rect = record.Value().arrangement.borderBox;
            }
            return arranged;
        }

        [[nodiscard]] Result<UiThemeSnapshot> Build(const UiElementTree &tree, const UiThemeUpdateRequest &request,
                                                    const RuntimeStyleRegistry &registry, const RuntimeStyleAssetId selection,
                                                    const std::uint32_t fallbacks) {
            struct BorrowReset final {
                explicit BorrowReset(std::vector<UiStyleElementInput> &source) : inputs(source) {}

                BorrowReset(const BorrowReset &) = delete;
                BorrowReset &operator=(const BorrowReset &) = delete;
                BorrowReset(BorrowReset &&) = delete;
                BorrowReset &operator=(BorrowReset &&) = delete;

                std::vector<UiStyleElementInput> &inputs;

                ~BorrowReset() {
                    inputs.clear();
                }
            };

            BorrowReset reset{styleInputs};

            if (request.styleSources.document != request.layout.sources.document ||
                request.styleSources.tree != request.layout.sources.tree)
                return StyleInternal::Failure<UiThemeSnapshot>(UiErrors::StyleSourceStale);
            if (const auto result = BuildStyleInputs(request.surfaces, selection); result.HasError())
                return Result<UiThemeSnapshot>::Failure(result.ErrorValue());
            auto sources = request.styleSources;
            sources.registry = registry.Generation();
            auto computed = styles.Update(tree, registry, {sources, styleInputs});
            if (computed.HasError())
                return Result<UiThemeSnapshot>::Failure(computed.ErrorValue());
            if (const auto projected = Project(computed.Value(), request.surfaces); projected.HasError())
                return Result<UiThemeSnapshot>::Failure(projected.ErrorValue());
            auto arranged = ArrangeSurfaces(tree, request);
            if (arranged.HasError())
                return Result<UiThemeSnapshot>::Failure(arranged.ErrorValue());
            if (current && current->theme == selection && current->fallbackTokens == fallbacks &&
                current->style.Descriptor().publication == computed.Value().Descriptor().publication &&
                current->layout.Descriptor().interaction == arranged.Value().Descriptor().interaction &&
                std::ranges::equal(paints, current->render.Commands(), [](const UiDrawCommand &left, const UiDrawCommand &right) {
                return left.element == right.element && left.rect == right.rect && left.opacity == right.opacity &&
                       std::get<UiSolidDraw>(left.payload).color == std::get<UiSolidDraw>(right.payload).color;
            }))
                return Result<UiThemeSnapshot>::Success(*current);
            const std::array transforms{UiLogicalTransform{}};
            const UiRenderSnapshotDescriptor renderDescriptor{tree.Instance(),         tree.Canvas(),
                                                              tree.SourceDocument(),   tree.SourceDocumentRevision(),
                                                              tree.Revision(),         arranged.Value().Descriptor().interaction,
                                                              revisions.render,        descriptor.render.view,
                                                              descriptor.render.limits};
            // Reserve the next revision before extraction so exhaustion never follows a successful publication.
            const auto nextRender = revisions.render.Next();
            if (nextRender.HasError())
                return Result<UiThemeSnapshot>::Failure(nextRender.ErrorValue());
            auto extracted = renders.Extract(tree, renderDescriptor, {.commands = paints, .transforms = transforms});
            if (extracted.HasError())
                return Result<UiThemeSnapshot>::Failure(extracted.ErrorValue());
            revisions.render = nextRender.Value();
            return Result<UiThemeSnapshot>::Success(
                {selection, fallbacks, std::move(computed).Value(), std::move(arranged).Value(), std::move(extracted).Value()});
        }
    };

    /** @copydoc UiThemeRuntime::Create */
    Result<UiThemeRuntime> UiThemeRuntime::Create(const UiThemeRuntimeDescriptor &descriptor, RuntimeStyleRegistry registry,
                                                  const RuntimeStyleAssetId theme) {
        const auto &style = descriptor.style;
        const auto &layout = descriptor.layout;
        if (!style.IsValid() || !layout.IsValid() || !descriptor.render.IsValid() || style.instance != layout.instance ||
            style.canvas != layout.canvas || style.document != layout.document || style.elementCapacity != layout.elementCapacity ||
            descriptor.classReferenceCapacity > MaximumUiStyleClasses || descriptor.render.limits.commands < style.elementCapacity ||
            descriptor.render.limits.transforms < 1 || registry.State() != RuntimeStyleRegistryState::Active || !registry.HasAsset(theme) ||
            registry.Generation() != style.initialRegistryGeneration)
            return StyleInternal::Failure<UiThemeRuntime>(UiErrors::StyleInvalid);
        if (const auto valid = ThemeInternal::ValidateProperties(registry, descriptor.properties); valid.HasError())
            return Result<UiThemeRuntime>::Failure(valid.ErrorValue());
        auto resolver = UiStyleResolver::Create(style);
        if (resolver.HasError())
            return Result<UiThemeRuntime>::Failure(resolver.ErrorValue());
        auto engine = UiLayoutEngine::Create(layout);
        if (engine.HasError())
            return Result<UiThemeRuntime>::Failure(engine.ErrorValue());
        auto extractor = UiRenderExtractor::Create(descriptor.render);
        if (extractor.HasError())
            return Result<UiThemeRuntime>::Failure(extractor.ErrorValue());
        try {
            return Result<UiThemeRuntime>::Success(
                UiThemeRuntime{std::make_unique<Storage>(descriptor, std::move(registry), theme, std::move(resolver).Value(),
                                                         std::move(engine).Value(), std::move(extractor).Value())});
        } catch (const std::bad_alloc &) {
            return StyleInternal::Failure<UiThemeRuntime>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiThemeRuntime::UiThemeRuntime */
    UiThemeRuntime::UiThemeRuntime(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiThemeRuntime::~UiThemeRuntime */
    UiThemeRuntime::~UiThemeRuntime() {
        Shutdown();
    }

    /** @copydoc UiThemeRuntime::UiThemeRuntime */
    UiThemeRuntime::UiThemeRuntime(UiThemeRuntime &&) noexcept = default;
    /** @copydoc UiThemeRuntime::operator= */
    UiThemeRuntime &UiThemeRuntime::operator=(UiThemeRuntime &&) noexcept = default;

    /** @copydoc UiThemeRuntime::BeginChange */
    Result<UiThemeChangeId> UiThemeRuntime::BeginChange() {
        if (!storage_ || storage_->state != UiThemeRuntimeState::Active)
            return StyleInternal::Failure<UiThemeChangeId>(UiErrors::StyleLifecycleUnavailable);
        const auto next = storage_->issued.Next();
        if (next.HasError())
            return Result<UiThemeChangeId>::Failure(next.ErrorValue());
        storage_->issued = next.Value();
        storage_->prepared.reset();
        storage_->pending = UiThemeChangeId{storage_->descriptor.style.canvas, next.Value()};
        return Result<UiThemeChangeId>::Success(*storage_->pending);
    }

    /** @copydoc UiThemeRuntime::Prepare */
    Result<void> UiThemeRuntime::Prepare(const UiThemeChangeId change, UiStyleRegistryDefinition definition,
                                         const RuntimeStyleAssetId theme, const std::span<const UiThemeTokenFallback> fallbacks) {
        if (!storage_)
            return StyleInternal::Failure(UiErrors::StyleLifecycleUnavailable);
        if (const auto valid = storage_->CheckChange(change); valid.HasError())
            return valid;
        storage_->prepared.reset();
        const auto next = storage_->issued.Next();
        if (next.HasError())
            return Result<void>::Failure(next.ErrorValue());
        storage_->issued = next.Value();
        try {
            if (const auto result = ThemeInternal::RestoreTokens(definition, storage_->active, fallbacks); result.HasError())
                return result;
            auto registry = RuntimeStyleRegistry::Create(std::move(definition), storage_->issued);
            if (registry.HasError())
                return Result<void>::Failure(registry.ErrorValue());
            if (!registry.Value().HasAsset(theme))
                return StyleInternal::Failure(UiErrors::StyleReferenceInvalid);
            if (const auto valid = ThemeInternal::ValidateProperties(registry.Value(), storage_->descriptor.properties); valid.HasError())
                return valid;
            storage_->prepared = std::move(registry).Value();
            storage_->preparedTheme = theme;
            storage_->preparedFallbacks = static_cast<std::uint32_t>(fallbacks.size());
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return StyleInternal::Failure(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiThemeRuntime::Cancel */
    Result<void> UiThemeRuntime::Cancel(const UiThemeChangeId change) {
        if (!storage_)
            return StyleInternal::Failure(UiErrors::StyleLifecycleUnavailable);
        if (const auto valid = storage_->CheckChange(change); valid.HasError())
            return valid;
        storage_->prepared.reset();
        storage_->pending.reset();
        return Result<void>::Success();
    }

    /** @copydoc UiThemeRuntime::Update */
    Result<UiThemeSnapshot> UiThemeRuntime::Update(const UiElementTree &tree, const UiThemeUpdateRequest &request) {
        if (!storage_ || storage_->state != UiThemeRuntimeState::Active)
            return StyleInternal::Failure<UiThemeSnapshot>(UiErrors::StyleLifecycleUnavailable);
        if (request.change) {
            if (const auto valid = storage_->CheckChange(*request.change); valid.HasError())
                return Result<UiThemeSnapshot>::Failure(valid.ErrorValue());
            if (!storage_->prepared)
                return StyleInternal::Failure<UiThemeSnapshot>(UiErrors::StyleLifecycleUnavailable);
            // Establish the initial resolver lineage before replacing the initial registry.
            if (!storage_->current)
                return StyleInternal::Failure<UiThemeSnapshot>(UiErrors::StyleSourceStale);
        }
        const auto &registry = request.change ? *storage_->prepared : storage_->active;
        const auto theme = request.change ? storage_->preparedTheme : storage_->theme;
        const auto fallbacks = request.change ? storage_->preparedFallbacks : storage_->activeFallbacks;
        auto snapshot = storage_->Build(tree, request, registry, theme, fallbacks);
        if (snapshot.HasError())
            return snapshot;
        if (request.change) {
            storage_->active = std::move(*storage_->prepared);
            storage_->theme = theme;
            storage_->activeFallbacks = fallbacks;
            storage_->prepared.reset();
            storage_->pending.reset();
        }
        storage_->current = snapshot.Value();
        return snapshot;
    }

    /** @copydoc UiThemeRuntime::Current */
    const UiThemeSnapshot *UiThemeRuntime::Current() const noexcept {
        return storage_ && storage_->current ? &*storage_->current : nullptr;
    }

    /** @copydoc UiThemeRuntime::BeginRetirement */
    Result<void> UiThemeRuntime::BeginRetirement() {
        if (!storage_ || storage_->state != UiThemeRuntimeState::Active)
            return StyleInternal::Failure(UiErrors::StyleLifecycleUnavailable);
        storage_->state = UiThemeRuntimeState::Retiring;
        storage_->pending.reset();
        storage_->prepared.reset();
        return Result<void>::Success();
    }

    /** @copydoc UiThemeRuntime::Shutdown */
    void UiThemeRuntime::Shutdown() noexcept {
        if (!storage_ || storage_->state == UiThemeRuntimeState::Stopped)
            return;
        storage_->state = UiThemeRuntimeState::Stopped;
        storage_->pending.reset();
        storage_->prepared.reset();
        storage_->current.reset();
        storage_->styles.Shutdown();
        storage_->layouts.Shutdown();
        storage_->renders.Close();
        storage_->active.Shutdown();
        storage_->styleInputs.clear();
        storage_->classes.clear();
        storage_->layoutInputs.clear();
        storage_->previousLayoutInputs.clear();
        storage_->paints.clear();
    }

    /** @copydoc UiThemeRuntime::State */
    UiThemeRuntimeState UiThemeRuntime::State() const noexcept {
        return storage_ ? storage_->state : UiThemeRuntimeState::Stopped;
    }

    /** @copydoc UiThemeRuntime::IsDrained */
    bool UiThemeRuntime::IsDrained() const noexcept {
        return !storage_ || (storage_->state == UiThemeRuntimeState::Stopped && storage_->styles.IsDrained() &&
                             storage_->layouts.IsDrained() && storage_->renders.IsDrained());
    }
}  // namespace Horo::Runtime::Ui
