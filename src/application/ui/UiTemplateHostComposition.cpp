#include "Horo/Foundation/Utf8.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateHostState.h"

#include <algorithm>

namespace Horo::Application {
    namespace {
        using namespace Runtime::Ui;

        /** @brief Checks cancellation at load-time adapter boundaries without changing any active owner. */
        Result<void> CheckCancellation(const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(UiErrors::AssetLoadCancelled));
            return Result<void>::Success();
        }

        /** @brief Rejects malformed Unicode spans, including mid-scalar byte boundaries and overlapping ranges. */
        bool ValidSpans(const UiTemplateResolvedMessage &message) {
            std::size_t end{};
            for (const auto &span : message.spans) {
                if (span.length == 0 || span.offset < end || span.offset > message.text.size() ||
                    span.length > message.text.size() - span.offset || span.kind > UiTemplateMessageSpanKind::Code)
                    return false;
                end = static_cast<std::size_t>(span.offset) + span.length;
                if (!IsValidUtf8ScalarSequence(std::string_view{message.text}.substr(span.offset, span.length)))
                    return false;
            }
            return true;
        }

        /** @brief Validates exact pinned Localization ownership separately from Unicode representation. */
        bool SameMessage(const UiTemplateLocalizationSnapshotQuery &query, const UiTemplateResolvedMessage &message,
                         const LocalizedMessageRef &reference, const Sha256Digest &snapshot, const UiLocaleTag &locale) {
            return query.Identity() == snapshot && query.Locale() == locale && message.snapshot == snapshot &&
                   message.key == reference.Key() && message.locale == locale;
        }

        /** @brief Preserves domain failures and checks cancellation on both sides of one external resolution call. */
        Result<UiTemplateResolvedMessage> ResolveMessage(const UiTemplateLocalizationSnapshotQuery &query,
                                                         const LocalizedMessageRef &reference, const Sha256Digest &snapshot,
                                                         const UiLocaleTag &locale, const std::size_t budget,
                                                         const CancellationToken &cancellation) {
            using Output = Result<UiTemplateResolvedMessage>;
            if (const auto checked = CheckCancellation(cancellation); checked.HasError())
                return Output::Failure(checked.ErrorValue());
            auto result = query.Resolve(reference, budget, cancellation);
            if (result.HasError())
                return result;
            if (const auto checked = CheckCancellation(cancellation); checked.HasError())
                return Output::Failure(checked.ErrorValue());
            const auto &message = result.Value();
            if (message.text.size() > budget)
                return Output::Failure(MakeError(UiErrors::CapacityExceeded));
            if (!SameMessage(query, message, reference, snapshot, locale) || message.direction >= UiTextParagraphDirection::Count ||
                !IsValidUtf8ScalarSequence(message.text) || !ValidSpans(message))
                return Output::Failure(MakeError(UiErrors::LocalizedMessageInvalid));
            return result;
        }

        /** @brief Copies fixed-snapshot Unicode for only this actual canvas, under its explicit owned text budget. */
        Result<std::vector<UiTemplateResolvedText>> ResolveTexts(const UiTemplates::UiTemplateCookedDocument &document,
                                                                 const UiElementTree &tree,
                                                                 const UiTemplateLocalizationSnapshotQuery *query, const std::size_t budget,
                                                                 const Sha256Digest &snapshot, const UiLocaleTag &locale,
                                                                 const CancellationToken &cancellation) {
            using Output = Result<std::vector<UiTemplateResolvedText>>;
            std::vector<UiTemplateResolvedText> texts;
            std::size_t bytes{};
            for (const auto &attachment : document.localizedTexts) {
                if (tree.Find(attachment.element).HasError())
                    continue;
                if (!query || snapshot == Sha256Digest{} || !locale.IsValid())
                    return Output::Failure(MakeError(UiErrors::LocalizedMessageInvalid));
                auto resolved = ResolveMessage(*query, attachment.text, snapshot, locale, budget - bytes, cancellation);
                if (resolved.HasError())
                    return Output::Failure(resolved.ErrorValue());
                bytes += resolved.Value().text.size();
                texts.push_back({attachment.element, attachment.property, std::move(resolved).Value()});
            }
            return Output::Success(std::move(texts));
        }

        /** @brief Resolves cooked token attachments through the actual registry before supplying literal inline inputs. */
        Result<void> AppendStyles(const UiTemplates::UiTemplateCookedDocument &document, UiElementId element,
                                  const RuntimeStyleRegistry &registry, std::vector<UiStyleAssignment> &properties, std::size_t capacity) {
            for (const auto &authored : document.styles) {
                if (authored.element != element)
                    continue;
                if (std::ranges::find(properties, authored.assignment.property, &UiStyleAssignment::property) != properties.end())
                    return Result<void>::Failure(MakeError(UiErrors::StyleInvalid));
                if (properties.size() >= capacity)
                    return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
                auto assignment = authored.assignment;
                if (assignment.value.referencesToken) {
                    auto value = registry.ResolveToken(assignment.value.token);
                    if (value.HasError())
                        return Result<void>::Failure(value.ErrorValue());
                    assignment.value = UiStyleValueSource::Literal(std::move(value).Value());
                }
                properties.push_back(std::move(assignment));
            }
            return Result<void>::Success();
        }

