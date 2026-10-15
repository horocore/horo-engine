#include "GameplayAiScriptValue.h"
#include "Horo/Gameplay/GameplayAiScriptBridge.h"

#include <format>

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
            using enum ScriptExportPrimitiveKind;
            using K = ScriptExportNamedTypeKind;
            ScriptExportTypeDescriptor scalar{.id = "ai-scalar", .kind = K::Struct, .introducedVersion = Version};
            scalar.fields = {PrimitiveField("kind", UnsignedInteger),
                             PrimitiveField("boolean", Boolean),
                             PrimitiveField("integer", SignedInteger),
                             PrimitiveField("number", Number),
                             PrimitiveField("incarnation", UnsignedInteger),
                             PrimitiveField("slot", UnsignedInteger),
                             PrimitiveField("generation", UnsignedInteger),
                             PrimitiveField("asset", Bytes),
                             PrimitiveField("x", SignedInteger),
                             PrimitiveField("y", SignedInteger),
                             PrimitiveField("z", SignedInteger)};
            ScriptExportTypeDescriptor elements{.id = "ai-elements",
                                                .kind = K::Array,
                                                .introducedVersion = Version,
                                                .elementType = {.namedType = "ai-scalar"},
                                                .maximumElements = AI::MaximumBlackboardCollectionElements};
            ScriptExportTypeDescriptor value{.id = "ai-value", .kind = K::Struct, .introducedVersion = Version};
            value.fields = {PrimitiveField("kind", UnsignedInteger),
                            NamedField("scalar", "ai-scalar"),
                            PrimitiveField("element-kind", UnsignedInteger),
                            NamedField("elements", "ai-elements"),
                            PrimitiveField("opaque-version", UnsignedInteger),
                            PrimitiveField("opaque-type", UnsignedInteger),
                            PrimitiveField("opaque", Bytes)};
            ScriptExportTypeDescriptor write{.id = "ai-write", .kind = K::Struct, .introducedVersion = Version};
            write.fields = {PrimitiveField("key", UnsignedInteger), NamedField("value", "ai-value")};
            ScriptExportTypeDescriptor writes{.id = "ai-writes",
                                              .kind = K::Array,
                                              .introducedVersion = Version,
                                              .elementType = {.namedType = "ai-write"},
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
                    {.id = std::format("argument{}", i), .type = std::move(parameters[i]), .introducedVersion = Version});
            if (result)
                function.results.push_back({.id = "result", .type = std::move(*result), .introducedVersion = Version});
            return function;
        }
    }  // namespace

    /** @copydoc Detail::GameplayAiScriptErrorIdentities */
    std::span<const Detail::GameplayAiScriptErrorIdentity> Detail::GameplayAiScriptErrorIdentities() noexcept {
        static const std::array
            identities{GameplayAiScriptErrorIdentity{&AI::AIErrors::CapabilityUnavailable, "ai.scene.capability-unavailable"},
                       GameplayAiScriptErrorIdentity{&AI::AIErrors::RuntimeUnavailable, "ai.runtime.unavailable"},
                       GameplayAiScriptErrorIdentity{&AI::AIErrors::HandleInvalid, "ai.handle.invalid"},
                       GameplayAiScriptErrorIdentity{&AI::AIErrors::IdentityInvalid, "ai.identity.invalid"},
                       GameplayAiScriptErrorIdentity{&AI::AIErrors::TaskContextInvalid, "ai.task.context-invalid"},
                       GameplayAiScriptErrorIdentity{&AI::AIErrors::TaskTransitionInvalid, "ai.task.transition-invalid"},
                       GameplayAiScriptErrorIdentity{&AI::AIErrors::TaskCapacityExceeded, "ai.task.capacity-exceeded"},
                       GameplayAiScriptErrorIdentity{&AI::AIErrors::CanonicalRestoreStale, "ai.canonical.restore-stale"},
                       GameplayAiScriptErrorIdentity{&AI::AIErrors::BlackboardValueInvalid, "ai.blackboard.value-invalid"},
                       GameplayAiScriptErrorIdentity{&AI::AIErrors::BlackboardValueTypeMismatch, "ai.blackboard.value-type-mismatch"},
                       GameplayAiScriptErrorIdentity{&AI::AIErrors::BlackboardInstanceStale, "ai.blackboard.instance-stale"},
                       GameplayAiScriptErrorIdentity{&AI::AIErrors::BlackboardBatchInvalid, "ai.blackboard.batch-invalid"},
                       GameplayAiScriptErrorIdentity{&AI::AIErrors::BlackboardUnknownValueRejected, "ai.blackboard.unknown-value-rejected"},
                       GameplayAiScriptErrorIdentity{&AI::AIErrors::BlackboardReentrantMutation, "ai.blackboard.reentrant-mutation"},
                       GameplayAiScriptErrorIdentity{&AI::AIErrors::BlackboardStorageUnavailable, "ai.blackboard.storage-unavailable"},
                       GameplayAiScriptErrorIdentity{&AI::AIErrors::BlackboardRevisionExhausted, "ai.blackboard.revision-exhausted"}};
        return identities;
    }

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
        api.functions = {Function("resolve-agent", {integer}, handle),
                         Function("start-task", {handle, integer}, handle),
                         Function("task-state", {handle}, integer),
                         Function("cancel-task", {handle}),
                         Function("complete-task", {handle}),
                         Function("release", {handle}),
                         Function("read-blackboard", {handle, integer},
                                  ScriptExportTypeReference{.namedType = "ai-value", .nullability = ScriptExportNullability::Nullable}),
                         Function("write-blackboard", {handle, ScriptExportTypeReference{.namedType = "ai-writes"}}, integer)};
        api.errors.push_back({.id = std::string{Detail::GameplayAiNativeErrorId}, .introducedVersion = Version});
        for (auto &function : api.functions)
            function.errorIds.emplace_back(Detail::GameplayAiNativeErrorId);
        for (const auto &identity : Detail::GameplayAiScriptErrorIdentities()) {
            api.errors.push_back(
                {.id = std::string{identity.scriptId}, .introducedVersion = Version, .retryable = identity.native->retryable});
            for (auto &function : api.functions)
                function.errorIds.emplace_back(identity.scriptId);
        }
        return api;
    }
}  // namespace Horo::Gameplay
