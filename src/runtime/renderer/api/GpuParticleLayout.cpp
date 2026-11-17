#include "Horo/Runtime/Render/GpuParticleLayout.h"

#include <bit>
#include <limits>

namespace Horo::Render {
    namespace {
        const ErrorDomainId Domain{"horo.render.gpu_particle_layout"};

        /** @brief Bounds shape before rounding or multiplication, including forged public layout values. */
        bool ValidShape(const std::uint32_t capacity, const std::uint32_t customFloatStreams) noexcept {
            return capacity > 0 && capacity <= GpuParticleLayout::MaximumParticles &&
                   customFloatStreams <= GpuParticleLayout::MaximumCustomFloatStreams;
        }

        /** @brief Derives exact dimensions only after ValidShape, whose hard bounds make every result representable. */
        GpuParticleLayout Describe(const std::uint32_t capacity, const std::uint32_t customFloatStreams) noexcept {
            constexpr std::uint64_t MaximumStride =
                (std::uint64_t{GpuParticleLayout::MaximumParticles} + GpuParticleLayout::StreamAlignmentWords - 1) &
                ~std::uint64_t{GpuParticleLayout::StreamAlignmentWords - 1};
            constexpr std::uint64_t MaximumWords =
                MaximumStride * (GpuParticleLayout::BaseStreams + GpuParticleLayout::MaximumCustomFloatStreams);
            static_assert(MaximumWords <= std::numeric_limits<std::uint32_t>::max());
            static_assert(MaximumWords * GpuParticleLayout::WordBytes <= std::numeric_limits<std::size_t>::max());
            constexpr auto Mask = GpuParticleLayout::StreamAlignmentWords - 1;
            const auto stride = (capacity + Mask) & ~Mask;
            const auto streams = GpuParticleLayout::BaseStreams + customFloatStreams;
            const auto words = stride * streams;
            return {capacity, customFloatStreams, stride, streams, words, std::size_t{words} * GpuParticleLayout::WordBytes};
        }

        /** @brief Compares borrowed byte intervals without overflowing end-address arithmetic. */
        bool Overlaps(const std::span<const std::byte> source, const std::span<std::byte> destination) noexcept {
            if (source.empty() || destination.empty())
                return false;
            const auto first = reinterpret_cast<std::uintptr_t>(source.data());
            const auto second = reinterpret_cast<std::uintptr_t>(destination.data());
            return first <= second ? second - first < source.size() : first - second < destination.size();
        }

        /** @brief Writes exactly one already-validated semantic force; no native object representation crosses the ABI. */
        void WriteForce(const Vfx::CpuParticleForceModule &force, const std::span<std::uint32_t> words) noexcept {
            using namespace GpuParticleForceWords;
            words[Kind] = static_cast<std::uint32_t>(force.kind);
            words[RandomChannel] = force.randomChannel;
            words[Strength] = std::bit_cast<std::uint32_t>(force.strength);
            words[Falloff] = std::bit_cast<std::uint32_t>(force.falloff);
            words[VectorX] = std::bit_cast<std::uint32_t>(force.vector.x);
            words[VectorY] = std::bit_cast<std::uint32_t>(force.vector.y);
            words[VectorZ] = std::bit_cast<std::uint32_t>(force.vector.z);
            words[Frequency] = std::bit_cast<std::uint32_t>(force.frequency);
            words[CenterX] = std::bit_cast<std::uint32_t>(force.center.x);
            words[CenterY] = std::bit_cast<std::uint32_t>(force.center.y);
            words[CenterZ] = std::bit_cast<std::uint32_t>(force.center.z);
            words[Reserved] = 0;
        }
    }  // namespace

