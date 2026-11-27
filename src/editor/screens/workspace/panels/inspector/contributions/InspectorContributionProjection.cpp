#include "Horo/Extensions/ExtensionErrors.h"
#include "InspectorContributionView.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <type_traits>

namespace Horo::Editor {
    using namespace Extensions;

    namespace {
        /** @brief Resolves one exact selected attachment; ambiguity never chooses an arbitrary first repeated behavior. */
        const Gameplay::BehaviorComponent *Unique(const SceneObject &object, const Gameplay::BehaviorComponent &primary, bool first) {
            const Gameplay::BehaviorComponent *found{};
            for (const auto &behavior : object.components.behaviors) {
                if (behavior.typeId != primary.typeId || behavior.schemaVersion != primary.schemaVersion)
                    continue;
                if (first) {
                    if (behavior.instanceId == primary.instanceId)
                        return &behavior;
                } else {
                    if (found)
                        return nullptr;
                    found = &behavior;
                }
            }
            return found;
        }

        /** @brief Copies exact scalar owner values without coercing integers through floating point. */
        std::optional<EditorInspectorScalar> Scalar(const Gameplay::BehaviorFieldValue &input) {
            return std::visit([]<class T>(const T &value) -> std::optional<EditorInspectorScalar> {
                if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, double> ||
                              std::is_same_v<T, std::string>)
                    return EditorInspectorScalar{value};
                else
                    return std::nullopt;
            }, input);
        }

        /** @brief Formats a finite decimal without changing the owner's round-trip precision. */
        bool FormatDecimal(double number, std::string &buffer) {
            std::array<char, 64> text{};
            const auto result = std::to_chars(text.data(), text.data() + text.size(), number, std::chars_format::general,
                                              std::numeric_limits<double>::max_digits10);
            if (result.ec != std::errc{})
                return false;
            buffer.assign(text.data(), result.ptr);
            return true;
        }

        /** @brief Installs exact numeric alternatives only when the admitted control has the same numeric kind. */
        bool InstallNumber(EditorUiNumberNode &node, const EditorInspectorScalar &value, std::string &buffer) {
            return std::visit([&node, &buffer]<class T>(const T &number) {
                if constexpr (std::is_same_v<T, std::int64_t>) {
                    if (node.numberKind != EditorUiNumberKind::Integer)
                        return false;
                    node.value = number;
                    buffer = std::to_string(number);
                } else if constexpr (std::is_same_v<T, double>) {
                    if (node.numberKind != EditorUiNumberKind::Decimal || !FormatDecimal(number, buffer))
                        return false;
                    node.value = number;
                } else
                    return false;
                buffer.reserve(64);
                return true;
            }, value);
        }

