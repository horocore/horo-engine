#pragma once

/**
 * @file ScriptEventCook.h
 * @brief Host-side event cook adapter for versioned script export descriptors.
 */

#include "Horo/Cinematic/EventTrack.h"
#include "Horo/Extensions/ScriptValue.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Horo::Cinematic {
    /** @brief Host-declared mapping from a qualified event name to an immutable script export function. */
    struct ScriptEventDescriptor final {
        std::string qualifiedName;
        EventBindingId binding;
        EventPayloadSchemaId schema;
        std::string exportApiId;
        std::string exportFunctionId;
        Extensions::ScriptExportVersion exportVersion; /**< Exact validated API generation expected by this binding. */
        std::byte allowedContexts{};                   /**< Bitset of EventRuntimeContext values. */
        std::size_t maximumPayloadBytes{64U * 1024U};
        bool required{true};
    };

    /** @brief One authored key whose values are validated against the exact export snapshot at cook. */
    struct AuthoredScriptEventKey final {
        TrackId track;
        KeyframeId key;
        std::string qualifiedName;
        std::vector<Extensions::ScriptValue> arguments;
        CurveTime time{};
        bool fireInReverse{};
    };

    /**
     * @brief Resolves and validates all event keys against one immutable script export generation.
     * @param keys Complete authored event keys from a validated sequence source.
     * @param descriptors Host-registered qualified event mappings; duplicate names or IDs fail.
     * @param exports Exact immutable script export generation used for cook.
     * @param context Product context admitted by the target cook profile.
     * @return Immutable, bounded payload plan or an actionable typed cook error with track/key context.
     * @post No module callback, provider, service locator, or runtime handler is invoked.
     */
    [[nodiscard]] Result<std::shared_ptr<const CookedEventPlan>> CookScriptEvents(
        std::span<const AuthoredScriptEventKey> keys, std::span<const ScriptEventDescriptor> descriptors,
        const Extensions::ScriptExportDescriptorSnapshotPtr &exports, EventRuntimeContext context);
}  // namespace Horo::Cinematic
