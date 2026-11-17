#include "Horo/Runtime/Render/VfxGpuKernel.h"
#include "VfxGpuSimulationInternal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <tuple>

namespace Horo::Render::VfxGpuSimulationErrors {
    const ErrorCodeDescriptor InvalidPreparation{.domain = ErrorDomainId{"horo.render.vfx_gpu"},
                                                 .code = ErrorCode{"render.vfx_gpu.preparation_invalid"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "GPU particle preparation does not match its bounded semantic word ABI.",
                                                 .remediationHint = "Rebuild the complete declared unit, stable seed identities, force "
                                                                    "words and shared ledger reservation before activation."};
    const ErrorCodeDescriptor
        ArithmeticUnsafe{.domain = ErrorDomainId{"horo.render.vfx_gpu"},
                         .code = ErrorCode{"render.vfx_gpu.arithmetic_unsafe"},
                         .defaultSeverity = ErrorSeverity::Error,
                         .summary = "GPU particle arithmetic cannot be proved finite over the admitted step envelope.",
                         .remediationHint =
                             "Reduce the authored seed, force, step-count or delta envelope, or select the explicitly admitted CPU unit."};
}  // namespace Horo::Render::VfxGpuSimulationErrors

namespace Horo::Render::VfxGpuDetail {
    namespace {
        /** @brief Checks one scene-issued VFX identity without confusing its zero-based slot with renderer handles. */
        bool Valid(const Vfx::ParticleRandomIdentity &id) noexcept {
            return id.scope && id.slot != std::numeric_limits<std::uint32_t>::max() && id.generation;
        }

        /** @brief Compares scalar identity lanes, not host padding or native slot assignment. */
        bool Same(const Vfx::ParticleRandomIdentity &a, const Vfx::ParticleRandomIdentity &b) noexcept {
            return a.scope == b.scope && a.slot == b.slot && a.generation == b.generation;
        }

        /** @brief Checks enabled schema payload and result bounds without conflating optional capability admission. */
        bool ReadbackLimits(const VfxGpuReadbackDescriptor &r, std::uint32_t recordCapacity, std::uint32_t customFloatStreams) noexcept {
            const auto bytes = std::uint64_t{r.maximumRecords} * VfxGpuObservationWords::Bytes;
            return !customFloatStreams && r.maximumRecords && r.maximumRecords <= recordCapacity && bytes <= r.maximumBytesPerResult &&
                   r.maximumPendingResults && r.maximumPendingResults <= 64;
        }

        /** @brief Checks exact bounded observation declaration without allocating or querying hardware. */
        bool ValidReadback(const VfxGpuSimulationDescriptor &d) noexcept {
            return IsValidVfxGpuReadbackPolicy(d.readback, d.initialParticleCount, d.layout.customFloatStreams);
        }

        /** @brief Bounds every descriptor-derived allocation product before native preparation consumes it. */
        bool ValidLedger(const VfxGpuSimulationDescriptor &d) noexcept {
            if (!d.maximumPendingSteps || d.maximumPendingSteps > 64)
                return false;
            const auto state = std::uint64_t{d.layout.byteCount} * VfxGpuStateBufferCount(d);
            const auto forces = std::max(std::uint64_t{48}, std::uint64_t{d.forceCount} * 48);
            const auto observations = d.readback.enabled ? std::uint64_t{d.readback.maximumRecords} * VfxGpuObservationWords::Bytes : 4;
            const auto readback = d.readback.enabled ? observations * d.readback.maximumPendingResults * 2 : 0;
            return state + forces + observations + readback <= d.maximumRetainedBytes;
        }

        /** @brief Checks finite nonnegative metadata before comparing recomputed arithmetic proof. */
        bool ValidEnvelope(const VfxGpuArithmeticEnvelope &e) noexcept {
            if (!e.maximumSteps || e.maximumSteps > 1'000'000 || !std::isfinite(e.maximumDeltaSeconds) || e.maximumDeltaSeconds <= 0 ||
                e.maximumDeltaSeconds > 60)
                return false;
            const std::array magnitudes{e.maximumPositionMagnitude, e.maximumVelocityMagnitude, e.maximumAccelerationMagnitude,
                                        e.maximumRotationMagnitude, e.maximumSizeMagnitude};
            return std::ranges::all_of(magnitudes, [](double v) {
                return std::isfinite(v) && v >= 0;
            });
        }
    }  // namespace

    bool ValidIdentity(const VfxGpuSimulationIdentity &id) noexcept {
        return id.scene && Valid(id.activation) && Valid(id.emitter) && Valid(id.buffer) && id.activation.scope == id.scene &&
               id.emitter.scope == id.scene && id.buffer.scope == id.scene;
    }