        /** @brief Uses exact cooked descriptors and explicit provider incarnations, never caller-substituted binding schemas. */
        Result<void> ComposeBindings(const UiTemplates::UiTemplateCookedDocument &document, UiReloadCanvas &canvas,
                                     const UiTemplateCanvasComposition &input) {
            if (canvas.bindings)
                return Result<void>::Failure(MakeError(UiErrors::BindingDescriptorInvalid));
            std::vector<UiResolvedBindingDescriptor> bindings;
            for (const auto &binding : document.bindings) {
                if (canvas.tree.Find(binding.target.element).HasError())
                    continue;
                const auto resolution = std::ranges::find(input.bindings, binding.id, &UiTemplateBindingResolution::binding);
                if (resolution == input.bindings.end())
                    return Result<void>::Failure(MakeError(UiErrors::BindingDescriptorInvalid));
                bindings.push_back({resolution->provider, binding, resolution->initialTarget});
            }
            if (bindings.size() != input.bindings.size())
                return Result<void>::Failure(MakeError(UiErrors::BindingDescriptorInvalid));
            for (std::size_t index = 0; index < input.bindings.size(); ++index)
                if (std::ranges::find(input.bindings.first(index), input.bindings[index].binding, &UiTemplateBindingResolution::binding) !=
                    input.bindings.first(index).end())
                    return Result<void>::Failure(MakeError(UiErrors::BindingDescriptorInvalid));
            if (bindings.empty() && input.providers.empty())
                return Result<void>::Success();
            auto store = UiBindingStore::Create(canvas.tree, input.providers, bindings);
            if (store.HasError())
                return Result<void>::Failure(store.ErrorValue());
            canvas.bindings.emplace(std::move(store).Value());
            return Result<void>::Success();
        }

        /** @brief Combines authored inline values with explicit host class/policy inputs before the real style owner resolves. */
        Result<UiComputedStyleSnapshot> ComposeStyles(const UiTemplates::UiTemplateCookedDocument &document, const UiElementTree &tree,
                                                      const UiTemplateCanvasComposition &input) {
            using Output = Result<UiComputedStyleSnapshot>;
            if (!input.registry || !input.styleOwner.IsValid() || input.styles.size() != tree.Size())
                return Output::Failure(MakeError(UiErrors::StyleInvalid));
            std::vector<UiStyleElementInput> elements{input.styles.begin(), input.styles.end()};
            std::vector<std::vector<UiStyleAssignment>> assignments(elements.size());
            for (std::size_t index = 0; index < elements.size(); ++index) {
                const auto record = tree.Get(elements[index].element);
                if (record.HasError())
                    return Output::Failure(record.ErrorValue());
                auto &properties = assignments[index];
                if (elements[index].inlineProperties.size() > input.styleOwner.propertyCapacity)
                    return Output::Failure(MakeError(UiErrors::CapacityExceeded));
                properties.assign(elements[index].inlineProperties.begin(), elements[index].inlineProperties.end());
                if (const auto appended =
                        AppendStyles(document, record.Value().id, *input.registry, properties, input.styleOwner.propertyCapacity);
                    appended.HasError())
                    return Output::Failure(appended.ErrorValue());
                elements[index].inlineProperties = properties;
            }
            auto resolver = UiStyleResolver::Create(input.styleOwner);
            if (resolver.HasError())
                return Output::Failure(resolver.ErrorValue());
            auto owner = std::move(resolver).Value();
            return owner.Update(tree, *input.registry, {input.styleSources, elements});
        }

        /** @brief Admits only exact owner/tree/style layout lineage from the application adapter. */
        Result<void> ComposeLayout(UiReloadCanvas &canvas, const UiTemplateCanvasComposition &input, const UiComputedStyleSnapshot &styles,
                                   std::span<const UiTemplateResolvedText> texts, const CancellationToken &cancellation) {
            if (!input.layout || canvas.layoutEngine || canvas.layout)
                return Result<void>::Failure(MakeError(UiErrors::LayoutInvalid));
            auto created = UiLayoutEngine::Create(input.layoutOwner);
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            canvas.layoutEngine.emplace(std::move(created).Value());
            auto layout = input.layout->Prepare(canvas.tree, styles, texts, canvas.bindings ? &*canvas.bindings : nullptr,
                                                *canvas.layoutEngine, cancellation);
            if (layout.HasError())
                return Result<void>::Failure(layout.ErrorValue());
            const auto &lineage = layout.Value().Descriptor();
            if (lineage.instance != canvas.tree.Instance() || lineage.canvas != canvas.tree.Canvas() ||
                lineage.document != canvas.tree.SourceDocument() || lineage.sources.document != canvas.tree.SourceDocumentRevision() ||
                lineage.sources.tree != canvas.tree.Revision() || lineage.sources.style.Value() != styles.Descriptor().geometry.Value() ||
                layout.Value().Records().size() != canvas.tree.Size())
                return Result<void>::Failure(MakeError(UiErrors::RevisionStale));
            if (const auto checked = CheckCancellation(cancellation); checked.HasError())
                return checked;
            canvas.layout.emplace(std::move(layout).Value());
            return Result<void>::Success();
        }

