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
                texts.emplace_back(attachment.element, attachment.property, std::move(resolved).Value());
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

        /** @brief Rejects order-dependent duplicate binding resolutions before constructing their real owner. */
        bool UniqueBindings(std::span<const UiTemplateBindingResolution> bindings) {
            for (std::size_t index = 0; index < bindings.size(); ++index)
                if (std::ranges::find(bindings.first(index), bindings[index].binding, &UiTemplateBindingResolution::binding) !=
                    bindings.first(index).end())
                    return false;
            return true;
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
                bindings.emplace_back(resolution->provider, binding, resolution->initialTarget);
            }
            if (bindings.size() != input.bindings.size())
                return Result<void>::Failure(MakeError(UiErrors::BindingDescriptorInvalid));
            if (!UniqueBindings(input.bindings))
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

        /** @brief Checks the complete actual tree/style lineage of a detached layout result. */
        bool MatchesLayout(const UiLayoutSnapshot &layout, const UiElementTree &tree, const UiComputedStyleSnapshot &styles) {
            const auto &lineage = layout.Descriptor();
            return lineage.instance == tree.Instance() && lineage.canvas == tree.Canvas() && lineage.document == tree.SourceDocument() &&
                   lineage.sources.document == tree.SourceDocumentRevision() && lineage.sources.tree == tree.Revision() &&
                   lineage.sources.style.Value() == styles.Descriptor().geometry.Value() && layout.Records().size() == tree.Size();
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
            if (!MatchesLayout(layout.Value(), canvas.tree, styles))
                return Result<void>::Failure(MakeError(UiErrors::RevisionStale));
            if (const auto checked = CheckCancellation(cancellation); checked.HasError())
                return checked;
            canvas.layout.emplace(std::move(layout).Value());
            return Result<void>::Success();
        }

        /** @brief Owns the outputs of one synchronous canvas composition; no adapter borrow survives. */
        struct ComposedCanvas {
            UiTemplateCanvasProjection projection;
            Detail::UiTemplateSemanticSource source;
        };

        /** @brief Composes the actual detached owners in domain order, retaining original failure information. */
        Result<ComposedCanvas> ComposeCanvas(const UiTemplates::UiTemplateCookedDocument &document, UiReloadCanvas &canvas,
                                             const UiTemplateCanvasComposition &input,
                                             const UiTemplateLocalizationSnapshotQuery *localization, const Sha256Digest &snapshot,
                                             const UiLocaleTag &locale, const CancellationToken &cancellation) {
            using Output = Result<ComposedCanvas>;
            if (!input.semanticOwner.IsValid())
                return Output::Failure(MakeError(UiErrors::CapacityExceeded));
            if (const auto bound = ComposeBindings(document, canvas, input); bound.HasError())
                return Output::Failure(bound.ErrorValue());
            auto styles = ComposeStyles(document, canvas.tree, input);
            if (styles.HasError())
                return Output::Failure(styles.ErrorValue());
            auto texts =
                ResolveTexts(document, canvas.tree, localization, input.semanticOwner.limits.textBytes, snapshot, locale, cancellation);
            if (texts.HasError())
                return Output::Failure(texts.ErrorValue());
            if (const auto laidOut = ComposeLayout(canvas, input, styles.Value(), texts.Value(), cancellation); laidOut.HasError())
                return Output::Failure(laidOut.ErrorValue());
            auto source = Detail::CaptureTemplateSemantics(canvas, input, texts.Value());
            if (source.HasError())
                return Output::Failure(source.ErrorValue());
            auto semantics = Detail::ExtractTemplateSemantics(canvas, source.Value(), UiAccessibilitySemanticRevision::Create(1).Value());
            if (semantics.HasError())
                return Output::Failure(semantics.ErrorValue());
            return Output::Success({{input.canvas, std::move(styles).Value(), canvas.layout->Descriptor(), std::move(semantics).Value(),
                                     std::move(texts).Value()},
                                    std::move(source).Value()});
        }

        /** @brief Admits each canvas exactly once and observes cancellation before calling external adapters. */
        Result<UiReloadCanvas *> AdmitCanvas(UiReloadGeneration &runtime, const UiTemplateCanvasComposition &input,
                                             std::span<const UiTemplateCanvasProjection> projections,
                                             const CancellationToken &cancellation) {
            using Output = Result<UiReloadCanvas *>;
            if (const auto checked = CheckCancellation(cancellation); checked.HasError())
                return Output::Failure(checked.ErrorValue());
            auto *canvas = runtime.Canvas(input.canvas);
            if (!canvas || std::ranges::find(projections, input.canvas, &UiTemplateCanvasProjection::canvas) != projections.end())
                return Output::Failure(MakeError(UiErrors::CanvasReferenceInvalid));
            return Output::Success(canvas);
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

        /** @brief Pins one host-owned immutable message identity/locale for the complete multi-canvas operation. */
        struct PinnedMessages {
            Sha256Digest snapshot;
            UiLocaleTag locale;
        };

        PinnedMessages PinMessages(const UiTemplateLocalizationSnapshotQuery *query) {
            if (!query)
                return {};
            return {query->Identity(), query->Locale()};
        }

        /** @brief Rejects a query that changed authority after any successful resolution. */
        bool StillPinned(const UiTemplateLocalizationSnapshotQuery *query, const PinnedMessages &pin) {
            return !query || (query->Identity() == pin.snapshot && query->Locale() == pin.locale);
        }

        /** @brief Admits a complete detached composition before touching any private canvas owner. */
        bool ValidComposition(const UiReloadGeneration &runtime, std::span<const UiTemplateCanvasComposition> canvases,
                              const UiTemplates::UiTemplateCookedDocument &document,
                              std::span<const UiTemplateCanvasProjection> projections) {
            return projections.empty() && runtime.Instance().State() == UiRuntimeInstanceState::Prepared &&
                   canvases.size() == runtime.Canvases().size() && CoversRichInput(document, runtime);
        }
    }  // namespace

    /** @copydoc UiTemplateHost::Compose */
    Result<UiTemplateHostCook> UiTemplateHost::Compose(UiTemplateHostCook cook, Runtime::Ui::UiReloadGeneration &runtime,
                                                       std::span<const UiTemplateCanvasComposition> canvases,
                                                       const UiTemplateLocalizationSnapshotQuery *localization,
                                                       const CancellationToken &cancellation) {
        using Output = Result<UiTemplateHostCook>;
        try {
            if (!cook.state_)
                return Output::Failure(MakeError(UiErrors::RenderCompositionInvalid));
            if (!ValidComposition(runtime, canvases, cook.Document(), cook.state_->projections))
                return Output::Failure(MakeError(UiErrors::RenderCompositionInvalid));
            auto state = std::make_shared<UiTemplateHostCook::State>(*cook.state_);
            const auto pin = PinMessages(localization);
            for (const auto &input : canvases) {
                auto canvas = AdmitCanvas(runtime, input, state->projections, cancellation);
                if (canvas.HasError())
                    return Output::Failure(canvas.ErrorValue());
                auto composed =
                    ComposeCanvas(cook.Document(), *canvas.Value(), input, localization, pin.snapshot, pin.locale, cancellation);
                if (composed.HasError())
                    return Output::Failure(composed.ErrorValue());
                auto output = std::move(composed).Value();
                state->projections.push_back(std::move(output.projection));
                state->semanticSources.push_back(std::move(output.source));
            }
            if (const auto checked = CheckCancellation(cancellation); checked.HasError())
                return Output::Failure(checked.ErrorValue());
            if (!StillPinned(localization, pin))
                return Output::Failure(MakeError(UiErrors::LocalizedMessageInvalid));
            return Output::Success(UiTemplateHostCook{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphBudgetExceeded));
        }
    }
}  // namespace Horo::Application