    /** @brief Checks immutable source representation before trusting its semantic metadata. */
    bool SourceShape(const VfxGpuSimulationDescriptor &d) noexcept {
        return ValidIdentity(d.identity) && d.layout.IsValid() && d.initialParticleCount && d.initialParticleCount <= d.layout.capacity &&
               d.forceCount <= Vfx::ParticleSimulationDataLimits::ForceModules;
    }

    /** @brief Binds random semantic identities and captured policy/capability generations to this source. */
    bool SourceKey(const VfxGpuSimulationDescriptor &d) noexcept {
        return Same(d.random.activation, d.identity.activation) && Same(d.random.emitter, d.identity.emitter) && d.capabilityRevision &&
               d.policyRevision && ValidEnvelope(d.arithmetic);
    }

    bool ValidDescriptor(const VfxGpuSimulationDescriptor &d) noexcept {
        return SourceShape(d) && SourceKey(d) && ValidReadback(d) && ValidLedger(d) &&
               std::ranges::any_of(d.initializationDigest.bytes, [](auto value) {
            return value != 0;
        });
    }

    bool SameEnvelope(const VfxGpuArithmeticEnvelope &a, const VfxGpuArithmeticEnvelope &b) noexcept {
        return a.maximumSteps == b.maximumSteps && a.maximumDeltaSeconds == b.maximumDeltaSeconds &&
               a.maximumPositionMagnitude == b.maximumPositionMagnitude && a.maximumVelocityMagnitude == b.maximumVelocityMagnitude &&
               a.maximumAccelerationMagnitude == b.maximumAccelerationMagnitude &&
               a.maximumRotationMagnitude == b.maximumRotationMagnitude && a.maximumSizeMagnitude == b.maximumSizeMagnitude;
    }
}  // namespace Horo::Render::VfxGpuDetail

namespace Horo::Render {
    /** @copydoc IsValidVfxGpuReadbackPolicy */
    bool IsValidVfxGpuReadbackPolicy(const VfxGpuReadbackDescriptor &descriptor, std::uint32_t recordCapacity,
                                     std::uint32_t customFloatStreams) noexcept {
        if (descriptor.schemaVersion != 1 || descriptor.purpose > VfxGpuReadbackPurpose::CosmeticObservation ||
            descriptor.requirement > VfxGpuReadbackRequirement::RequiredForVisualEffect || !descriptor.cadenceSteps)
            return false;
        if (!descriptor.enabled)
            return !descriptor.maximumRecords && !descriptor.maximumBytesPerResult && !descriptor.maximumPendingResults;
        return VfxGpuDetail::ReadbackLimits(descriptor, recordCapacity, customFloatStreams);
    }

