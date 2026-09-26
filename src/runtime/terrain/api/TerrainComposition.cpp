#include "Horo/Terrain/TerrainComposition.h"

#include <array>
#include <utility>

namespace Horo::Terrain {
    namespace {
        using Requirement = TerrainCapabilityRequirement;

        static_assert(TerrainCompositionCapabilityCount == 12, "Update every product policy when the capability vocabulary changes");

        [[nodiscard]] constexpr bool IsKnown(const TerrainFoliageCapability capability) noexcept {
            return capability < TerrainFoliageCapability::Count;
        }

        [[nodiscard]] constexpr bool IsKnown(const TerrainCompositionLifecycle lifecycle) noexcept {
            return lifecycle < TerrainCompositionLifecycle::Count;
        }

        /** @brief Checks complete one-fact-per-family evidence without accepting an inferred service. */
        [[nodiscard]] Result<std::array<TerrainCapabilityFact, TerrainCompositionCapabilityCount>> OrderFacts(
            const std::span<const TerrainCapabilityFact> facts) {
            using OrderedFacts = std::array<TerrainCapabilityFact, TerrainCompositionCapabilityCount>;
            if (facts.size() != TerrainCompositionCapabilityCount)
                return Result<OrderedFacts>::Failure(MakeError(TerrainErrors::CompositionInvalid));
            OrderedFacts ordered{};
            std::array<bool, TerrainCompositionCapabilityCount> seen{};
            for (const TerrainCapabilityFact &fact : facts) {
                if (!IsKnown(fact.capability) || fact.available != fact.revision.IsValid())
                    return Result<OrderedFacts>::Failure(MakeError(TerrainErrors::CompositionInvalid));
                const auto index = static_cast<std::size_t>(fact.capability);
                if (seen[index])
                    return Result<OrderedFacts>::Failure(MakeError(TerrainErrors::CompositionInvalid));
                seen[index] = true;
                ordered[index] = fact;
            }
            return Result<OrderedFacts>::Success(ordered);
        }

        /** @brief Builds an explicit fixed policy in capability-enum order. */
        [[nodiscard]] constexpr TerrainProductProfilePolicy Policy(
            const TerrainProductProfile profile, const std::array<Requirement, TerrainCompositionCapabilityCount> &requirements) noexcept {
            return {profile, requirements};
        }

        /** @brief A dependent component cannot report a grant without its owning runtime or render path. */
        [[nodiscard]] bool EffectiveAvailable(const std::array<TerrainCapabilityFact, TerrainCompositionCapabilityCount> &facts,
                                              const TerrainFoliageCapability capability) noexcept {
            const auto present = [&](const TerrainFoliageCapability value) {
                return facts[static_cast<std::size_t>(value)].available;
            };
            if (!present(capability))
                return false;
            if (capability == TerrainFoliageCapability::FoliageRuntime)
                return present(TerrainFoliageCapability::TerrainRuntime);
            if (capability == TerrainFoliageCapability::CpuCulling || capability == TerrainFoliageCapability::GpuIndirectCulling)
                return present(TerrainFoliageCapability::RenderExtraction);
            if (capability == TerrainFoliageCapability::VertexWind)
                return present(TerrainFoliageCapability::RenderExtraction) && present(TerrainFoliageCapability::FoliageRuntime) &&
                       present(TerrainFoliageCapability::TerrainRuntime);
            return true;
        }
    }  // namespace

    /** @copydoc GetTerrainProductProfilePolicy */
    Result<TerrainProductProfilePolicy> GetTerrainProductProfilePolicy(const TerrainProductProfile profile) {
        using enum Requirement;
        switch (profile) {
            case TerrainProductProfile::Null:
            case TerrainProductProfile::Unsupported:
                return Result<TerrainProductProfilePolicy>::Success(
                    Policy(profile,
                           {Omitted, Omitted, Omitted, Omitted, Omitted, Omitted, Omitted, Omitted, Omitted, Omitted, Omitted, Omitted}));
            case TerrainProductProfile::Headless:
                return Result<TerrainProductProfilePolicy>::Success(
                    Policy(profile, {Required, Required, Required, Required, Omitted, Optional, Optional, Omitted, Omitted, Omitted,
                                     Omitted, Omitted}));
            case TerrainProductProfile::Editor:
                return Result<TerrainProductProfilePolicy>::Success(
                    Policy(profile, {Required, Required, Optional, Optional, Optional, Optional, Optional, Optional, Omitted, Optional,
                                     Required, Required}));
            case TerrainProductProfile::Runtime:
                return Result<TerrainProductProfilePolicy>::Success(
                    Policy(profile, {Required, Required, Required, Required, Required, Optional, Optional, Optional, Omitted, Optional,
                                     Omitted, Omitted}));
            case TerrainProductProfile::Count:
                break;
        }
        return Result<TerrainProductProfilePolicy>::Failure(MakeError(TerrainErrors::CompositionInvalid));
    }

