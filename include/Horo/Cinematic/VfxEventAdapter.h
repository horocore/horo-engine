#pragma once

/** @file VfxEventAdapter.h
 * @brief Prepared owning consumer of the unchanged cinematic commit-gated event registration seam.
 */

#include "Horo/Cinematic/EventDispatcher.h"
#include "Horo/Gameplay/GameplayVfxContext.h"

namespace Horo::Cinematic {
    /** @brief One declared flat canonical ScriptValue argument projected into a semantic scalar channel. */
    struct VfxEventParameterArgument final {
        std::uint16_t argument{};
        std::uint16_t channel{};
    };

    /** @brief Cold data projection over actual CookScriptEvents argument arrays; no new cinematic wire schema.
     * @details World position uses exact SignedInteger millimeters. Optional orientation, scale, normal and scalar channels
     * use finite Number arguments, narrowed only after range validation. Omitted orientation/scale/normal retain identity/one/absence.
     */
    struct VfxEventPayloadProjection final {
        std::array<std::uint16_t, 3> positionArguments{0, 1, 2};
        std::optional<std::array<std::uint16_t, 4>> orientationArguments;
        std::optional<std::uint16_t> scaleArgument;
        std::optional<std::array<std::uint16_t, 3>> impactNormalArguments;
        std::uint64_t parameterSchema{};
        std::vector<VfxEventParameterArgument> parameters;
    };

    /** @brief Exact registration metadata and finite cold key/result reservation. */
    struct VfxEventBinding final {
        EventBindingId binding;
        EventPayloadSchemaId schema;
        EventRuntimeContext context{EventRuntimeContext::Runtime};
        std::uint64_t generation{};
        std::string semanticEvent;
        std::uint32_t semanticSchemaVersion{1};
        std::size_t maximumKeys{256};
        std::size_t maximumResults{256};
    };

    /** @brief Full original source correlation and source acceptance, never a fabricated completed effect claim. */
    struct VfxEventDispatchRecord final {
        SequenceFrameEventOccurrence occurrence;
        std::uint64_t committedTick{};
        Gameplay::GameplayVfxSubmission submission;
    };

    /** @brief Pins actual cooked payloads, decodes cold, and submits only through the same scoped semantic source/World pipeline.
     * @details EventDispatcher remains the sole cinematic order/commit/dedup owner. Preview is explicitly unsupported by
     * this runtime integration. Registration leases retain prepared adapter state until revocation; no callback parses values,
     * grows a container or selects an effect asset. Source receipts preserve full occurrence correlation independently of traversal.
     */
    class CinematicVfxEventAdapter final {
    public:
        /** @brief Prepares an exact bounded key cache from a genuine CookScriptEvents plan.
         * @param binding Existing registration contract metadata. @param plan Actual immutable cooked payload owner.
         * @param producer Actual Scene-issued committed-source capability. @param projection Declared positional argument mapping.
         * @return Owned adapter or typed unavailable/schema/payload/capacity/allocation failure before any effect is requested.
         */
        [[nodiscard]] static Result<std::shared_ptr<CinematicVfxEventAdapter>> Create(
            VfxEventBinding binding, std::shared_ptr<const CookedEventPlan> plan,
            std::shared_ptr<const Gameplay::GameplayVfxCommittedProducer> producer, VfxEventPayloadProjection projection = {});
        /** @brief Releases this observation facade; outstanding registration leases retain their exact prepared state. */
        ~CinematicVfxEventAdapter();
        /** @brief Returns the unchanged dispatcher registration and lifetime lease.
         * @return Explicit host registration; obtaining it performs no registration or World mutation.
         */
        [[nodiscard]] EventHandlerRegistration Registration() const;
        /** @brief Copies no payloads; borrows bounded full-source correlation records until ClearResults.
         * @return Exact latest adapter records on the owner lane, empty on another thread.
         */
        [[nodiscard]] std::span<const VfxEventDispatchRecord> Results() const noexcept;
        /** @brief Acknowledges adapter evidence only; dispatcher replay/dedup history is unchanged. */
        void ClearResults() noexcept;
        /** @brief Permanently suppresses future callbacks for this exact prepared registration. */
        void Revoke() noexcept;

    private:
        struct State;
        explicit CinematicVfxEventAdapter(std::shared_ptr<State> state);
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::Cinematic