        /** @brief Requires each authored rich attachment to belong to exactly one actual canvas tree. */
        bool CoversRichInput(const UiTemplates::UiTemplateCookedDocument &document, const UiReloadGeneration &runtime) {
            const auto covered = [&](const UiElementId element) {
                return std::ranges::count_if(runtime.Canvases(), [&](const auto &canvas) {
                    return canvas.tree.Find(element).HasValue();
                }) == 1;
            };
            return std::ranges::all_of(document.styles, [&](const auto &value) {
                return covered(value.element);
            }) && std::ranges::all_of(document.localizedTexts, [&](const auto &value) {
                return covered(value.element);
            }) && std::ranges::all_of(document.bindings, [&](const auto &value) {
                return covered(value.target.element);
            });
        }
    }  // namespace

    /** @copydoc UiTemplateHost::Compose */
    Result<UiTemplateHostCook> UiTemplateHost::Compose(UiTemplateHostCook cook, Runtime::Ui::UiReloadGeneration &runtime,
                                                       std::span<const UiTemplateCanvasComposition> canvases,
                                                       const UiTemplateLocalizationSnapshotQuery *localization,
                                                       const CancellationToken &cancellation) {
        using Output = Result<UiTemplateHostCook>;
        try {
            if (!cook.state_ || !cook.state_->projections.empty() || runtime.Instance().State() != UiRuntimeInstanceState::Prepared ||
                canvases.size() != runtime.Canvases().size() || !CoversRichInput(cook.Document(), runtime))
                return Output::Failure(MakeError(UiErrors::RenderCompositionInvalid));
            auto state = std::make_shared<UiTemplateHostCook::State>(*cook.state_);
            const auto snapshot = localization ? localization->Identity() : Sha256Digest{};
            const auto locale = localization ? localization->Locale() : UiLocaleTag{};
            for (const auto &input : canvases) {
                if (const auto checked = CheckCancellation(cancellation); checked.HasError())
                    return Output::Failure(checked.ErrorValue());
                auto *canvas = runtime.Canvas(input.canvas);
                if (!canvas ||
                    std::ranges::find(state->projections, input.canvas, &UiTemplateCanvasProjection::canvas) != state->projections.end())
                    return Output::Failure(MakeError(UiErrors::CanvasReferenceInvalid));
                if (!input.semanticOwner.IsValid())
                    return Output::Failure(MakeError(UiErrors::CapacityExceeded));
                if (const auto bound = ComposeBindings(cook.Document(), *canvas, input); bound.HasError())
                    return Output::Failure(bound.ErrorValue());
                auto styles = ComposeStyles(cook.Document(), canvas->tree, input);
                if (styles.HasError())
                    return Output::Failure(styles.ErrorValue());
                auto texts = ResolveTexts(cook.Document(), canvas->tree, localization, input.semanticOwner.limits.textBytes, snapshot,
                                          locale, cancellation);
                if (texts.HasError())
                    return Output::Failure(texts.ErrorValue());
                if (const auto laidOut = ComposeLayout(*canvas, input, styles.Value(), texts.Value(), cancellation); laidOut.HasError())
                    return Output::Failure(laidOut.ErrorValue());
                auto semanticSource = Detail::CaptureTemplateSemantics(*canvas, input, texts.Value());
                if (semanticSource.HasError())
                    return Output::Failure(semanticSource.ErrorValue());
                auto semantics =
                    Detail::ExtractTemplateSemantics(*canvas, semanticSource.Value(), UiAccessibilitySemanticRevision::Create(1).Value());
                if (semantics.HasError())
                    return Output::Failure(semantics.ErrorValue());
                state->projections.push_back({input.canvas, std::move(styles).Value(), canvas->layout->Descriptor(),
                                              std::move(semantics).Value(), std::move(texts).Value()});
                state->semanticSources.push_back(std::move(semanticSource).Value());
            }
            if (const auto checked = CheckCancellation(cancellation); checked.HasError())
                return Output::Failure(checked.ErrorValue());
            if (localization && (localization->Identity() != snapshot || localization->Locale() != locale))
                return Output::Failure(MakeError(UiErrors::LocalizedMessageInvalid));
            return Output::Success(UiTemplateHostCook{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphBudgetExceeded));
        }
    }
}  // namespace Horo::Application
