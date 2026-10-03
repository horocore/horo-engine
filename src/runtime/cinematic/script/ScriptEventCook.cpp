#include "Horo/Cinematic/ScriptEventCook.h"

#include "Horo/Cinematic/EventTrackErrors.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace Horo::Cinematic {
    namespace {
        template <typename T>
        [[nodiscard]] Result<T> Failed(const ErrorCodeDescriptor &code, const AuthoredScriptEventKey &key, std::string reason) {
            return Result<T>::Failure(MakeError(code, std::format("Event track {} key {} ('{}'): {}", key.track.stableValue,
                                                                  key.key.stableValue, key.qualifiedName, reason)));
        }

        [[nodiscard]] bool ValidContext(const EventRuntimeContext context) noexcept {
            using enum EventRuntimeContext;
            return context == Runtime || context == Pie || context == Preview || context == Headless;
        }

        [[nodiscard]] bool DuplicateDescriptor(const std::span<const ScriptEventDescriptor> descriptors) {
            for (std::size_t left = 0; left < descriptors.size(); ++left) {
                const ScriptEventDescriptor &entry = descriptors[left];
                if (entry.qualifiedName.empty() || entry.qualifiedName.size() > 160 || !entry.binding.IsValid() ||
                    !entry.schema.IsValid() || entry.exportApiId.empty() || entry.exportFunctionId.empty() ||
                    entry.allowedContexts == std::byte{} || (entry.allowedContexts & ~std::byte{15}) != std::byte{} ||
                    !entry.exportVersion.IsValid() || entry.maximumPayloadBytes == 0 || entry.maximumPayloadBytes > 64U * 1024U)
                    return true;
                for (std::size_t right = left + 1; right < descriptors.size(); ++right) {
                    if (entry.qualifiedName == descriptors[right].qualifiedName || entry.binding == descriptors[right].binding)
                        return true;
                }
            }
            return false;
        }

        [[nodiscard]] const Extensions::ScriptExportFunctionDescriptor *FindFunction(const Extensions::ScriptExportDescriptor &api,
                                                                                     const std::string &id) noexcept {
            const auto found = std::ranges::find(api.functions, id, &Extensions::ScriptExportFunctionDescriptor::id);
            return found == api.functions.end() ? nullptr : std::to_address(found);
        }

        /** @brief Rejects generation-scoped script handles from durable cooked payloads after bounded value validation. */
        [[nodiscard]] bool ContainsRuntimeHandle(const Extensions::ScriptValue &value) {
            if (value.GetKind() == Extensions::ScriptValue::Kind::Handle)
                return true;
            for (const auto &element : value.AsArray())
                if (ContainsRuntimeHandle(element))
                    return true;
            for (const auto &[key, element] : value.AsMap())
                if (ContainsRuntimeHandle(key) || ContainsRuntimeHandle(element))
                    return true;
            return std::ranges::any_of(value.AsStruct(), [](const auto &field) {
                return ContainsRuntimeHandle(field.second);
            });
        }

        /** @brief Validates durable typed arguments and preserves actionable parameter diagnostics. */
        [[nodiscard]] Result<void> ValidatePayload(const AuthoredScriptEventKey &key,
                                                   const Extensions::ScriptExportFunctionDescriptor &function,
                                                   const Extensions::ScriptExportDescriptor &api) {
            for (std::size_t index = 0; index < std::min(key.arguments.size(), function.parameters.size()); ++index) {
                if (auto validated = Extensions::ValidateScriptValueForType(key.arguments[index], function.parameters[index].type, api);
                    validated.HasError())
                    return Failed<void>(EventTrackErrors::SchemaMismatch, key,
                                        std::format("Parameter '{}' (index {}) has incompatible payload kind {}: {}",
                                                    function.parameters[index].id, index,
                                                    static_cast<unsigned>(key.arguments[index].GetKind()), validated.ErrorValue().message));
            }
            if (auto validated = Extensions::ValidateScriptArguments(function, key.arguments, api); validated.HasError())
                return Failed<void>(EventTrackErrors::SchemaMismatch, key,
                                    std::format("Payload for {}.{} does not match its parameters: {}", api.id, function.id,
                                                validated.ErrorValue().message));
            if (std::ranges::any_of(key.arguments, ContainsRuntimeHandle))
                return Failed<void>(EventTrackErrors::SchemaMismatch, key,
                                    "Durable cinematic payloads cannot retain runtime script handles; "
                                    "author a stable target identity.");

            return Result<void>::Success();
        }

        /** @brief Resolves and owns one exact schema-validated event without invoking gameplay. */
        [[nodiscard]] Result<CookedEventKey> CookKey(const AuthoredScriptEventKey &key, std::span<const ScriptEventDescriptor> descriptors,
                                                     const Extensions::ScriptExportDescriptorSnapshot &exports,
                                                     EventRuntimeContext context) {
            if (!key.track.IsValid() || !key.key.IsValid() || key.time < 0 || key.qualifiedName.size() > 160)
                return Failed<CookedEventKey>(EventTrackErrors::CookInvalid, key, "Invalid key identity.");
            const auto descriptor = std::ranges::find(descriptors, key.qualifiedName, &ScriptEventDescriptor::qualifiedName);
            if (descriptor == descriptors.end())
                return Failed<CookedEventKey>(EventTrackErrors::UnknownName, key, "No qualified event descriptor is registered.");
            if ((descriptor->allowedContexts & static_cast<std::byte>(context)) == std::byte{})
                return Failed<CookedEventKey>(EventTrackErrors::SchemaMismatch, key, "Event is forbidden in the selected product context.");
            const Extensions::ScriptExportDescriptor *api = exports.Find(descriptor->exportApiId);
            if (api == nullptr)
                return Failed<CookedEventKey>(EventTrackErrors::UnknownName, key,
                                              "The owning script export API is absent from this cook generation.");
            if (api->version != descriptor->exportVersion)
                return Failed<CookedEventKey>(EventTrackErrors::SchemaMismatch, key,
                                              "The script export API generation differs from the event binding.");
            const Extensions::ScriptExportFunctionDescriptor *function = FindFunction(*api, descriptor->exportFunctionId);
            if (function == nullptr)
                return Failed<CookedEventKey>(EventTrackErrors::UnknownName, key, "The declared script export function is absent.");

            if (auto validated = ValidatePayload(key, *function, *api); validated.HasError())
                return Result<CookedEventKey>::Failure(validated.ErrorValue());
            auto encoded = Extensions::EncodeScriptValue(Extensions::ScriptValue::Array(key.arguments));
            if (encoded.HasError() || encoded.Value().size() > descriptor->maximumPayloadBytes)
                return Failed<CookedEventKey>(EventTrackErrors::CookCapacityExceeded, key,
                                              "Canonical payload exceeds the event descriptor limit.");
            return Result<CookedEventKey>::Success(CookedEventKey{key.track, key.key, descriptor->binding, descriptor->schema,
                                                                  std::move(encoded).Value(), descriptor->required,
                                                                  descriptor->allowedContexts, key.time, key.fireInReverse});
        }
    }  // namespace

    /** @copydoc CookScriptEvents */
    Result<std::shared_ptr<const CookedEventPlan>> CookScriptEvents(const std::span<const AuthoredScriptEventKey> keys,
                                                                    const std::span<const ScriptEventDescriptor> descriptors,
                                                                    const Extensions::ScriptExportDescriptorSnapshotPtr &exports,
                                                                    const EventRuntimeContext context) {
        if (keys.size() > 2'048 || descriptors.size() > 2'048)
            return Result<std::shared_ptr<const CookedEventPlan>>::Failure(MakeError(EventTrackErrors::CookCapacityExceeded));
        if (!exports || !ValidContext(context) || DuplicateDescriptor(descriptors))
            return Result<std::shared_ptr<const CookedEventPlan>>::Failure(MakeError(EventTrackErrors::CookInvalid));

        std::vector<CookedEventKey> cooked;
        cooked.reserve(keys.size());
        for (const AuthoredScriptEventKey &key : keys) {
            auto result = CookKey(key, descriptors, *exports, context);
            if (result.HasError())
                return Result<std::shared_ptr<const CookedEventPlan>>::Failure(result.ErrorValue());
            cooked.push_back(std::move(result).Value());
        }
        return CookedEventPlan::Create(std::move(cooked));
    }
}  // namespace Horo::Cinematic
