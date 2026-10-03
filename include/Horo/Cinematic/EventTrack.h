#pragma once

/**
 * @file EventTrack.h
 * @brief Cooked, versioned gameplay event bindings and immutable payload ownership.
 */

#include "Horo/Cinematic/CinematicIdentity.h"
#include "Horo/Cinematic/CurveSampling.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Extensions {
    class ScriptExportDescriptorSnapshot;
}

namespace Horo::Cinematic {
    struct AuthoredScriptEventKey;
    struct ScriptEventDescriptor;
    struct EventBindingIdentityTag;
    struct EventPayloadSchemaIdentityTag;

    /** @brief Stable module-qualified gameplay event identity. */
    using EventBindingId = CinematicIdentity<EventBindingIdentityTag>;
    /** @brief Stable typed payload schema identity; generation is the compatible schema revision. */
    using EventPayloadSchemaId = CinematicIdentity<EventPayloadSchemaIdentityTag>;

    /** @brief Session contexts explicitly admitted by a cooked event descriptor. */
    enum class EventRuntimeContext : std::uint8_t {
        Runtime = 1,
        Pie = 2,
        Preview = 4,
        Headless = 8
    };

    /** @brief One event key with a bounded, canonical script-value payload owned by the cook plan. */
    struct CookedEventKey final {
        TrackId track;
        KeyframeId key;
        EventBindingId binding;
        EventPayloadSchemaId schema;
        std::vector<std::byte> payload;
        bool required{true};
        std::byte allowedContexts{};
        CurveTime time{};
        bool fireInReverse{};
    };

    /** @brief Immutable cook artifact retained by the session until all staged occurrences retire. */
    class CookedEventPlan final {
        struct ConstructionKey {
        private:
            friend class CookedEventPlan;
            ConstructionKey() = default;
        };

    public:
        /** @brief Cook-only construction after schema validation; callers cannot create the private construction key. */
        CookedEventPlan(ConstructionKey, std::vector<CookedEventKey> keys) noexcept;
        /** @brief Finds one exact track/key binding. @return Borrowed key or nullptr. */
        [[nodiscard]] const CookedEventKey *Find(TrackId track, KeyframeId key) const noexcept;
        /** @brief Returns canonical immutable keys. @return Borrowed plan storage. */
        [[nodiscard]] std::span<const CookedEventKey> Keys() const noexcept;

    private:
        friend Result<std::shared_ptr<const CookedEventPlan>> CookScriptEvents(
            std::span<const AuthoredScriptEventKey>, std::span<const ScriptEventDescriptor>,
            const std::shared_ptr<const Extensions::ScriptExportDescriptorSnapshot> &, EventRuntimeContext);
        /** @brief Publishes only payloads already schema-validated by the typed cook adapter. */
        [[nodiscard]] static Result<std::shared_ptr<const CookedEventPlan>> Create(std::vector<CookedEventKey> keys);
        std::vector<CookedEventKey> keys_;
    };
}  // namespace Horo::Cinematic