    namespace GpuParticleLayoutErrors {
        const ErrorCodeDescriptor
            InvalidLayout{.domain = Domain,
                          .code = ErrorCode{"render.gpu_particle_layout.invalid"},
                          .defaultSeverity = ErrorSeverity::Error,
                          .summary = "GPU particle layout dimensions or limits are invalid.",
                          .remediationHint =
                              "Supply bounded particle/custom-stream counts and explicit nonzero caller/device byte ceilings.",
                          .retryable = false,
                          .userActionable = false};
        const ErrorCodeDescriptor
            CapacityExceeded{.domain = Domain,
                             .code = ErrorCode{"render.gpu_particle_layout.capacity"},
                             .defaultSeverity = ErrorSeverity::Error,
                             .summary = "GPU particle storage exceeds its explicit byte or output capacity.",
                             .remediationHint =
                                 "Lower the admitted capacity or prepare sufficient caller/device storage before activation.",
                             .retryable = false,
                             .userActionable = false};
        const ErrorCodeDescriptor InvalidForces{.domain = Domain,
                                                .code = ErrorCode{"render.gpu_particle_layout.forces_invalid"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "GPU particle forces violate the canonical simulation descriptor contract.",
                                                .remediationHint =
                                                    "Use a finite bounded shared force stack with distinct admitted noise channels.",
                                                .retryable = false,
                                                .userActionable = false};
        const ErrorCodeDescriptor
            InputAliased{.domain = Domain,
                         .code = ErrorCode{"render.gpu_particle_layout.input_aliased"},
                         .defaultSeverity = ErrorSeverity::Error,
                         .summary = "GPU particle force output overlaps borrowed input descriptors.",
                         .remediationHint = "Prepare disjoint force word storage; native/host structure padding is not a shader ABI.",
                         .retryable = false,
                         .userActionable = false};
    }  // namespace GpuParticleLayoutErrors

    /** @copydoc GpuParticleLayout::IsValid */
    bool GpuParticleLayout::IsValid() const noexcept {
        if (!ValidShape(capacity, customFloatStreams))
            return false;
        const auto expected = Describe(capacity, customFloatStreams);
        return streamStrideWords == expected.streamStrideWords && streamCount == expected.streamCount && wordCount == expected.wordCount &&
               byteCount == expected.byteCount;
    }

    /** @copydoc BuildGpuParticleLayout */
    Result<GpuParticleLayout> BuildGpuParticleLayout(const std::uint32_t capacity, const std::uint32_t customFloatStreams,
                                                     const GpuParticleLayoutLimits &limits) {
        if (!ValidShape(capacity, customFloatStreams) || limits.maximumBytes == 0 || limits.maximumDeviceBufferBytes == 0)
            return Result<GpuParticleLayout>::Failure(MakeError(GpuParticleLayoutErrors::InvalidLayout));
        const auto layout = Describe(capacity, customFloatStreams);
        if (layout.byteCount > limits.maximumBytes || layout.byteCount > limits.maximumDeviceBufferBytes)
            return Result<GpuParticleLayout>::Failure(MakeError(GpuParticleLayoutErrors::CapacityExceeded));
        return Result<GpuParticleLayout>::Success(layout);
    }

    /** @copydoc TryGpuParticleWordOffset */
    bool TryGpuParticleWordOffset(const GpuParticleLayout &layout, const std::uint32_t stream, const std::uint32_t slot,
                                  std::uint32_t &wordOffset) noexcept {
        wordOffset = 0;
        if (!layout.IsValid() || stream >= layout.streamCount || slot >= layout.capacity)
            return false;
        wordOffset = stream * layout.streamStrideWords + slot;
        return true;
    }

    /** @copydoc PackGpuParticleForces */
    Result<std::size_t> PackGpuParticleForces(const std::span<const Vfx::CpuParticleForceModule> forces,
                                              const std::span<std::uint32_t> destination) {
        if (!Vfx::ValidateParticleForceModules(forces))
            return Result<std::size_t>::Failure(MakeError(GpuParticleLayoutErrors::InvalidForces));
        const auto words = forces.size() * GpuParticleForceWords::Count;
        if (destination.size() < words)
            return Result<std::size_t>::Failure(MakeError(GpuParticleLayoutErrors::CapacityExceeded));
        const auto output = destination.first(words);
        if (Overlaps(std::as_bytes(forces), std::as_writable_bytes(output)))
            return Result<std::size_t>::Failure(MakeError(GpuParticleLayoutErrors::InputAliased));
        for (std::size_t index = 0; index < forces.size(); ++index)
            WriteForce(forces[index], output.subspan(index * GpuParticleForceWords::Count, GpuParticleForceWords::Count));
        return Result<std::size_t>::Success(words);
    }
}  // namespace Horo::Render
