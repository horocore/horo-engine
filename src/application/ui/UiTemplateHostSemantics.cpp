#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateHostState.h"

#include <algorithm>

namespace Horo::Application {
    namespace Detail {
        namespace {
            using namespace Runtime::Ui;

            /** @brief Consumes finite declared capacities before copying any host-owned semantic payload. */
            bool Consume(const std::size_t size, std::size_t &remaining) noexcept {
                if (size > remaining)
                    return false;
                remaining -= size;
                return true;
            }

            /** @brief Admits one complete declaration against the remaining whole-canvas arenas. */
            bool AdmitNode(const UiAccessibilityNodeInput &node, std::size_t &bytes, std::size_t &relations,
                           std::size_t &actions) noexcept {
                if (!Consume(node.name.text.size(), bytes) || !Consume(node.description.text.size(), bytes) ||
                    !Consume(node.value.text.text.size(), bytes) || !Consume(node.error.message.text.size(), bytes) ||
                    !Consume(node.relations.size(), relations) || !Consume(node.actions.size(), actions))
                    return false;
                for (const auto &action : node.actions)
                    if (!Consume(action.name.text.size(), bytes))
                        return false;
                return true;
            }

            /** @brief Bounds all text/relations/actions, including filtered nodes, before owned storage is allocated. */
            bool AdmitInputs(const UiTemplateCanvasComposition &input, std::size_t &bytes) noexcept {
                if (!input.semanticOwner.IsValid() || input.semantics.size() > input.semanticOwner.limits.nodes ||
                    input.accessibleText.size() > 3U * input.semanticOwner.limits.nodes)
                    return false;
                bytes = input.semanticOwner.limits.textBytes;
                std::size_t relations = input.semanticOwner.limits.relations;
                std::size_t actions = input.semanticOwner.limits.actions;
                for (const auto &node : input.semantics)
                    if (!AdmitNode(node, bytes, relations, actions))
                        return false;
                return true;
            }

            /** @brief Clears every borrowed field after copying its value into independent load-time storage. */
            UiTemplateSemanticNode OwnNode(const UiAccessibilityNodeInput &input) {
                UiTemplateSemanticNode node{input,
                                            std::string{input.name.text},
                                            std::string{input.description.text},
                                            std::string{input.value.text.text},
                                            std::string{input.error.message.text},
                                            {input.relations.begin(), input.relations.end()},
                                            {}};
                for (const auto &action : input.actions) {
                    node.actions.emplace_back(action, std::string{action.name.text});
                    node.actions.back().descriptor.name.text = {};
                }
                node.descriptor.name.text = {};
                node.descriptor.description.text = {};
                node.descriptor.value.text.text = {};
                node.descriptor.error.message.text = {};
                node.descriptor.relations = {};
                node.descriptor.actions = {};
                return node;
            }

            /** @brief Assigns one explicit localized field without inventing label policy or fallback copy. */
            Result<void> SetText(UiTemplateSemanticNode &node, UiTemplateAccessibleTextField field, const UiTemplateResolvedText &text,
                                 std::size_t &remaining) {
                using enum UiTemplateAccessibleTextField;
                std::string *destination{};
                UiAccessibilityTextInput *descriptor{};
                switch (field) {
                    case Name:
                        destination = &node.name;
                        descriptor = &node.descriptor.name;
                        break;
                    case Description:
                        destination = &node.description;
                        descriptor = &node.descriptor.description;
                        break;
                    case Value:
                        destination = &node.value;
                        descriptor = &node.descriptor.value.text;
                        break;
                    default:
                        return Result<void>::Failure(MakeError(UiErrors::AccessibilityTextInvalid));
                }
                remaining += destination->size();
                if (!Consume(text.message.text.size(), remaining))
                    return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
                *destination = text.message.text;
                descriptor->source = UiAccessibilityTextSource::ResolvedMessage;
                if (field == Value)
                    node.descriptor.value.kind = UiAccessibilityValueKind::Text;
                return Result<void>::Success();
            }

