#pragma once
/** @file GpuParticleLayout.h
 * @brief Explicit word-addressed particle storage and canonical force lowering for renderer-owned GPU simulation.
 * @details This replaces assumptions about CPU SoA addresses or native structure padding at the GPU boundary.
 * ParticleSimulationData remains the sole force/RNG semantic authority; existing CPU storage and raster UBOs
 * retain their contracts. A valid layout grants neither device support, allocation ownership nor GPU completion.
 */
#include "Horo/Foundation/Result.h"
#include "Horo/Vfx/ParticleSimulationData.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace Horo::Render {
    namespace GpuParticleLayoutErrors {
        extern const ErrorCodeDescriptor InvalidLayout;
        extern const ErrorCodeDescriptor CapacityExceeded;
        extern const ErrorCodeDescriptor InvalidForces;
        extern const ErrorCodeDescriptor InputAliased;
    }  // namespace GpuParticleLayoutErrors

    /** @brief Version-one stream indices, each holding one numeric uint32 word per particle slot.
     * @details Position streams are emitter-local, as in the canonical CPU SoA, and float streams contain exact
     * IEEE-754 bits. PackedColor is numeric 0xRRGGBBAA in linear color,
     * not an array of host-order bytes. ParticleIdLow/High encode the stable simulation identity, independent of
     * dense/native slot indices; a live particle identity is nonzero. Alive is a separate zero-or-one word,
     * never inferred from CustomFlags (whose canonical kill bit remains bit zero). SlotGeneration is nonzero for an occupied issued slot
     * and never wraps. Layout arithmetic does not admit particle values or generate identities. Custom streams are the canonical declared
     * payload channels in ascending stream order, starting at FirstCustomFloat.
     */
    enum class GpuParticleStream : std::uint32_t {
        PositionX = 0,
        PositionY = 1,
        PositionZ = 2,
        VelocityX = 3,
        VelocityY = 4,
        VelocityZ = 5,
        SizeX = 6,
        SizeY = 7,
        Rotation = 8,
        AngularVelocity = 9,
        Age = 10,
        MaximumAge = 11,
        PackedColor = 12,
        CustomFlags = 13,
        ParticleIdLow = 14,
        ParticleIdHigh = 15,
        Alive = 16,
        SlotGeneration = 17,
        FirstCustomFloat = 18
    };

    /** @brief Checked single-buffer SoA dimensions; all addresses are words, never native byte-pointer offsets. */
    struct GpuParticleLayout final {
        static constexpr std::uint32_t Version = 1;
        static constexpr std::uint32_t MaximumParticles = 1'000'000;
        static constexpr std::uint32_t BaseStreams = 18;
        static constexpr std::uint32_t MaximumCustomFloatStreams = 16;
        static constexpr std::uint32_t StreamAlignmentWords = 4;
        static constexpr std::size_t WordBytes = 4;

        std::uint32_t capacity{};
        std::uint32_t customFloatStreams{};
        std::uint32_t streamStrideWords{}; /**< Capacity rounded up to four words; padding lanes are not particle slots. */
        std::uint32_t streamCount{};       /**< Exactly 18 plus the declared custom stream count. */
        std::uint32_t wordCount{};         /**< Full storage extent, including every stream's alignment padding. */
        std::size_t byteCount{};           /**< Exact wordCount * 4 allocation requirement. */

        /** @brief Checks the entire version-one shape and derived extents without allocating or trusting public fields. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Caller-owned cold admission limits; the renderer supplies the selected device's actual buffer ceiling. */
    struct GpuParticleLayoutLimits final {
        std::size_t maximumBytes{};
        std::uint64_t maximumDeviceBufferBytes{};
    };

    /** @brief Builds one finite storage layout at a cold preparation boundary; performs no native allocation.
     * @param capacity One through one million real particle slots, independent of padded stream stride.
     * @param customFloatStreams Zero through sixteen declared custom float streams.
     * @param limits Nonzero caller byte budget and actual selected-device maximum buffer size; neither raises hard limits.
     * @return Exact validated dimensions or typed invalid/capacity failure; error construction may allocate diagnostic text.
     * @details Every stream starts at a 16-byte boundary. Both word and byte extents are checked before publication.
     */
    [[nodiscard]] Result<GpuParticleLayout> BuildGpuParticleLayout(std::uint32_t capacity, std::uint32_t customFloatStreams,
                                                                   const GpuParticleLayoutLimits &limits);

    /** @brief Resolves one real particle slot in one declared stream without allocation, including on failure.
     * @param layout Complete dimensions returned by BuildGpuParticleLayout; malformed copies are rejected.
     * @param stream Base enum index or FirstCustomFloat plus a declared custom channel index.
     * @param slot Real slot strictly below capacity; padding lanes cannot be addressed through this method.
     * @param wordOffset Receives the buffer-relative word address on success and zero on failure.
     * @return True only for a valid complete layout and address; performs no resource or particle-identity lookup.
     */
    [[nodiscard]] bool TryGpuParticleWordOffset(const GpuParticleLayout &layout, std::uint32_t stream, std::uint32_t slot,
                                                std::uint32_t &wordOffset) noexcept;

    /** @brief Explicit twelve-word force record; offsets are numeric words, independent of host/native structure layout. */
    namespace GpuParticleForceWords {
        inline constexpr std::size_t Kind = 0, RandomChannel = 1, Strength = 2, Falloff = 3;
        inline constexpr std::size_t VectorX = 4, VectorY = 5, VectorZ = 6, Frequency = 7;
        inline constexpr std::size_t CenterX = 8, CenterY = 9, CenterZ = 10, Reserved = 11;
        inline constexpr std::size_t Count = 12, Bytes = 48;
    }  // namespace GpuParticleForceWords

    /** @brief Lowers the shared ordered force descriptors into caller-owned numeric shader words without native work.
     * @param forces Canonical force stack, validated by Vfx::ValidateParticleForceModules with its existing 64-module ceiling.
     * @param destination At least 12 * forces.size() writable words, disjoint from all borrowed force objects.
     * @return Written word count or typed invalid/capacity/alias failure. An empty force stack writes nothing and returns zero.
     * @details All validation precedes all writes; failure and the unused destination tail preserve every byte. Scalars
     * use exact float bit patterns, kind/channel are integers, and the reserved word is zero. This is cold immutable
     * force preparation; retain the resulting words instead of rebuilding them each frame. Success allocates nothing;
     * failure may construct diagnostic text. No Math::Vec3 or descriptor object representation is copied.
     */
    [[nodiscard]] Result<std::size_t> PackGpuParticleForces(std::span<const Vfx::CpuParticleForceModule> forces,
                                                            std::span<std::uint32_t> destination);

    static_assert(static_cast<std::uint32_t>(GpuParticleStream::FirstCustomFloat) == GpuParticleLayout::BaseStreams);
    static_assert(std::numeric_limits<float>::is_iec559);
    static_assert(sizeof(float) == GpuParticleLayout::WordBytes && sizeof(std::uint32_t) == GpuParticleLayout::WordBytes);
    static_assert(GpuParticleForceWords::Count * GpuParticleLayout::WordBytes == GpuParticleForceWords::Bytes);
}  // namespace Horo::Render
