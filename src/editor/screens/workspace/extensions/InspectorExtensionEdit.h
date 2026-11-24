#pragma once
/** @file InspectorExtensionEdit.h @brief Owner-fenced user intent from a validated restricted inspector projection. */
#include "Horo/Editor/InspectorBehaviorBinding.h"
#include "Horo/Extensions/EditorInspectorHost.h"
#include "Horo/Extensions/EditorSurfaceDescriptor.h"

namespace Horo::Editor {
    /** @brief Explicit document-owner approval for an exact provider/declaration/type/field closure; never persisted or module-issued. */
    struct InspectorProviderApproval final {
        Extensions::EditorSurfaceProviderIdentity provider;
        std::string surface;
        Gameplay::BehaviorTypeId behavior;
        std::uint32_t schemaVersion{};
        std::uint64_t documentGeneration{};
        std::uint64_t behaviorRegistryRevision{};
        std::vector<std::string> fields;
        std::shared_ptr<const Extensions::EditorInspectorDefinition> declaration;
    };

    /** @brief Typed user edit; every identity and declared field is re-resolved by the real controller before one document transaction. */
    struct InspectorExtensionEdit final {
        Extensions::EditorSurfaceProviderIdentity provider;
        std::string surface;
        Gameplay::BehaviorTypeId behavior;
        std::uint32_t schemaVersion{};
        std::uint64_t documentGeneration{};
        std::uint64_t behaviorRegistryRevision{};
        std::uint64_t documentRevision{};
        std::uint64_t selectionRevision{};
        std::vector<InspectorBehaviorTarget> targets;
        std::string field;
        Gameplay::BehaviorFieldValue value;
    };
}  // namespace Horo::Editor
