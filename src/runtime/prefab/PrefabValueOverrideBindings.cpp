#include "PrefabValueResolutionInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Prefab::ValueDetail {
    namespace {
        /** @brief Charge owned provenance before copying possibly large strings into the immutable result. */
        Result<void> AddBinding(ResolutionState &state, PrefabValueOverrideTarget target, PrefabOverrideValue value,
                                const std::size_t object, const std::optional<std::size_t> component,
                                const PrefabValuePropertySchema *schema, const PrefabValueOrigin origin) {
            const auto size = EncodeTarget(target).dump(0).size() + 2 * EncodeValue(value).dump(0).size() +
                              (schema ? EncodeValue(schema->defaultValue).dump(0).size() : 0) + sizeof(ValueBinding);
            if (state.bindings.size() >= PrefabHardLimits::OverrideRecords ||
                state.provenanceBytes > state.limits.Policy().maximumOverrideSetBytes ||
                size > state.limits.Policy().maximumOverrideSetBytes - state.provenanceBytes)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            state.provenanceBytes += size;
            PrefabValueProvenance provenance{std::move(target),
                                             origin,
                                             schema ? std::optional{schema->defaultValue} : std::nullopt,
                                             value,
                                             std::move(value),
                                             state.objects[object].sourcePrefab};
            state.bindings.push_back({std::move(provenance), object, component, schema});
            return Result<void>::Success();
        }

        /** @brief Decode the actual source component once; duplicate keys and excessive nesting never enter resolution. */
        Result<Json *> CapturePayload(ResolutionState &state, const std::size_t object, const std::size_t component) {
            const auto &source = state.objects[object].object.components[component].component.payload;
            Detail::JsonParseGuard guard{PrefabHardLimits::SourceJsonDepth};
            if (source.empty())
                return Invalid<Json *>("Registered component source payload is empty.");
            const auto *first = reinterpret_cast<const char *>(source.data());
            auto json = Json::parse(first, first + source.size(), std::ref(guard), false);
            if (json.is_discarded() || !json.is_object() || guard.HasDuplicate() || guard.IsTooDeep())
                return Invalid<Json *>("Registered component source payload is malformed.");
            const auto found = state.payloads.emplace(std::pair{object, component}, std::move(json));
            return Result<Json *>::Success(&found.first->second);
        }

        /** @brief Capture exact registered values or declared defaults; names and vector positions never retarget a property. */
        Result<void> CaptureComponent(ResolutionState &state, const std::size_t object, const std::size_t component) {
            const auto &source = state.objects[object];
            const auto &payload = source.object.components[component];
            Json *json{};
            const auto schemas = state.schemas->Properties();
            const auto first =
                std::ranges::lower_bound(schemas, std::string_view(payload.component.typeId.Value()), {}, [](const auto &schema) {
                return std::string_view(schema.componentType.Value());
            });
            for (auto found = first; found != schemas.end() && found->componentType == payload.component.typeId; ++found) {
                const auto &schema = *found;
                if (schema.componentSchemaVersion != payload.component.schemaVersion)
                    continue;
                if (auto charged = state.budget.Consume(1); charged.HasError())
                    return charged;
                if (!json) {
                    auto captured = CapturePayload(state, object, component);
                    if (captured.HasError())
                        return Result<void>::Failure(captured.ErrorValue());
                    json = captured.Value();
                }
                const bool present = json->contains(schema.payloadProperty.Value());
                if (!present && schema.required)
                    return Invalid<void>("Required source component property is absent.");
                // Registry defaults are inspection metadata; they never synthesize absent authored source values.
                if (!present)
                    continue;
                auto value = ReadPayloadValue((*json)[schema.payloadProperty.Value()], schema.kind);
                if (value.HasError())
                    return Result<void>::Failure(value.ErrorValue());
                if (auto valid = ValidateValue(value.Value(), &schema); valid.HasError())
                    return valid;
                auto target = PrefabPropertyAddress::Create(source.key.object, schema.componentType, payload.instance, schema.property);
                if (target.HasError())
                    return Result<void>::Failure(target.ErrorValue());
                const auto origin =
                    source.sourcePrefab == state.source.RootAsset() ? PrefabValueOrigin::Source : PrefabValueOrigin::Inherited;
                if (auto added = AddBinding(state, std::move(target).Value(), std::move(value).Value(), object, component, &schema, origin);
                    added.HasError())
                    return added;
            }
            return Result<void>::Success();
        }

        /** @brief Re-admit source allocation bounds under this operation's exact captured policy before copying it. */
        Result<void> AdmitSource(const EffectivePrefabCandidate &source, const PrefabLimitProfile &limits) {
            if (source.Objects().size() > limits.Policy().maximumObjectCount)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            std::size_t bytes{};
            for (const auto &object : source.Objects()) {
                if (object.object.components.size() > limits.Policy().maximumComponentsPerObject)
                    return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
                for (const auto &component : object.object.components) {
                    if (component.component.payload.size() > limits.Policy().maximumExpandedPayloadBytes - bytes)
                        return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
                    bytes += component.component.payload.size();
                }
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @brief Capture immutable source/default/inheritance evidence before any override mutates a detached payload. */
    Result<void> CaptureBindings(ResolutionState &state) {
        if (auto valid = AdmitSource(state.source, state.limits); valid.HasError())
            return valid;
        state.objects.assign(state.source.Objects().begin(), state.source.Objects().end());
        for (std::size_t i = 0; i < state.objects.size(); ++i) {
            if (state.cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PrefabErrors::Cancelled));
            if (auto charged = state.budget.Consume(1); charged.HasError())
                return charged;
            const auto &object = state.objects[i];
            const auto origin = object.sourcePrefab == state.source.RootAsset() ? PrefabValueOrigin::Source : PrefabValueOrigin::Inherited;
            if (auto added = AddBinding(state, object.key.object, object.object.localTransform, i, std::nullopt, nullptr, origin);
                added.HasError())
                return added;
            if (state.schemas)
                for (std::size_t c = 0; c < object.object.components.size(); ++c)
                    if (auto captured = CaptureComponent(state, i, c); captured.HasError())
                        return captured;
        }
        std::ranges::sort(state.bindings, [](const ValueBinding &a, const ValueBinding &b) {
            return TargetLess(a.provenance.target, b.provenance.target);
        });
        return Result<void>::Success();
    }

    /** @brief Find one exact stable target with logarithmic lookup in captured canonical metadata. */
    ValueBinding *FindBinding(ResolutionState &state, const PrefabValueOverrideTarget &target) {
        auto found = std::ranges::lower_bound(state.bindings, target, TargetLess, [](const ValueBinding &binding) -> const auto & {
            return binding.provenance.target;
        });
        return found != state.bindings.end() && found->provenance.target == target ? &*found : nullptr;
    }

    /** @brief Apply one validated value solely to operation-owned objects, preserving a nested root's original mount transform. */
    Result<void> WriteBinding(ResolutionState &state, ValueBinding &binding, const PrefabOverrideValue &value) {
        if (!binding.component) {
            auto &object = state.objects[binding.object];
            const auto &transform = std::get<Math::Transform>(value);
            auto effective = Math::TryDecomposeAffineTRS(Math::Multiply(object.placementTransform.ToMatrix(), transform.ToMatrix()));
            if (effective.HasError())
                return Result<void>::Failure(effective.ErrorValue());
            object.object.localTransform = transform;
            object.effectiveLocalTransform = effective.Value();
        } else {
            state.modifiedPayloads.emplace(binding.object, *binding.component);
            state.payloads.at({binding.object, *binding.component})[binding.schema->payloadProperty.Value()] = PayloadValue(value);
        }
        binding.provenance.effectiveValue = value;
        binding.provenance.origin = PrefabValueOrigin::Instance;
        return Result<void>::Success();
    }

    /** @brief Validate aggregate derived payload size before replacing any owned component envelope. */
    Result<void> FinishPayloads(ResolutionState &state) {
        std::size_t bytes{};
        for (auto &object : state.objects)
            for (auto &component : object.object.components) {
                const auto objectIndex = static_cast<std::size_t>(&object - state.objects.data());
                const auto componentIndex = static_cast<std::size_t>(&component - object.object.components.data());
                const auto payload = state.payloads.find({objectIndex, componentIndex});
                if (payload != state.payloads.end() && state.modifiedPayloads.contains({objectIndex, componentIndex})) {
                    const auto encoded = payload->second.dump();
                    if (encoded.size() > Gameplay::MaximumSerializedComponentBytes ||
                        encoded.size() > state.limits.Policy().maximumExpandedPayloadBytes - bytes)
                        return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
                    const auto raw = std::as_bytes(std::span(encoded.data(), encoded.size()));
                    component.component.payload.assign(raw.begin(), raw.end());
                }
                if (component.component.payload.size() > state.limits.Policy().maximumExpandedPayloadBytes - bytes)
                    return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
                bytes += component.component.payload.size();
            }
        return Result<void>::Success();
    }
}  // namespace Horo::Prefab::ValueDetail