    /** @copydoc TerrainComposition::Create */
    Result<TerrainComposition> TerrainComposition::Create(const TerrainCompositionRequest &request) {
        if (request.contractVersion != CurrentTerrainCompositionContractVersion || !request.revision.IsValid())
            return Result<TerrainComposition>::Failure(MakeError(TerrainErrors::CompositionInvalid));
        auto policy = GetTerrainProductProfilePolicy(request.profile);
        if (policy.HasError())
            return Result<TerrainComposition>::Failure(policy.ErrorValue());
        auto facts = OrderFacts(request.capabilities);
        if (facts.HasError())
            return Result<TerrainComposition>::Failure(facts.ErrorValue());

        std::array<TerrainCapabilityDecision, TerrainCompositionCapabilityCount> decisions{};
        for (std::size_t index = 0; index < decisions.size(); ++index) {
            const auto &fact = facts.Value()[index];
            const Requirement requirement = policy.Value().requirements[index];
            const bool available = EffectiveAvailable(facts.Value(), fact.capability);
            if (requirement == Requirement::Required && !available)
                return Result<TerrainComposition>::Failure(MakeError(TerrainErrors::CapabilityUnsupported));
            const TerrainCapabilityState state = requirement == Requirement::Omitted ? TerrainCapabilityState::Omitted
                                                 : available                         ? TerrainCapabilityState::Bound
                                                                                     : TerrainCapabilityState::Unavailable;
            decisions[index] = {fact.capability, requirement, state,
                                state == TerrainCapabilityState::Bound ? fact.revision : TerrainHostCapabilityRevision{}};
        }
        return Result<TerrainComposition>::Success(TerrainComposition{policy.Value(), request.revision, decisions});
    }

    /** @copydoc TerrainComposition::Replace */
    Result<TerrainComposition> TerrainComposition::Replace(const TerrainComposition &current,
                                                           const TerrainCapabilityRevision expectedCurrent,
                                                           const TerrainCompositionRequest &request) {
        if (!expectedCurrent.IsValid())
            return Result<TerrainComposition>::Failure(MakeError(TerrainErrors::CompositionInvalid));
        if (current.Revision() != expectedCurrent || !request.revision.IsValid() || request.revision.Value() <= current.Revision().Value())
            return Result<TerrainComposition>::Failure(MakeError(TerrainErrors::RevisionStale));
        return Create(request);
    }

    TerrainComposition::TerrainComposition(TerrainProductProfilePolicy policy, const TerrainCapabilityRevision revision,
                                           std::array<TerrainCapabilityDecision, TerrainCompositionCapabilityCount> decisions) noexcept
        : policy_(std::move(policy)), revision_(revision), decisions_(std::move(decisions)) {}

    /** @copydoc TerrainComposition::Policy */
    const TerrainProductProfilePolicy &TerrainComposition::Policy() const noexcept {
        return policy_;
    }

    /** @copydoc TerrainComposition::Revision */
    TerrainCapabilityRevision TerrainComposition::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc TerrainComposition::Capabilities */
    const std::array<TerrainCapabilityDecision, TerrainCompositionCapabilityCount> &TerrainComposition::Capabilities() const noexcept {
        return decisions_;
    }

    /** @copydoc TerrainComposition::Resolve */
    Result<TerrainCapabilityDecision> TerrainComposition::Resolve(const TerrainFoliageCapability capability) const {
        if (!IsKnown(capability))
            return Result<TerrainCapabilityDecision>::Failure(MakeError(TerrainErrors::CompositionInvalid));
        return Result<TerrainCapabilityDecision>::Success(decisions_[static_cast<std::size_t>(capability)]);
    }

    /** @copydoc ValidateTerrainCompositionAdmission */
    Result<void> ValidateTerrainCompositionAdmission(const TerrainComposition &composition, const TerrainCapabilityRevision currentRevision,
                                                     const TerrainCompositionLifecycle lifecycle,
                                                     const TerrainFoliageCapabilitySet required) {
        if (!currentRevision.IsValid() || !IsKnown(lifecycle) || !required.IsValid())
            return Result<void>::Failure(MakeError(TerrainErrors::CompositionInvalid));
        if (composition.Revision() != currentRevision)
            return Result<void>::Failure(MakeError(TerrainErrors::RevisionStale));
        if (lifecycle == TerrainCompositionLifecycle::Cancelling)
            return Result<void>::Failure(MakeError(TerrainErrors::CompositionCancelled));
        if (lifecycle == TerrainCompositionLifecycle::ShuttingDown || lifecycle == TerrainCompositionLifecycle::Closed)
            return Result<void>::Failure(MakeError(TerrainErrors::LifecycleUnavailable));
        if (composition.Policy().profile == TerrainProductProfile::Unsupported)
            return Result<void>::Failure(MakeError(TerrainErrors::CompositionProfileUnsupported));
        for (std::size_t index = 0; index < TerrainCompositionCapabilityCount; ++index) {
            const auto capability = static_cast<TerrainFoliageCapability>(index);
            if (required.Contains(capability) && composition.Capabilities()[index].state != TerrainCapabilityState::Bound)
                return Result<void>::Failure(MakeError(TerrainErrors::CapabilityUnsupported));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Terrain