    namespace {
        /** @brief Appends one canonical little-endian numeric lane without object padding. */
        void HashWord(Sha256Builder &hash, std::uint32_t value) noexcept {
            std::array<std::byte, 4> bytes;
            for (std::size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<std::byte>(value >> (i * 8));
            (void)hash.Update(bytes);
        }

        /** @brief Appends the specified low/high representation of a full 64-bit semantic value. */
        void HashWide(Sha256Builder &hash, std::uint64_t value) noexcept {
            HashWord(hash, static_cast<std::uint32_t>(value));
            HashWord(hash, static_cast<std::uint32_t>(value >> 32));
        }

        /** @brief Hashes every dimension of a stable activation/emitter/buffer identity. */
        void HashIdentity(Sha256Builder &hash, const Vfx::ParticleRandomIdentity &id) noexcept {
            HashWide(hash, id.scope);
            HashWord(hash, id.slot);
            HashWord(hash, id.generation);
        }

        /** @brief Hashes arithmetic metadata as explicit IEEE double/float bits and scalar bounds. */
        void HashArithmetic(Sha256Builder &hash, const VfxGpuArithmeticEnvelope &e) noexcept {
            HashWord(hash, e.maximumSteps);
            HashWord(hash, std::bit_cast<std::uint32_t>(e.maximumDeltaSeconds));
            for (auto v : {e.maximumPositionMagnitude, e.maximumVelocityMagnitude, e.maximumAccelerationMagnitude,
                           e.maximumRotationMagnitude, e.maximumSizeMagnitude})
                HashWide(hash, std::bit_cast<std::uint64_t>(v));
        }

        /** @brief Hashes optional schema, cadence and finite result bounds regardless of enabled state. */
        void HashReadback(Sha256Builder &hash, const VfxGpuReadbackDescriptor &r) noexcept {
            HashWord(hash, r.enabled);
            HashWord(hash, r.schemaVersion);
            HashWord(hash, static_cast<std::uint32_t>(r.purpose));
            HashWord(hash, static_cast<std::uint32_t>(r.requirement));
            HashWord(hash, r.cadenceSteps);
            HashWord(hash, r.maximumRecords);
            HashWord(hash, r.maximumBytesPerResult);
            HashWord(hash, r.maximumPendingResults);
        }

        /** @brief Hashes source identities and canonical origin lanes separately from the RNG key. */
        void HashSource(Sha256Builder &hash, const VfxGpuSimulationDescriptor &d) noexcept {
            HashWide(hash, d.identity.scene);
            HashIdentity(hash, d.identity.activation);
            HashIdentity(hash, d.identity.emitter);
            HashIdentity(hash, d.identity.buffer);
            HashWide(hash, d.random.effectSeed);
            HashIdentity(hash, d.random.activation);
            HashIdentity(hash, d.random.emitter);
            for (auto axis : {d.simulationOrigin.X(), d.simulationOrigin.Y(), d.simulationOrigin.Z()}) {
                HashWide(hash, static_cast<std::uint64_t>(axis.cellIndex));
                HashWord(hash, static_cast<std::uint32_t>(axis.offsetMillimeters));
            }
        }

        /** @brief Hashes every derived dimension and the actual shared layout/RNG schema versions. */
        void HashDimensions(Sha256Builder &hash, const VfxGpuSimulationDescriptor &d) noexcept {
            HashWord(hash, GpuParticleLayout::Version);
            HashWord(hash, Vfx::ParticleSimulationDataLimits::RandomAlgorithmVersion);
            for (auto value : {d.layout.capacity, d.layout.customFloatStreams, d.layout.streamStrideWords, d.layout.streamCount,
                               d.layout.wordCount, d.initialParticleCount, d.forceCount, d.maximumPendingSteps})
                HashWord(hash, value);
            HashWide(hash, d.layout.byteCount);
            HashWide(hash, d.maximumRetainedBytes);
            HashWide(hash, d.capabilityRevision);
            HashWide(hash, d.policyRevision);
        }

        /** @brief Validates issued-shaped candidate generation arithmetic without granting registry/native authority. */
        bool StepRepresentation(const VfxGpuStepIdentity &s) noexcept {
            return s.simulation.IsValid() && s.deviceIncarnation && s.sourceGeneration &&
                   s.sourceGeneration != std::numeric_limits<std::uint64_t>::max() && s.targetGeneration == s.sourceGeneration + 1;
        }

        /** @brief Checks one exact semantic step within its admitted arithmetic range. */
        bool StepBounds(const VfxGpuSimulationDescriptor &d, const GpuVfxSimulationWork &w) noexcept {
            return w.step.tick < d.arithmetic.maximumSteps && w.step.sourceGeneration == w.step.tick + 1 && std::isfinite(w.deltaSeconds) &&
                   w.deltaSeconds >= 0 && w.deltaSeconds <= d.arithmetic.maximumDeltaSeconds;
        }

        /** @brief Enforces one-based cadence; optional omission is represented separately by admission/observation diagnostics. */
        bool ObservationIntent(const VfxGpuSimulationDescriptor &d, const GpuVfxSimulationWork &w) noexcept {
            const bool due = d.readback.enabled && (w.step.tick + 1) % d.readback.cadenceSteps == 0;
            if (w.observe)
                return due;
            return !due || d.readback.requirement == VfxGpuReadbackRequirement::Optional;
        }
    }  // namespace

    namespace {
        /** @brief Compares all random-key identity components without padding or hashing. */
        auto VfxRandomIdentity(const Vfx::ParticleRandomIdentity &identity) noexcept {
            return std::tuple{identity.scope, identity.slot, identity.generation};
        }

        /** @brief Includes derived layout extents as well as authored dimensions. */
        auto VfxLayoutIdentity(const GpuParticleLayout &layout) noexcept {
            return std::tuple{layout.capacity,    layout.customFloatStreams, layout.streamStrideWords,
                              layout.streamCount, layout.wordCount,          layout.byteCount};
        }

        /** @brief Includes the entire admitted arithmetic envelope. */
        auto VfxArithmeticIdentity(const VfxGpuArithmeticEnvelope &envelope) noexcept {
            return std::tuple{envelope.maximumSteps,
                              envelope.maximumDeltaSeconds,
                              envelope.maximumPositionMagnitude,
                              envelope.maximumVelocityMagnitude,
                              envelope.maximumAccelerationMagnitude,
                              envelope.maximumRotationMagnitude,
                              envelope.maximumSizeMagnitude};
        }

        /** @brief Keeps required observation policy and all finite retention bounds in identity. */
        auto VfxReadbackIdentity(const VfxGpuReadbackDescriptor &readback) noexcept {
            return std::tuple{readback.enabled,
                              readback.schemaVersion,
                              readback.purpose,
                              readback.requirement,
                              readback.cadenceSteps,
                              readback.maximumRecords,
                              readback.maximumBytesPerResult,
                              readback.maximumPendingResults};
        }

