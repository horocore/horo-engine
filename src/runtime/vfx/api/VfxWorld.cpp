#include "Horo/Vfx/VfxWorld.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::Vfx {
    namespace {
        /** @brief Computes canonical cell differences before conversion, preserving nearby positions at large global coordinates. */
        float RelativeAxis(Math::WorldCoordinateAxis64 simulation, Math::WorldCoordinateAxis64 render, float local) noexcept {
            const auto cells = simulation.cellIndex - render.cellIndex;
            const auto offset = static_cast<double>(simulation.offsetMillimeters) - render.offsetMillimeters;
            return static_cast<float>(static_cast<double>(cells) * 1024.0 + offset / 1000.0 + local);
        }

        /** @brief Rejects incompatible cooked material, topology and scene identities before activation. */
        bool ValidBinding(const VfxEmitterPreparation &binding, VfxIdentityScope scene) noexcept {
            using enum Render::VfxParticlePass;
            const bool solid = binding.blend == Opaque || binding.blend == Masked;
            return binding.effect.IsValid() && binding.effect.scope == scene && binding.emitter.IsValid() &&
                   binding.emitter.scope == scene && binding.particles.buffer.IsValid() && binding.particles.buffer.scope == scene &&
                   binding.material.IsValid() && binding.blend < Count && binding.topology < Render::VfxPrimitiveTopology::Count &&
                   static_cast<unsigned>(binding.blend) == static_cast<unsigned>(binding.materialBlend) &&
                   (!solid || binding.topology == Render::VfxPrimitiveTopology::InstancedMesh) &&
                   (binding.topology != Render::VfxPrimitiveTopology::InstancedMesh || binding.mesh.IsValid()) &&
                   (!binding.shadowCaster || solid);
        }

        /** @brief Reserves maximum recipe depth/color/shadow multiplicity at extraction admission. */
        std::uint32_t DrawCost(const VfxEmitterPreparation &binding, std::uint32_t count) noexcept {
            using enum Render::VfxParticlePass;
            std::uint32_t draws = 1;
            if (binding.blend == Opaque || binding.blend == Masked)
                draws = 2;
            else if (binding.blend == Translucent)
                draws = count;
            return draws + (binding.shadowCaster ? 1U : 0U);
        }

        /** @brief Captures scene/effect/emitter generations without a pointer to live storage. */
        Render::VfxBatchId BatchId(const VfxEmitterPreparation &binding) noexcept {
            return {binding.effect.scope.Value(), binding.effect.slot, binding.effect.generation, binding.emitter.slot,
                    binding.emitter.generation};
        }

        /** @brief Keeps overload evidence bounded while accumulating every omitted complete batch. */
        void Omit(VfxExtractionReport &report, const VfxEmitterPreparation &binding, std::uint32_t count) noexcept {
            if (report.omittedBatches == 0)
                report.firstOmitted = BatchId(binding);
            ++report.omittedBatches;
            report.omittedInstances += count;
            report.status = VfxExtractionStatus::BudgetExceeded;
        }
    }  // namespace

    /** @copydoc VfxRenderExtractor::Prepare */
    Result<VfxRenderExtractor> VfxRenderExtractor::Prepare(const VfxExtractionBudget budget) {
        constexpr std::uint32_t MaxBatches = 4'096;
        if (constexpr std::uint32_t MaxFrameSlots = 16;
            budget.maximumBatches == 0 || budget.maximumBatches > MaxBatches || budget.maximumInstances == 0 ||
            budget.maximumInstances > CpuParticleBufferHardLimits::Particles || budget.maximumDrawCalls == 0 ||
            budget.maximumDrawCalls > MaxBatches * 3U || budget.frameSlots == 0 || budget.frameSlots > MaxFrameSlots)
            return Result<VfxRenderExtractor>::Failure(MakeError(VfxErrors::LimitExceeded));
        if (const auto bytes = static_cast<std::uint64_t>(budget.frameSlots) *
                               (static_cast<std::uint64_t>(budget.maximumBatches) * sizeof(Render::VfxRenderBatch) +
                                static_cast<std::uint64_t>(budget.maximumInstances) * sizeof(Render::VfxRenderInstance) +
                                sizeof(Render::RenderWorldFrameData));
            bytes > budget.maximumBytes || budget.maximumBytes > CpuParticleBufferHardLimits::Bytes)
            return Result<VfxRenderExtractor>::Failure(MakeError(VfxErrors::LimitExceeded));
        try {
            VfxRenderExtractor extractor;
            extractor.budget_ = budget;
            extractor.owner_ = std::this_thread::get_id();
            extractor.slots_.reserve(budget.frameSlots);
            for (std::uint32_t slot = 0; slot < budget.frameSlots; ++slot) {
                auto frame = std::make_shared<Render::RenderWorldFrameData>();
                frame->batches_.reserve(budget.maximumBatches);
                frame->instances_.reserve(budget.maximumInstances);
                extractor.slots_.push_back(std::move(frame));
            }
            return Result<VfxRenderExtractor>::Success(std::move(extractor));
        } catch (const std::bad_alloc &) {
            return Result<VfxRenderExtractor>::Failure(MakeError(VfxErrors::ParticleBufferAllocationFailed));
        }
    }

    /** @copydoc VfxWorld::VfxWorld */
    VfxWorld::VfxWorld(VfxIdentityScope scene, VfxRenderExtractor extractor)
        : scene_(scene), extractor_(std::move(extractor)), owner_(std::this_thread::get_id()) {}

    /** @copydoc VfxWorld::Prepare */
    Result<std::unique_ptr<VfxWorld>> VfxWorld::Prepare(VfxIdentityScope scene, std::span<const VfxEmitterPreparation> bindings,
                                                        VfxExtractionBudget budget) {
        if (!scene.IsValid() || bindings.empty() || bindings.size() > 4'096)
            return Result<std::unique_ptr<VfxWorld>>::Failure(MakeError(VfxErrors::IdentityInvalid));
        std::uint64_t particles{};
        for (const auto &binding : bindings)
            particles += binding.particles.capacity;
        if (particles > CpuParticleBufferHardLimits::Particles)
            return Result<std::unique_ptr<VfxWorld>>::Failure(MakeError(VfxErrors::LimitExceeded));
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            if (!ValidBinding(bindings[index], scene))
                return Result<std::unique_ptr<VfxWorld>>::Failure(MakeError(VfxErrors::ParticleModeIncompatible));
            for (std::size_t previous = 0; previous < index; ++previous) {
                const auto &left = bindings[previous];
                const auto &right = bindings[index];
                if (left.emitter.slot == right.emitter.slot || left.particles.buffer.slot == right.particles.buffer.slot ||
                    (left.effect.slot == right.effect.slot && left.effect != right.effect))
                    return Result<std::unique_ptr<VfxWorld>>::Failure(MakeError(VfxErrors::IdentityInvalid));
            }
        }
        auto extractor = VfxRenderExtractor::Prepare(budget);
        if (extractor.HasError())
            return Result<std::unique_ptr<VfxWorld>>::Failure(extractor.ErrorValue());
        try {
            auto world = std::make_unique<VfxWorld>(VfxWorld{scene, std::move(extractor).Value()});
            world->emitters_.reserve(bindings.size());
            for (const auto &binding : bindings) {
                auto particleBuffer = CpuParticleBuffer::Create(binding.particles);
                if (particleBuffer.HasError())
                    return Result<std::unique_ptr<VfxWorld>>::Failure(particleBuffer.ErrorValue());
                world->emitters_.emplace_back(binding, std::move(particleBuffer).Value(), false);
            }
            return Result<std::unique_ptr<VfxWorld>>::Success(std::move(world));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<VfxWorld>>::Failure(MakeError(VfxErrors::ParticleBufferAllocationFailed));
        }
    }

    /** @copydoc VfxWorld::Play */
    Result<void> VfxWorld::Play(EffectSystemId effect) {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(VfxErrors::ParticleBufferThreadViolation));
        if (shutDown_)
            return Result<void>::Failure(MakeError(VfxErrors::ParticleBufferShutDown));
        bool found = false;
        for (const auto &emitter : emitters_) {
            if (emitter.binding.effect == effect) {
                if (emitter.retired)
                    return Result<void>::Failure(MakeError(VfxErrors::GenerationExhausted));
                if (emitter.live)
                    return Result<void>::Failure(MakeError(VfxErrors::IdentityInvalid));
                found = true;
            }
        }
        if (!found)
            return Result<void>::Failure(MakeError(VfxErrors::IdentityInvalid));
        for (auto &emitter : emitters_)
            if (emitter.binding.effect == effect)
                emitter.live = true;
        return Result<void>::Success();
    }

    /** @copydoc VfxWorld::Stop */
    Result<EffectSystemId> VfxWorld::Stop(EffectSystemId effect) {
        if (owner_ != std::this_thread::get_id())
            return Result<EffectSystemId>::Failure(MakeError(VfxErrors::ParticleBufferThreadViolation));
        if (shutDown_)
            return Result<EffectSystemId>::Failure(MakeError(VfxErrors::IdentityInvalid));
        if (const bool found = std::ranges::any_of(emitters_,
                                                   [effect](const Emitter &emitter) {
            return emitter.live && emitter.binding.effect == effect;
        });
            !found)
            return Result<EffectSystemId>::Failure(MakeError(VfxErrors::IdentityInvalid));
        auto next = effect;
        const bool exhausted = effect.generation == std::numeric_limits<std::uint32_t>::max();
        if (!exhausted)
            ++next.generation;
        for (auto &emitter : emitters_) {
            if (emitter.binding.effect == effect) {
                if (auto cleared = emitter.particles.Clear(); cleared.HasError())
                    return Result<EffectSystemId>::Failure(cleared.ErrorValue());
                emitter.live = false;
                emitter.retired = exhausted;
                emitter.binding.effect = next;
            }
        }
        if (exhausted)
            return Result<EffectSystemId>::Failure(MakeError(VfxErrors::GenerationExhausted));
        return Result<EffectSystemId>::Success(next);
    }

    /** @copydoc VfxWorld::Particles */
    Result<CpuParticleBuffer *> VfxWorld::Particles(EffectSystemId effect, EmitterId emitter) {
        if (owner_ != std::this_thread::get_id())
            return Result<CpuParticleBuffer *>::Failure(MakeError(VfxErrors::ParticleBufferThreadViolation));
        for (auto &entry : emitters_)
            if (!shutDown_ && entry.live && entry.binding.effect == effect && entry.binding.emitter == emitter)
                return Result<CpuParticleBuffer *>::Success(&entry.particles);
        return Result<CpuParticleBuffer *>::Failure(MakeError(VfxErrors::IdentityInvalid));
    }

    /** @copydoc VfxWorld::Extract */
    VfxExtractionOutcome VfxWorld::Extract(std::uint64_t frame, const Math::WorldCoordinate64 &origin, std::uint64_t revision) noexcept {
        return extractor_.Extract(*this, frame, origin, revision);
    }

    /** @copydoc VfxWorld::Shutdown */
    Result<void> VfxWorld::Shutdown() {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(VfxErrors::ParticleBufferThreadViolation));
        for (auto &emitter : emitters_) {
            if (auto closed = emitter.particles.Shutdown(); closed.HasError())
                return closed;
            emitter.live = false;
        }
        shutDown_ = true;
        return Result<void>::Success();
    }

    /** @copydoc VfxRenderExtractor::CopyParticles */
    bool VfxRenderExtractor::CopyParticles(const VfxEmitterPreparation &binding, const CpuParticleBuffer &particles,
                                           const CpuParticleSoAConstView &source, Render::RenderWorldFrameData &data,
                                           const Math::WorldCoordinate64 &origin) noexcept {
        const auto count = static_cast<std::uint32_t>(source.positionX.size());
        for (std::uint32_t particle = 0; particle < count; ++particle) {
            const Math::Vec3 position{source.positionX[particle], source.positionY[particle], source.positionZ[particle]};
            const auto &simulationOrigin = binding.simulationOrigin;
            const Math::Vec3 relative{RelativeAxis(simulationOrigin.X(), origin.X(), position.x),
                                      RelativeAxis(simulationOrigin.Y(), origin.Y(), position.y),
                                      RelativeAxis(simulationOrigin.Z(), origin.Z(), position.z)};
            if (!Math::IsFinite(relative) || !std::isfinite(source.sizeX[particle]) || !std::isfinite(source.sizeY[particle]) ||
                source.sizeX[particle] < 0 || source.sizeY[particle] < 0 || !std::isfinite(source.rotation[particle])) {
                return false;
            }
            CpuParticleHandle identity;
            if (particles.TryHandleAt(particle, identity) != CpuParticleReadStatus::Complete) {
                return false;
            }
            data.instances_.emplace_back(identity.particle.Value(), relative, Math::Vec2{source.sizeX[particle], source.sizeY[particle]},
                                         source.rotation[particle], source.packedColor[particle]);
        }
        return true;
    }

    /** @copydoc VfxRenderExtractor::AppendEmitter */
    bool VfxRenderExtractor::AppendEmitter(const VfxEmitterPreparation &binding, const CpuParticleBuffer &particles,
                                           Render::RenderWorldFrameData &data, const Math::WorldCoordinate64 &origin,
                                           VfxExtractionReport &report) const noexcept {
        CpuParticleSoAConstView source;
        if (particles.TryReadView(source) != CpuParticleReadStatus::Complete) {
            report.status = VfxExtractionStatus::InvalidParticleData;
            return false;
        }
        const auto count = static_cast<std::uint32_t>(source.positionX.size());
        if (count == 0)
            return true;
        const auto draws = DrawCost(binding, count);
        if (report.admittedBatches == budget_.maximumBatches || count > budget_.maximumInstances - report.admittedInstances ||
            draws > budget_.maximumDrawCalls - report.reservedDrawCalls) {
            Omit(report, binding, count);
            return true;
        }
        const auto first = static_cast<std::uint32_t>(data.instances_.size());
        if (!CopyParticles(binding, particles, source, data, origin)) {
            report.status = VfxExtractionStatus::InvalidParticleData;
            return false;
        }
        data.batches_.emplace_back(BatchId(binding), binding.material, binding.mesh, binding.blend, binding.topology,
                                   binding.deferredCompatible, binding.shadowCaster, first, count);
        ++report.admittedBatches;
        report.admittedInstances += count;
        report.reservedDrawCalls += draws;
        return true;
    }

    /** @copydoc VfxRenderExtractor::Extract */
    VfxExtractionOutcome VfxRenderExtractor::Extract(const VfxWorld &world, std::uint64_t frame, const Math::WorldCoordinate64 &origin,
                                                     std::uint64_t revision) noexcept {
        VfxExtractionOutcome outcome;
        auto &report = outcome.report;
        if (owner_ != std::this_thread::get_id() || world.owner_ != owner_) {
            report.status = VfxExtractionStatus::ThreadViolation;
            return outcome;
        }
        if (world.shutDown_) {
            report.status = VfxExtractionStatus::ShutDown;
            return outcome;
        }
        if (frame == 0 || frame <= lastFrame_ || revision == 0) {
            report.status = VfxExtractionStatus::InvalidFrame;
            return outcome;
        }
        const auto available = std::ranges::find_if(slots_, [](const auto &slot) {
            return slot.use_count() == 1;
        });
        if (available == slots_.end()) {
            report.status = VfxExtractionStatus::FramesRetained;
            return outcome;
        }
        auto &data = **available;
        data.batches_.clear();
        data.instances_.clear();
        data.scene_ = world.scene_.Value();
        data.frame_ = frame;
        data.originRevision_ = revision;
        for (const auto &emitter : world.emitters_) {
            if (!emitter.live)
                continue;
            if (!AppendEmitter(emitter.binding, emitter.particles, data, origin, report))
                return outcome;
        }
        lastFrame_ = frame;
        outcome.snapshot.data_ = *available;
        return outcome;
    }
}  // namespace Horo::Vfx