            /** @brief Applies explicit unique semantic-field mappings under the single captured text budget. */
            Result<void> ApplyAccessibleTexts(UiTemplateSemanticSource &source, std::span<const UiTemplateAccessibleTextTarget> targets,
                                              std::span<const UiTemplateResolvedText> texts, std::size_t &remaining) {
                for (std::size_t index = 0; index < targets.size(); ++index) {
                    const auto &target = targets[index];
                    // Two mappings for the same field would create order-dependent competing authority.
                    if (std::ranges::any_of(targets.first(index), [&](const auto &prior) {
                        return prior.element == target.element && prior.field == target.field;
                    }))
                        return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::AccessibilityTextInvalid));
                    const auto text = std::ranges::find_if(texts, [&](const auto &value) {
                        return value.element == target.element && value.property == target.property;
                    });
                    const auto node = std::ranges::find_if(source.nodes, [&](const auto &value) {
                        return value.descriptor.element == target.element;
                    });
                    if (text == texts.end() || node == source.nodes.end())
                        return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::AccessibilityTextInvalid));
                    if (const auto applied = SetText(*node, target.field, *text, remaining); applied.HasError())
                        return Result<void>::Failure(applied.ErrorValue());
                }
                return Result<void>::Success();
            }

            /** @brief Rebinds owned strings and slices only during synchronous domain extraction. */
            UiAccessibilityNodeInput BorrowNode(const UiTemplateSemanticNode &node,
                                                const std::span<const UiAccessibilityActionInput> actions) {
                auto input = node.descriptor;
                input.name.text = node.name;
                input.description.text = node.description;
                input.value.text.text = node.value;
                input.error.message.text = node.error;
                input.relations = node.relations;
                input.actions = actions;
                return input;
            }
        }  // namespace

        /** @copydoc CaptureTemplateSemantics */
        Result<UiTemplateSemanticSource> CaptureTemplateSemantics(const Runtime::Ui::UiReloadCanvas &canvas,
                                                                  const UiTemplateCanvasComposition &input,
                                                                  std::span<const UiTemplateResolvedText> texts) {
            using Output = Result<UiTemplateSemanticSource>;
            std::size_t remaining{};
            if (!AdmitInputs(input, remaining))
                return Output::Failure(MakeError(Runtime::Ui::UiErrors::CapacityExceeded));
            UiTemplateSemanticSource source{canvas.id, input.semanticOwner, {}};
            for (const auto &inputNode : input.semantics) {
                auto element = canvas.tree.Find(inputNode.element);
                if (element.HasError())
                    return Output::Failure(element.ErrorValue());
                auto layout = canvas.layout->Get(element.Value());
                if (layout.HasError())
                    return Output::Failure(layout.ErrorValue());
                auto node = OwnNode(inputNode);
                node.descriptor.bounds = layout.Value().arrangement.borderBox;
                source.nodes.push_back(std::move(node));
            }
            if (const auto mapped = ApplyAccessibleTexts(source, input.accessibleText, texts, remaining); mapped.HasError())
                return Output::Failure(mapped.ErrorValue());
            return Output::Success(std::move(source));
        }

        /** @copydoc ExtractTemplateSemantics */
        Result<Runtime::Ui::UiAccessibilitySnapshot> ExtractTemplateSemantics(const Runtime::Ui::UiReloadCanvas &canvas,
                                                                              const UiTemplateSemanticSource &source,
                                                                              Runtime::Ui::UiAccessibilitySemanticRevision revision) {
            using namespace Runtime::Ui;
            using Output = Result<UiAccessibilitySnapshot>;
            std::vector<UiAccessibilityNodeInput> nodes;
            std::vector<std::vector<UiAccessibilityActionInput>> actions(source.nodes.size());
            for (std::size_t index = 0; index < source.nodes.size(); ++index) {
                const auto &node = source.nodes[index];
                for (const auto &action : node.actions) {
                    auto input = action.descriptor;
                    input.name.text = action.name;
                    actions[index].push_back(input);
                }
                nodes.push_back(BorrowNode(node, actions[index]));
            }
            auto created = UiAccessibilityExtractor::Create(source.owner);
            if (created.HasError())
                return Output::Failure(created.ErrorValue());
            auto extractor = std::move(created).Value();
            const UiAccessibilitySnapshotDescriptor descriptor{CurrentUiAccessibilitySchemaVersion,
                                                               canvas.tree.Instance(),
                                                               canvas.tree.Canvas(),
                                                               canvas.tree.SourceDocument(),
                                                               canvas.tree.SourceDocumentRevision(),
                                                               canvas.tree.Revision(),
                                                               canvas.layout->Descriptor().interaction,
                                                               revision,
                                                               source.owner.limits};
            if (canvas.focus)
                return extractor.Extract(canvas.tree, descriptor, {nodes, {}}, *canvas.focus);
            return extractor.Extract(canvas.tree, descriptor, {nodes, {}});
        }
    }  // namespace Detail

    /** @copydoc UiTemplateHost::RefreshSemantics */
    Result<UiTemplateHostCook> UiTemplateHost::RefreshSemantics(UiTemplateHostCook cook, const Runtime::Ui::UiReloadGeneration &candidate) {
        using Output = Result<UiTemplateHostCook>;
        if (cook.state_->projections.empty())
            return Output::Success(std::move(cook));
        try {
            auto state = std::make_shared<UiTemplateHostCook::State>(*cook.state_);
            for (std::size_t index = 0; index < state->projections.size(); ++index) {
                auto &projection = state->projections[index];
                const auto canvas = std::ranges::find(candidate.Canvases(), projection.canvas, &Runtime::Ui::UiReloadCanvas::id);
                if (canvas == candidate.Canvases().end() || index >= state->semanticSources.size())
                    return Output::Failure(MakeError(Runtime::Ui::UiErrors::RevisionStale));
                auto revision = projection.semantics.Descriptor().semanticRevision.Next();
                if (revision.HasError())
                    return Output::Failure(revision.ErrorValue());
                auto semantics = Detail::ExtractTemplateSemantics(*canvas, state->semanticSources[index], revision.Value());
                if (semantics.HasError())
                    return Output::Failure(semantics.ErrorValue());
                projection.semantics = std::move(semantics).Value();
            }
            return Output::Success(UiTemplateHostCook{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphBudgetExceeded));
        }
    }
}  // namespace Horo::Application
