#include "Horo/Editor/InspectorBehaviorBinding.h"

#include "Horo/Scene/SceneSource.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        /** @brief Returns the canonical owner error without inventing a second edit/history protocol. */
        template <class T> Result<T> Reject(SceneTransactionFailure reason = SceneTransactionFailure::Invalid) {
            return Result<T>::Failure(MakeError(SceneTransactionError(reason)));
        }

        /** @brief Rejects nonfinite authored numeric values before detached replacement preparation. */
        bool ValidValue(const Gameplay::BehaviorFieldValue &value) {
            return std::visit([]<class T>(const T &typed) {
                if constexpr (std::is_same_v<T, std::monostate>)
                    return false;
                else if constexpr (std::is_same_v<T, double>)
                    return std::isfinite(typed);
                else if constexpr (std::is_same_v<T, Math::Vec2>)
                    return std::isfinite(typed.x) && std::isfinite(typed.y);
                else if constexpr (std::is_same_v<T, Math::Vec3>)
                    return std::isfinite(typed.x) && std::isfinite(typed.y) && std::isfinite(typed.z);
                else if constexpr (std::is_same_v<T, Math::Quaternion>)
                    return std::isfinite(typed.x) && std::isfinite(typed.y) && std::isfinite(typed.z) && std::isfinite(typed.w);
                else
                    return true;
            }, value);
        }

        /** @brief Checks declared schema identities and defaults before inspecting any selected payload. */
        bool ValidDescriptor(const Gameplay::BehaviorDescriptor &descriptor) {
            if (!descriptor.typeId.IsValid() || !descriptor.schemaVersion || descriptor.fields.empty() ||
                descriptor.fields.size() > Gameplay::MaximumBehaviorFields)
                return false;
            for (std::size_t i = 0; i < descriptor.fields.size(); ++i) {
                const auto &field = descriptor.fields[i];
                if (field.name.empty() || field.name.size() > Gameplay::MaximumBehaviorFieldNameBytes || !ValidValue(field.defaultValue))
                    return false;
                for (std::size_t j = 0; j < i; ++j)
                    if (descriptor.fields[j].name == field.name)
                        return false;
            }
            return true;
        }

        /** @brief Resolves one actual attachment and checks its schema/value kind against the current owner descriptor. */
        const Gameplay::BehaviorComponent *FindBehavior(const SceneSource::SceneObjectSnapshot &object,
                                                        Gameplay::BehaviorInstanceId attachment,
                                                        const Gameplay::BehaviorDescriptor &descriptor) {
            const auto found = std::ranges::find(object.components.behaviors, attachment, &Gameplay::BehaviorComponent::instanceId);
            if (found == object.components.behaviors.end() || found->typeId != descriptor.typeId ||
                found->schemaVersion != descriptor.schemaVersion || Gameplay::ValidateBehaviorComponent(*found).HasError())
                return nullptr;
            for (const auto &declared : descriptor.fields) {
                const auto field = std::ranges::find(found->fields, declared.name, &Gameplay::BehaviorField::name);
                if (field == found->fields.end() || field->value.index() != declared.defaultValue.index())
                    return nullptr;
            }
            return std::to_address(found);
        }

        /** @brief Copies one target after reserving its bounded canonical content; no borrowed object escapes. */
        Result<void> CaptureTarget(InspectorBehaviorDraft &draft, const SceneTransactionSnapshot &snapshot,
                                   const InspectorBehaviorTarget &target, const Gameplay::BehaviorDescriptor &descriptor,
                                   std::size_t &bytes) {
            const auto object = std::ranges::find(snapshot.objects, target.object, &SceneSource::SceneObjectSnapshot::id);
            if (object == snapshot.objects.end() || !target.attachment.IsValid())
                return Reject<void>(SceneTransactionFailure::Stale);
            const auto *behavior = FindBehavior(*object, target.attachment, descriptor);
            if (!behavior)
                return Reject<void>(SceneTransactionFailure::Stale);
            auto digest = FingerprintSceneObjectContent(*object, 1024 * 1024);
            if (digest.HasError())
                return Result<void>::Failure(digest.ErrorValue());
            // The existing Scene codec applies the same bounded, lossless authoring representation.
            const auto size = SceneSource::EncodeSceneSource({std::span{std::to_address(object), 1}, {}}).size();
            if (size > 8 * 1024 * 1024 - bytes)
                return Reject<void>(SceneTransactionFailure::Capacity);
            bytes += size;
            for (auto &field : draft.fields) {
                const auto source = std::ranges::find(behavior->fields, field.field, &Gameplay::BehaviorField::name);
                if (draft.baselines.empty())
                    field.value = source->value;
                else
                    field.mixed = field.mixed || field.value != source->value;
            }
            draft.baselines.emplace_back(*object, target.attachment, digest.Value());
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CaptureInspectorBehavior */
    Result<InspectorBehaviorDraft> CaptureInspectorBehavior(const SceneTransactionSnapshot &snapshot,
                                                            const std::span<const InspectorBehaviorTarget> targets,
                                                            const Gameplay::BehaviorDescriptor &descriptor,
                                                            const std::uint64_t selectionGeneration) {
        if (!snapshot.session.owner.IsValid() || !snapshot.session.generation || !selectionGeneration || targets.empty() ||
            targets.size() > MaximumInspectorBehaviorSelection || !ValidDescriptor(descriptor))
            return Reject<InspectorBehaviorDraft>();
        InspectorBehaviorDraft draft{snapshot.session,
                                     snapshot.revision,
                                     snapshot.state,
                                     selectionGeneration,
                                     descriptor.typeId,
                                     descriptor.schemaVersion,
                                     {},
                                     {}};
        draft.baselines.reserve(targets.size());
        for (const auto &field : descriptor.fields)
            draft.fields.emplace_back(field.name, field.defaultValue, false);
        std::size_t bytes{};
        for (std::size_t i = 0; i < targets.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j)
                if (targets[i].object == targets[j].object)
                    return Reject<InspectorBehaviorDraft>();
            if (auto result = CaptureTarget(draft, snapshot, targets[i], descriptor, bytes); result.HasError())
                return Result<InspectorBehaviorDraft>::Failure(result.ErrorValue());
        }
        return Result<InspectorBehaviorDraft>::Success(std::move(draft));
    }

    /** @copydoc PrepareInspectorBehaviorEdit */
    Result<std::vector<SceneObjectReplacement>> PrepareInspectorBehaviorEdit(const InspectorBehaviorDraft &draft,
                                                                             const std::string_view field,
                                                                             const Gameplay::BehaviorFieldValue &value,
                                                                             const std::uint64_t selectionGeneration) {
        if (!selectionGeneration || selectionGeneration != draft.selectionGeneration)
            return Reject<std::vector<SceneObjectReplacement>>(SceneTransactionFailure::Stale);
        if (const auto declared = std::ranges::find(draft.fields, field, &InspectorBehaviorValue::field);
            !ValidValue(value) || declared == draft.fields.end() || value.index() != declared->value.index() || draft.baselines.empty() ||
            draft.baselines.size() > MaximumInspectorBehaviorSelection)
            return Reject<std::vector<SceneObjectReplacement>>();
        std::vector<SceneObjectReplacement> changes;
        changes.reserve(draft.baselines.size());
        for (const auto &baseline : draft.baselines) {
            auto candidate = baseline.object;
            const auto attachment =
                std::ranges::find(candidate.components.behaviors, baseline.attachment, &Gameplay::BehaviorComponent::instanceId);
            if (attachment == candidate.components.behaviors.end() || attachment->typeId != draft.type ||
                attachment->schemaVersion != draft.schemaVersion || candidate.generated)
                return Reject<std::vector<SceneObjectReplacement>>();
            const auto selected = std::ranges::find(attachment->fields, field, &Gameplay::BehaviorField::name);
            if (selected == attachment->fields.end())
                return Reject<std::vector<SceneObjectReplacement>>();
            selected->value = value;
            if (auto valid = Gameplay::ValidateBehaviorComponent(*attachment); valid.HasError())
                return Result<std::vector<SceneObjectReplacement>>::Failure(valid.ErrorValue());
            changes.push_back({candidate.id, baseline.content, candidate.generated, std::move(candidate), {}});
        }
        return Result<std::vector<SceneObjectReplacement>>::Success(std::move(changes));
    }
}  // namespace Horo::Editor