        /** @brief Installs actual values in one admitted primitive, retaining constraints and exact full-width storage. */
        bool Install(EditorUiNode &node, const EditorInspectorScalar &value, std::string &buffer) {
            return std::visit([&value, &buffer]<class T>(T &typed) {
                if constexpr (std::is_same_v<T, EditorUiBooleanNode>) {
                    const auto *boolean = std::get_if<bool>(&value);
                    if (!boolean)
                        return false;
                    typed.value = *boolean;
                } else if constexpr (std::is_same_v<T, EditorUiTextFieldNode>) {
                    const auto *text = std::get_if<std::string>(&value);
                    if (!text)
                        return false;
                    typed.value = *text;
                    buffer = typed.value;
                    buffer.reserve(typed.maximumBytes);
                } else if constexpr (std::is_same_v<T, EditorUiNumberNode>)
                    return InstallNumber(typed, value, buffer);
                else
                    return false;
                return true;
            }, node.payload);
        }
    }  // namespace

    /** @copydoc InspectorContributionView::InspectorContributionView */
    InspectorContributionView::InspectorContributionView(EditorInspectorHost &host, const EditorThemeFrame &theme)
        : host_(host), theme_(theme) {}

    /** @copydoc InspectorContributionView::CaptureValues */
    bool InspectorContributionView::CaptureValues(Entry &entry, const Gameplay::BehaviorDescriptor &descriptor,
                                                  const Gameplay::BehaviorComponent &behavior,
                                                  const EditorWorkspaceViewModel &model) const {
        for (const auto &field : entry.contribution.definition->fields) {
            const auto declared = std::ranges::find(descriptor.fields, field.field, &Gameplay::BehaviorFieldDescriptor::name);
            const auto source = std::ranges::find(behavior.fields, field.field, &Gameplay::BehaviorField::name);
            if (declared == descriptor.fields.end() || source == behavior.fields.end() ||
                source->value.index() != declared->defaultValue.index())
                return false;
            const auto value = Scalar(source->value);
            if (!value || ValidateEditorInspectorValue(field, *value).HasError())
                return false;
            const auto node = std::ranges::find_if(entry.form.nodes, [&](const auto &candidate) {
                return EditorUiNodeBaseOf(candidate).id == field.node;
            });
            if (node == entry.form.nodes.end())
                return false;
            const auto index = static_cast<std::size_t>(node - entry.form.nodes.begin());
            if (entry.targets.size() == 1) {
                if (!Install(*node, *value, entry.buffers[index]))
                    return false;
            } else {
                const auto &first = entry.targets.front();
                const auto firstObject = std::ranges::find(model.objects, first.object, &SceneObject::id);
                const auto firstBehavior =
                    std::ranges::find(firstObject->components.behaviors, first.attachment, &Gameplay::BehaviorComponent::instanceId);
                const auto firstField = std::ranges::find(firstBehavior->fields, field.field, &Gameplay::BehaviorField::name);
                entry.mixed[index] = entry.mixed[index] || firstField->value != source->value;
            }
        }
        return true;
    }

    /** @copydoc InspectorContributionView::Capture */
    std::optional<InspectorContributionView::Entry> InspectorContributionView::Capture(const EditorInspectorContribution &contribution,
                                                                                       const Gameplay::BehaviorDescriptor &descriptor,
                                                                                       const Gameplay::BehaviorComponent &primary,
                                                                                       const EditorWorkspaceViewModel &model,
                                                                                       std::span<const SceneObjectId> selection) const {
        if (!contribution.context.IsUsable() || selection.empty() || selection.size() > MaximumInspectorBehaviorSelection)
            return std::nullopt;
        Entry entry{.contribution = contribution,
                    .behavior = descriptor.typeId,
                    .schemaVersion = descriptor.schemaVersion,
                    .form = contribution.definition->form,
                    .enabled = true};
        for (const auto id : selection) {
            const auto object = std::ranges::find(model.objects, id, &SceneObject::id);
            if (object == model.objects.end())
                return std::nullopt;
            const auto *behavior = Unique(*object, primary, model.primarySelection == id);
            if (!behavior || Gameplay::ValidateBehaviorComponent(*behavior).HasError())
                return std::nullopt;
            entry.targets.push_back({id, behavior->instanceId});
            entry.enabled = entry.enabled && behavior->enabled;
            if (!CaptureValues(entry, descriptor, *behavior, model))
                return std::nullopt;
        }
        for (const auto &field : contribution.definition->fields)
            entry.covered.push_back(field.field);
        entry.primaryAttachment = primary.instanceId;
        entry.approved = std::ranges::any_of(model.inspectorApprovals, [&](const auto &approval) {
            return approval.provider == contribution.context.Provider() && approval.surface == contribution.context.Surface().id &&
                   approval.behavior == descriptor.typeId && approval.schemaVersion == descriptor.schemaVersion &&
                   approval.documentGeneration == model.inspectorDocumentGeneration &&
                   approval.behaviorRegistryRevision == model.inspectorBehaviorRegistryRevision &&
                   approval.declaration == contribution.definition && approval.fields == entry.covered;
        });
        for (std::size_t index = 0; index < entry.form.nodes.size(); ++index) {
            const auto &base = EditorUiNodeBaseOf(entry.form.nodes[index]);
            if (base.custom && base.custom->widget == EditorUiCustomWidget::ScalarDial)
                entry.scalars.emplace_back(index, EditorUiScalarState{});
        }
        return entry;
    }

    /** @copydoc InspectorContributionView::Synchronize */
    void InspectorContributionView::Synchronize(const EditorWorkspaceViewModel &model, std::span<const SceneObjectId> selection) {
        const auto registry = host_.Revision();
        if (documentRevision_ == model.documentRevision.value && documentGeneration_ == model.inspectorDocumentGeneration &&
            selectionRevision_ == model.selectionRevision && registryRevision_ == registry &&
            behaviorRegistryRevision_ == model.inspectorBehaviorRegistryRevision && approvalRevision_ == model.inspectorApprovalRevision)
            return;
        entries_.clear();
        coverage_.clear();
        documentRevision_ = model.documentRevision.value;
        documentGeneration_ = model.inspectorDocumentGeneration;
        selectionRevision_ = model.selectionRevision;
        registryRevision_ = registry;
        behaviorRegistryRevision_ = model.inspectorBehaviorRegistryRevision;
        approvalRevision_ = model.inspectorApprovalRevision;
        if (!documentGeneration_ || !selectionRevision_ || !model.primarySelection || selection.empty())
            return;
        const auto primary = std::ranges::find(model.objects, *model.primarySelection, &SceneObject::id);
        if (primary == model.objects.end())
            return;
        for (const auto &behavior : primary->components.behaviors) {
            const auto descriptor = std::ranges::find(model.availableBehaviors, behavior.typeId, &Gameplay::BehaviorDescriptor::typeId);
            if (descriptor == model.availableBehaviors.end() || descriptor->schemaVersion != behavior.schemaVersion)
                continue;
            auto available = host_.Contributions(behavior.typeId.Value(), behavior.schemaVersion);
            if (available.HasError())
                continue;
            Coverage covered{behavior.instanceId, {}};
            for (const auto &contribution : available.Value()) {
                if (entries_.size() >= 16)
                    break;
                auto captured = Capture(contribution, *descriptor, behavior, model, selection);
                if (!captured)
                    continue;
                if (captured->approved)
                    covered.fields.insert(covered.fields.end(), captured->covered.begin(), captured->covered.end());
                entries_.push_back(std::move(*captured));
            }
            if (!covered.fields.empty())
                coverage_.push_back(std::move(covered));
        }
    }
}  // namespace Horo::Editor
