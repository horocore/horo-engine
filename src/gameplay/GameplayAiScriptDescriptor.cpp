#include "Horo/Gameplay/GameplayAiScriptBridge.h"

namespace Horo::Gameplay {
    namespace {
        using namespace Extensions;
        constexpr ScriptExportVersion Version{1, 0, 0};

        /** @brief Constructs a required logical scalar field. */
        ScriptExportFieldDescriptor PrimitiveField(std::string id, ScriptExportPrimitiveKind type) {
            return {.id = std::move(id), .type = {.primitive = type}, .introducedVersion = Version};
        }

        /** @brief Constructs a required named logical field. */
        ScriptExportFieldDescriptor NamedField(std::string id, std::string type) {
            return {.id = std::move(id), .type = {.namedType = std::move(type)}, .introducedVersion = Version};
        }

        /** @brief Builds bounded value projections without ABI layout or native handles. */
        std::vector<ScriptExportTypeDescriptor> ValueTypes() {
            using P = ScriptExportPrimitiveKind;
            using K = ScriptExportNamedTypeKind;
            ScriptExportTypeDescriptor scalar{.id = "ai_scalar", .kind = K::Struct, .introducedVersion = Version};
            scalar.fields = {PrimitiveField("kind", P::UnsignedInteger),
                             PrimitiveField("boolean", P::Boolean),
                             PrimitiveField("integer", P::SignedInteger),
                             PrimitiveField("number", P::Number),
                             PrimitiveField("incarnation", P::UnsignedInteger),
                             PrimitiveField("slot", P::UnsignedInteger),
                             PrimitiveField("generation", P::UnsignedInteger),
                             PrimitiveField("asset", P::Bytes),
                             PrimitiveField("x", P::SignedInteger),
                             PrimitiveField("y", P::SignedInteger),
                             PrimitiveField("z", P::SignedInteger)};
            ScriptExportTypeDescriptor elements{.id = "ai_elements",
                                                .kind = K::Array,
                                                .introducedVersion = Version,
                                                .elementType = {.namedType = "ai_scalar"},
                                                .maximumElements = AI::MaximumBlackboardCollectionElements};
            ScriptExportTypeDescriptor value{.id = "ai_value", .kind = K::Struct, .introducedVersion = Version};
            value.fields = {PrimitiveField("kind", P::UnsignedInteger),
                            NamedField("scalar", "ai_scalar"),
                            PrimitiveField("element_kind", P::UnsignedInteger),
                            NamedField("elements", "ai_elements"),
                            PrimitiveField("opaque_version", P::UnsignedInteger),
                            PrimitiveField("opaque_type", P::UnsignedInteger),
                            PrimitiveField("opaque", P::Bytes)};
            ScriptExportTypeDescriptor write{.id = "ai_write", .kind = K::Struct, .introducedVersion = Version};
            write.fields = {PrimitiveField("key", P::UnsignedInteger), NamedField("value", "ai_value")};
            ScriptExportTypeDescriptor writes{.id = "ai_writes",
                                              .kind = K::Array,
                                              .introducedVersion = Version,
                                              .elementType = {.namedType = "ai_write"},
                                              .maximumElements = AI::MaximumBlackboardWritesPerBatch};
            return {std::move(scalar), std::move(elements), std::move(value), std::move(write), std::move(writes)};
        }

        /** @brief Builds one synchronous operation declaration with required positional arguments. */
        ScriptExportFunctionDescriptor Function(std::string id, std::vector<ScriptExportTypeReference> parameters,
                                                std::optional<ScriptExportTypeReference> result = {}) {
            ScriptExportFunctionDescriptor function{.id = std::move(id),
                                                    .introducedVersion = Version,
                                                    .invocation = ScriptExportInvocationMode::Synchronous};
            for (std::size_t i = 0; i < parameters.size(); ++i)
                function.parameters.push_back(
                    {.id = "argument" + std::to_string(i), .type = std::move(parameters[i]), .introducedVersion = Version});
            if (result)
                function.results.push_back({.id = "result", .type = std::move(*result), .introducedVersion = Version});
            return function;
        }
    }  // namespace

    /** @copydoc MakeGameplayAiScriptDescriptor */
    Extensions::ScriptExportDescriptor MakeGameplayAiScriptDescriptor(std::string moduleId, Extensions::ExtensionCapabilityId capability) {
        using namespace Extensions;
        const ScriptExportTypeReference integer{.primitive = ScriptExportPrimitiveKind::UnsignedInteger};
        const ScriptExportTypeReference handle{.primitive = ScriptExportPrimitiveKind::Handle};
        ScriptExportDescriptor api{.moduleId = std::move(moduleId),
                                   .id = std::string{GameplayAiScriptApiId},
                                   .nameSpace = "horo.ai",
                                   .version = Version,
                                   .compatibility = {Version, Version},
                                   .service = {std::move(capability), "horo.gameplay.ai", "horo.gameplay.ai.v1", Version}};
        api.types = ValueTypes();
        api.functions = {Function("resolve_agent", {integer}, handle),
                         Function("start_task", {handle, integer}, handle),
                         Function("task_state", {handle}, integer),
                         Function("cancel_task", {handle}),
                         Function("complete_task", {handle}),
                         Function("release", {handle}),
                         Function("read_blackboard", {handle, integer},
                                  ScriptExportTypeReference{.namedType = "ai_value", .nullability = ScriptExportNullability::Nullable}),
                         Function("write_blackboard", {handle, ScriptExportTypeReference{.namedType = "ai_writes"}}, integer)};
        const std::array errors{&AI::AIErrors::CapabilityUnavailable,
                                &AI::AIErrors::RuntimeUnavailable,
                                &AI::AIErrors::HandleInvalid,
                                &AI::AIErrors::IdentityInvalid,
                                &AI::AIErrors::TaskContextInvalid,
                                &AI::AIErrors::TaskTransitionInvalid,
                                &AI::AIErrors::TaskCapacityExceeded,
                                &AI::AIErrors::CanonicalRestoreStale,
                                &AI::AIErrors::BlackboardValueInvalid,
                                &AI::AIErrors::BlackboardValueTypeMismatch,
                                &AI::AIErrors::BlackboardInstanceStale,
                                &AI::AIErrors::BlackboardBatchInvalid,
                                &AI::AIErrors::BlackboardUnknownValueRejected,
                                &AI::AIErrors::BlackboardReentrantMutation,
                                &AI::AIErrors::BlackboardStorageUnavailable,
                                &AI::AIErrors::BlackboardRevisionExhausted};
        for (const auto *error : errors) {
            api.errors.push_back({.id = error->code.Value(), .introducedVersion = Version, .retryable = error->retryable});
            for (auto &function : api.functions)
                function.errorIds.push_back(error->code.Value());
        }
        return api;
    }
}  // namespace Horo::Gameplay
