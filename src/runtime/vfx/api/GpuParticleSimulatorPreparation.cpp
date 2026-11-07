#include "GpuParticleSimulatorInternal.h"
#include "Horo/Runtime/Render/VfxGpuKernel.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace Horo::Vfx::GpuParticleDetail {
    namespace {
        /** @brief Checks every authored source motion requirement independently from the captured complete-unit feature mask. */
        bool MotionSubset(const ParticleSystemDescriptorData &d) noexcept {
            return d.spawnRate.minimum == 0 && d.spawnRate.maximum == 0 && d.collisionMode == ParticleCollisionMode::None &&
                   d.lifetimeKind == ParticleLifetimeKind::Finite && d.killCondition == ParticleKillCondition::Lifetime;
        }

        /** @brief Rejects unimplemented source behavior instead of silently narrowing a complete emitter. */
        bool SupportedSource(const ParticleSystemDescriptorData &d, const GpuParticleSimulatorCreateInfo &info) noexcept {
            if (info.initialization != GpuParticleInitialization::PreparedParticles ||
                info.resolution.domain != ResolvedSimulationDomain::GPU || info.requiredFeatures != GpuParticleRequiredFeature::None)
                return false;
            return d.simulationPreference != SimulationPreference::RequireCPU && MotionSubset(d) &&
                   d.renderMode == ParticleRenderMode::Billboard && d.sortMode == ParticleSortMode::None;
        }

        /** @brief Checks seed representation before any source storage is allocated. */
        bool SeedShape(const GpuParticleInitialState &initial, std::uint32_t capacity) noexcept {
            const auto count = initial.identities.size();
            if (!count || count > capacity || initial.attributes.customFloatStreamCount > 16)
                return false;
            const auto &a = initial.attributes;
            const std::array streams{a.positionX, a.positionY, a.positionZ, a.velocityX,       a.velocityY, a.velocityZ,
                                     a.sizeX,     a.sizeY,     a.rotation,  a.angularVelocity, a.age,       a.maximumAge};
            if (!std::ranges::all_of(streams, [count](auto stream) {
                return stream.size() == count;
            }))
                return false;
            if (a.packedColor.size() != count || a.customFlags.size() != count)
                return false;
            for (std::uint32_t i = 0; i < a.customFloatStreamCount; ++i)
                if (a.customFloats[i].size() != count)
                    return false;
            return true;
        }

        /** @brief Validates scene ownership and exact GPU resolution without creating a capability or choosing a backend. */
        bool OwnerIdentities(const ParticleSystemDescriptorData &d, const GpuParticleSimulatorCreateInfo &i) noexcept {
            return i.scene.IsValid() && i.activation.IsValid() && i.buffer.IsValid() && d.emitter.IsValid() &&
                   i.activation.scope == i.scene && i.buffer.scope == i.scene && d.emitter.scope == i.scene;
        }

        /** @brief Keeps admission within the actual resolver count/memory projection and explicit owner-supplied ledger slice. */
        bool Budget(const ParticleCookPlan &plan, const GpuParticleSimulatorCreateInfo &i) noexcept {
            return i.resolution.selectedCount && i.resolution.selectedCount <= plan.maximumParticles &&
                   i.resolution.capabilityRevision.IsValid() && i.resolution.policyRevision.IsValid() && i.maximumRetainedBytes &&
                   i.maximumRetainedBytes <= i.resolution.selectedMemoryBytes && i.maximumPendingSteps && i.maximumPendingSteps <= 64;
        }

        /** @brief Validates complete logical seed admission independently from allocation and source feature selection. */
        bool ValidSeedAdmission(const ParticleCookAdmission &admission, const GpuParticleSimulatorCreateInfo &i) noexcept {
            return OwnerIdentities(admission.Descriptor().Data(), i) && Budget(admission.Plan(), i) &&
                   SeedShape(i.initial, i.resolution.selectedCount) && ValidateParticleForceModules(i.forces);
        }

        /** @brief Adds a known requested payload using subtraction bounds before multiplication. */
        bool Charge(std::uint64_t &used, std::size_t count, std::size_t stride, std::uint64_t limit) noexcept {
            if (used > limit || count > (limit - used) / stride)
                return false;
            used += std::uint64_t{count} * stride;
            return true;
        }

        /** @brief Accounts distinct logical copies and fixed/cancellation payload alongside the renderer's separate backing slice. */
        bool LogicalBudget(const GpuParticleSimulatorCreateInfo &i, std::size_t initialCapacity, std::size_t forceCapacity,
                           std::size_t pendingCapacity) noexcept {
            const auto limit = i.resolution.selectedMemoryBytes;
            std::uint64_t used =
                sizeof(Detail::GpuParticleSimulatorState) + sizeof(GpuParticleSimulator) + CancellationSource::StatePayloadBytes();
            if (!Charge(used, initialCapacity, sizeof(std::uint32_t), limit) ||
                !Charge(used, forceCapacity, sizeof(std::uint32_t), limit) ||
                !Charge(used, pendingCapacity, sizeof(Detail::GpuParticlePending), limit))
                return false;
            return i.maximumRetainedBytes <= limit - used;
        }

        /** @brief Projects an already validated logical identity to the shared value-only representation. */
        template <typename Identity> ParticleRandomIdentity IdentityValue(const Identity &identity) noexcept {
            return {identity.scope.Value(), identity.slot, identity.generation};
        }

        /** @brief Builds scalar cold metadata without reading native state or retaining caller spans. */
        Render::VfxGpuSimulationDescriptor Descriptor(const ParticleCookAdmission &admission, const GpuParticleSimulatorCreateInfo &i,
                                                      const Render::GpuParticleLayout &layout) {
            const auto &source = admission.Descriptor().Data();
            Render::VfxGpuSimulationDescriptor d;
            d.identity = {i.scene.Value(), IdentityValue(i.activation), IdentityValue(source.emitter), IdentityValue(i.buffer)};
            d.layout = layout;
            d.random = {i.effectSeed, IdentityValue(i.activation), IdentityValue(source.emitter)};
            d.simulationOrigin = i.simulationOrigin;
            d.initialParticleCount = static_cast<std::uint32_t>(i.initial.identities.size());
            d.forceCount = static_cast<std::uint32_t>(i.forces.size());
            d.maximumPendingSteps = i.maximumPendingSteps;
            d.maximumRetainedBytes = i.maximumRetainedBytes;
            d.capabilityRevision = i.resolution.capabilityRevision.Value();
            d.policyRevision = i.resolution.policyRevision.Value();
            d.arithmetic.maximumSteps = i.maximumSteps;
            d.arithmetic.maximumDeltaSeconds = i.maximumDeltaSeconds;
            d.readback = admission.Plan().gpuReadback;
            return d;
        }

        /** @brief Writes one numeric word using the shared checked layout, never a CPU address or padded object representation. */
        void Store(Detail::GpuParticleSimulatorState &state, std::uint32_t stream, std::uint32_t slot, std::uint32_t word) noexcept {
            const auto stride = state.preparation.descriptor.layout.streamStrideWords;
            state.initialWords[std::size_t{stream} * stride + slot] = word;
        }

        /** @brief Copies all canonical seed float streams once at cold initialization. */
        void SeedFloats(Detail::GpuParticleSimulatorState &s, const GpuParticleInitialState &initial) noexcept {
            const auto &a = initial.attributes;
            const std::array streams{a.positionX, a.positionY, a.positionZ, a.velocityX,       a.velocityY, a.velocityZ,
                                     a.sizeX,     a.sizeY,     a.rotation,  a.angularVelocity, a.age,       a.maximumAge};
            for (std::uint32_t stream = 0; stream < streams.size(); ++stream)
                for (std::uint32_t slot = 0; slot < initial.identities.size(); ++slot)
                    Store(s, stream, slot, std::bit_cast<std::uint32_t>(streams[stream][slot]));
            for (std::uint32_t stream = 0; stream < a.customFloatStreamCount; ++stream)
                for (std::uint32_t slot = 0; slot < initial.identities.size(); ++slot)
                    Store(s, stream + 18, slot, std::bit_cast<std::uint32_t>(a.customFloats[stream][slot]));
        }

        /** @brief Copies stable identities and semantic flags; occupied seed lanes have explicit generation one. */
        void SeedIdentities(Detail::GpuParticleSimulatorState &s, const GpuParticleInitialState &initial) noexcept {
            for (std::uint32_t slot = 0; slot < initial.identities.size(); ++slot) {
                const auto id = initial.identities[slot].Value();
                Store(s, 12, slot, initial.attributes.packedColor[slot]);
                Store(s, 13, slot, initial.attributes.customFlags[slot]);
                Store(s, 14, slot, static_cast<std::uint32_t>(id));
                Store(s, 15, slot, static_cast<std::uint32_t>(id >> 32));
                Store(s, 16, slot, 1);
                Store(s, 17, slot, 1);
            }
        }

        /** @brief Computes canonical arithmetic/content evidence from actual words, then applies the same frontend cold validator. */
        Result<void> Seal(Detail::GpuParticleSimulatorState &s) {
            s.preparation.initialWords = s.initialWords;
            s.preparation.forceWords = s.forceWords;
            auto envelope = Render::ComputeVfxGpuArithmeticEnvelope(s.preparation);
            if (envelope.HasError())
                return Result<void>::Failure(envelope.ErrorValue());
            s.preparation.descriptor.arithmetic = envelope.Value();
            s.preparation.descriptor.initializationDigest = Render::ComputeVfxGpuInitializationDigest(s.preparation);
            return Render::ValidateVfxGpuSimulationPreparation(s.preparation);
        }

        /** @brief Materializes fixed source/ack storage before the unit can become active. */
        Result<std::unique_ptr<Detail::GpuParticleSimulatorState>> Allocate(const ParticleCookAdmission &admission,
                                                                            const GpuParticleSimulatorCreateInfo &i,
                                                                            const Render::GpuParticleLayout &layout) {
            auto s = std::make_unique<Detail::GpuParticleSimulatorState>();
            s->preparation.descriptor = Descriptor(admission, i, layout);
            s->initialWords.resize(layout.wordCount);
            s->forceWords.resize(i.forces.size() * Render::GpuParticleForceWords::Count);
            s->submitted.resize(i.maximumPendingSteps);
            if (!LogicalBudget(i, s->initialWords.capacity(), s->forceWords.capacity(), s->submitted.capacity()))
                return Result<std::unique_ptr<Detail::GpuParticleSimulatorState>>::Failure(MakeError(VfxErrors::GpuSimulationInvalid));
            SeedFloats(*s, i.initial);
            SeedIdentities(*s, i.initial);
            auto packed = Render::PackGpuParticleForces(i.forces, s->forceWords);
            if (packed.HasError())
                return Result<std::unique_ptr<Detail::GpuParticleSimulatorState>>::Failure(packed.ErrorValue());
            if (auto sealed = Seal(*s); sealed.HasError())
                return Result<std::unique_ptr<Detail::GpuParticleSimulatorState>>::Failure(sealed.ErrorValue());
            return Result<std::unique_ptr<Detail::GpuParticleSimulatorState>>::Success(std::move(s));
        }
    }  // namespace

    Result<std::unique_ptr<Detail::GpuParticleSimulatorState>> PrepareGpuState(const ParticleCookAdmission &admission,
                                                                               const GpuParticleSimulatorCreateInfo &i) {
        using Outcome = Result<std::unique_ptr<Detail::GpuParticleSimulatorState>>;
        const auto &d = admission.Descriptor().Data();
        if (!SupportedSource(d, i) || (d.gpuReadback.enabled && i.initial.attributes.customFloatStreamCount))
            return Outcome::Failure(MakeError(VfxErrors::GpuSimulationUnsupported));
        if (!ValidSeedAdmission(admission, i))
            return Outcome::Failure(MakeError(VfxErrors::GpuSimulationInvalid));
        const auto layout =
            Render::BuildGpuParticleLayout(i.resolution.selectedCount, i.initial.attributes.customFloatStreamCount, i.layoutLimits);
        if (layout.HasError())
            return Outcome::Failure(layout.ErrorValue());
        if (!LogicalBudget(i, layout.Value().wordCount, i.forces.size() * Render::GpuParticleForceWords::Count, i.maximumPendingSteps))
            return Outcome::Failure(MakeError(VfxErrors::GpuSimulationInvalid));
        try {
            return Allocate(admission, i, layout.Value());
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(VfxErrors::GpuSimulationAllocationFailed));
        }
    }
}  // namespace Horo::Vfx::GpuParticleDetail
