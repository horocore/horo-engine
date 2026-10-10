#pragma once

/** @file XRActionBindings.h
 * @brief Typed XR/Input action schemas, profile resolution and neutral replacement boundaries.
 */

#include "Horo/Runtime/Input.h"
#include "Horo/XR/XRTrackingSnapshot.h"

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace Horo::XR {
    /** @brief Registered persistent profile identity; zero denotes an unknown runtime profile. */
    struct XRInteractionProfileId final {
        std::uint32_t value{};
        constexpr auto operator<=>(const XRInteractionProfileId &) const noexcept = default;
    };

    /** @brief Registered persistent physical-control identity, independent of paths, glyphs and labels. */
    struct XRPhysicalControlId final {
        std::uint32_t value{};
        constexpr auto operator<=>(const XRPhysicalControlId &) const noexcept = default;
    };

    /** @brief Product-declared native action-set grouping; unrelated to dynamic Input context activation. */
    struct XRActionSetId final {
        std::uint32_t value{};
        constexpr auto operator<=>(const XRActionSetId &) const noexcept = default;
    };

    /** @brief Finite lifecycle-time plan ceilings, never per-frame allocation budgets. */
    struct XRActionBindingLimits final {
        static constexpr std::size_t MaximumActions = 256;
        static constexpr std::size_t MaximumBindings = 1024;
        static constexpr std::size_t MaximumProfiles = 32;
        static constexpr std::size_t MaximumSets = 16;
        static constexpr std::size_t MaximumNameBytes = 128;
        static constexpr std::size_t MaximumRegisteredActions = 4096;
    };

    /** @brief Canonical registered action accepted by the XR/Input composition boundary. */
    struct XRActionDeclaration final {
        Input::ActionId action;
        Input::InputContextId context;
        Input::ActionValueType valueType{Input::ActionValueType::Digital};
        XRActionSetId set;
        XRTrackedDeviceRole role{XRTrackedDeviceRole::LeftController};
        bool required{};
    };

    /** @brief Backend catalog fact describing one registered profile control and its exact value type. */
    struct XRProfileControl final {
        XRInteractionProfileId profile;
        XRPhysicalControlId control;
        XRTrackedDeviceRole role{XRTrackedDeviceRole::LeftController};
        Input::ActionValueType valueType{Input::ActionValueType::Digital};
    };

    /** @brief Product suggested binding; identity never contains a native component path. */
    struct XRSuggestedActionBinding final {
        Input::ActionId action;
        XRInteractionProfileId profile;
        XRPhysicalControlId control;
    };

    /** @brief Explicit compatible control rename from one older schema directly to the current schema. */
    struct XRBindingMigration final {
        std::uint32_t fromVersion{};
        XRPhysicalControlId previous;
        XRPhysicalControlId current;
    };

    /** @brief Synchronously borrowed immutable product declarations, validated before native creation. */
    struct XRActionBindingSchema final {
        std::uint32_t version{};
        std::span<const XRActionDeclaration> actions;
        std::span<const XRSuggestedActionBinding> suggestions;
        std::span<const XRBindingMigration> migrations;
        XRInteractionProfileId genericFallback;                     /**< Zero forbids generic fallback. */
        std::span<const Input::ActionDescriptor> registeredActions; /**< Input registry authority; selected metadata must match exactly. */
    };

    /** @brief Input-owned persisted override with exact schema, semantic action and physical-control identities. */
    struct XRActionBindingOverride final {
        std::uint32_t schemaVersion{};
        Input::ActionId action;
        XRInteractionProfileId profile;
        XRPhysicalControlId control;
    };

    /** @brief Active profile evidence for one controller role; zero profile means unknown/absent. */
    struct XRActiveInteractionProfile final {
        XRTrackedDeviceRole role{XRTrackedDeviceRole::LeftController};
        XRInteractionProfileId profile;
        constexpr auto operator<=>(const XRActiveInteractionProfile &) const noexcept = default;
    };

    /** @brief Fully resolved native-free action binding, copied into canonical semantic-action order. */
    struct XRResolvedActionBinding final {
        Input::ActionId action;
        Input::InputContextId context;
        Input::ActionValueType valueType{Input::ActionValueType::Digital};
        XRActionSetId set;
        XRTrackedDeviceRole role{XRTrackedDeviceRole::LeftController};
        XRInteractionProfileId profile;
        XRPhysicalControlId control;
        bool fallback{};
        bool migratedOverride{};
        bool operator==(const XRResolvedActionBinding &) const noexcept = default;
    };

    /** @brief Owned deterministic binding publication without native paths or presentation labels. */
    struct XRResolvedActionBindings final {
        std::uint32_t schemaVersion{};
        std::vector<XRResolvedActionBinding> bindings;
        bool operator==(const XRResolvedActionBindings &) const noexcept = default;
    };

    /**
     * @brief Validates finite schema/catalog/override declarations without native effects.
     * @param schema Registered product actions, suggestions, migrations and fallback policy.
     * @param catalog Backend-supplied registered physical controls.
     * @param overrides Input-owned overrides; incompatible migrations and duplicate overrides fail atomically.
     * @return Success or typed invalid, capacity, incompatible or unsupported diagnostics.
     */
    [[nodiscard]] Result<void> ValidateXRActionBindings(const XRActionBindingSchema &schema, std::span<const XRProfileControl> catalog,
                                                        std::span<const XRActionBindingOverride> overrides);
    /**
     * @brief Resolves exact per-role profiles, required actions, explicit fallback and migrated overrides deterministically.
     * @param schema Validated finite product declarations.
     * @param catalog Registered profile/control/type facts.
     * @param profiles At most one current profile per controller role; absent roles use only declared fallback.
     * @param overrides Persisted Input-owned overrides, copied semantically without modifying their source.
     * @return Owned canonical binding result or an actionable typed failure; no partial plan escapes.
     * @throws std::bad_alloc When bounded lifecycle-time result storage cannot be allocated.
     */
    [[nodiscard]] Result<XRResolvedActionBindings> ResolveXRActionBindings(const XRActionBindingSchema &schema,
                                                                           std::span<const XRProfileControl> catalog,
                                                                           std::span<const XRActiveInteractionProfile> profiles,
                                                                           std::span<const XRActionBindingOverride> overrides = {});

    /** @brief Host-composed Input boundary; neutralization commits before a replacement becomes eligible for projection. */
    class IXRBindingNeutralizer {
    public:
        virtual ~IXRBindingNeutralizer() = default;
        /** @brief Releases all actions/capture from the exact previous session and binding revision synchronously.
         * @param session Exact retired projection owner.
         * @param revision Previous admitted binding revision.
         */
        virtual void Neutralize(const XRSessionId &session, std::uint64_t revision) noexcept = 0;
    };

    /** @brief Owner-thread binding publication; the neutralizer must outlive this non-copyable owner. */
    class XRActionBindingCoordinator final {
    public:
        /** @brief Binds one exact session and host Input port without native effects.
         * @param session Valid session owner; malformed identity rejects Update.
         * @param neutralizer Host port whose synchronous boundary releases stale projected actions.
         */
        XRActionBindingCoordinator(XRSessionId session, IXRBindingNeutralizer &neutralizer) noexcept;
        /** @brief Neutralizes any retained projection; host keeps the port alive through destruction. */
        ~XRActionBindingCoordinator();
        XRActionBindingCoordinator(const XRActionBindingCoordinator &) = delete;
        XRActionBindingCoordinator &operator=(const XRActionBindingCoordinator &) = delete;
        /** @brief Resolves and atomically publishes; profile/schema/override changes neutralize before publication.
         * @param session Exact current owner; foreign calls cannot retire a current publication.
         * @param schema Product binding schema.
         * @param catalog Backend control catalog.
         * @param profiles Current per-role runtime profiles.
         * @param overrides Input-owned overrides.
         * @return Success or typed failure. A resolution failure retires stale bindings before returning.
         * @throws std::bad_alloc Bounded candidate allocation failure; prior projection is neutralized before propagation.
         */
        [[nodiscard]] Result<void> Update(const XRSessionId &session, const XRActionBindingSchema &schema,
                                          std::span<const XRProfileControl> catalog, std::span<const XRActiveInteractionProfile> profiles,
                                          std::span<const XRActionBindingOverride> overrides = {});
        /** @brief Returns the current owned immutable plan, or null while unsupported/shut down. @return Borrow valid until
         * Update/Shutdown. */
        [[nodiscard]] const XRResolvedActionBindings *Current() const noexcept;
        /** @brief Returns the non-wrapping current binding generation. @return Zero before initial admission. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;
        /** @brief Closes admission and neutralizes the prior projection exactly once. */
        void Shutdown() noexcept;

    private:
        void Retire() noexcept;
        XRSessionId session_;
        IXRBindingNeutralizer *neutralizer_;
        std::optional<XRResolvedActionBindings> current_;
        std::array<XRActiveInteractionProfile, 2> profiles_{};
        std::uint64_t revision_{};
    };
}  // namespace Horo::XR