        /** @brief Compares scalar metadata exactly; an initialization digest alone never substitutes for descriptor equality. */
        auto VfxDescriptorIdentity(const VfxGpuSimulationDescriptor &descriptor) noexcept {
            const auto &identity = descriptor.identity;
            const auto &random = descriptor.random;
            return std::tuple{identity.scene,
                              VfxRandomIdentity(identity.activation),
                              VfxRandomIdentity(identity.emitter),
                              VfxRandomIdentity(identity.buffer),
                              VfxLayoutIdentity(descriptor.layout),
                              random.effectSeed,
                              VfxRandomIdentity(random.activation),
                              VfxRandomIdentity(random.emitter),
                              descriptor.simulationOrigin,
                              descriptor.initialParticleCount,
                              descriptor.forceCount,
                              descriptor.maximumPendingSteps,
                              descriptor.maximumRetainedBytes,
                              descriptor.capabilityRevision,
                              descriptor.policyRevision,
                              descriptor.initializationDigest,
                              VfxArithmeticIdentity(descriptor.arithmetic),
                              VfxReadbackIdentity(descriptor.readback)};
        }

    }  // namespace

    /** @copydoc SameVfxGpuDescriptor */
    bool SameVfxGpuDescriptor(const VfxGpuSimulationDescriptor &a, const VfxGpuSimulationDescriptor &b) noexcept {
        return VfxDescriptorIdentity(a) == VfxDescriptorIdentity(b);
    }

    /** @copydoc SameVfxGpuIdentity */
    bool SameVfxGpuIdentity(const VfxGpuSimulationIdentity &a, const VfxGpuSimulationIdentity &b) noexcept {
        return a.scene == b.scene && VfxGpuDetail::Same(a.activation, b.activation) && VfxGpuDetail::Same(a.emitter, b.emitter) &&
               VfxGpuDetail::Same(a.buffer, b.buffer);
    }

    /** @copydoc ComputeVfxGpuInitializationDigest */
    Sha256Digest ComputeVfxGpuInitializationDigest(const VfxGpuSimulationPreparation &p) noexcept {
        if (!p.descriptor.layout.IsValid() || p.initialWords.size() != p.descriptor.layout.wordCount || p.descriptor.forceCount > 64 ||
            p.forceWords.size() != std::size_t{p.descriptor.forceCount} * 12)
            return {};
        Sha256Builder hash;
        constexpr std::array tag{std::byte{'V'}, std::byte{'F'}, std::byte{'X'}, std::byte{'G'},
                                 std::byte{'P'}, std::byte{'U'}, std::byte{1}};
        (void)hash.Update(tag);
        HashSource(hash, p.descriptor);
        HashDimensions(hash, p.descriptor);
        HashArithmetic(hash, p.descriptor.arithmetic);
        HashReadback(hash, p.descriptor.readback);
        for (const auto word : p.initialWords)
            HashWord(hash, word);
        for (const auto word : p.forceWords)
            HashWord(hash, word);
        return hash.Finalize();
    }

    /** @copydoc ValidateVfxGpuSimulationPreparation */
    Result<void> ValidateVfxGpuSimulationPreparation(const VfxGpuSimulationPreparation &p) {
        using Outcome = Result<void>;
        if (!VfxGpuDetail::ValidDescriptor(p.descriptor))
            return Outcome::Failure(MakeError(VfxGpuSimulationErrors::InvalidPreparation));
        const auto derived = ComputeVfxGpuArithmeticEnvelope(p);
        if (derived.HasError())
            return Outcome::Failure(derived.ErrorValue());
        if (!VfxGpuDetail::SameEnvelope(derived.Value(), p.descriptor.arithmetic) ||
            ComputeVfxGpuInitializationDigest(p) != p.descriptor.initializationDigest)
            return Outcome::Failure(MakeError(VfxGpuSimulationErrors::InvalidPreparation));
        return Outcome::Success();
    }

    /** @copydoc ValidateVfxGpuSimulationWork */
    bool ValidateVfxGpuSimulationWork(const VfxGpuSimulationDescriptor &d, const GpuVfxSimulationWork &w) noexcept {
        if (!VfxGpuDetail::ValidDescriptor(d) || !SameVfxGpuIdentity(d.identity, w.identity) || !StepRepresentation(w.step))
            return false;
        if (!StepBounds(d, w))
            return false;
        return !w.callerCancellation.IsCancellationRequested() && !w.lifetimeCancellation.IsCancellationRequested() &&
               ObservationIntent(d, w);
    }
}  // namespace Horo::Render
