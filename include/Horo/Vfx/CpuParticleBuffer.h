#pragma once

/**
 * @file CpuParticleBuffer.h
 * @brief Preallocated CPU particle SoA storage and generation-safe slot bookkeeping.
 */

#include "Horo/Foundation/StrongId.h"
#include "Horo/Vfx/VfxErrors.h"
#include "Horo/Vfx/VfxIdentity.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Horo::Vfx {
    namespace Detail {
        struct ParticleSimulationIdentityTag;
        struct CpuParticleBufferState;
    }  // namespace Detail

    /** @brief Stable monotonic simulation identity independent of a recyclable storage slot. */
    using ParticleSimulationId =
        Foundation::Detail::NonZeroId64<Detail::ParticleSimulationIdentityTag, VfxErrors::ParticleSimulationIdentityInvalid>;

    /** @brief Compile-time ceilings that project and product configuration cannot raise. */
    struct CpuParticleBufferHardLimits final {
        static constexpr std::uint32_t Particles = 1'000'000;      /**< Absolute particle-slot ceiling. */
        static constexpr std::uint32_t CustomFloatStreams = 16;    /**< Absolute custom float stream ceiling. */
        static constexpr std::size_t Bytes = 512U * 1024U * 1024U; /**< Absolute single-buffer byte ceiling. */
        static constexpr std::size_t StreamAlignment = 64;         /**< Cache-line alignment of every SoA stream. */
    };

    /** @brief Finite allocation policy captured before frame-hot use begins. */
    struct CpuParticleBufferCreateInfo final {
        ParticleBufferId buffer{};                                    /**< Exact generation-safe logical buffer identity. */
        std::uint32_t capacity{};                                     /**< Fixed slot and dense-particle capacity. */
        std::uint32_t customFloatStreams{};                           /**< Number of private compiled float payload streams. */
        std::size_t maximumBytes{CpuParticleBufferHardLimits::Bytes}; /**< Product-lowered allocation ceiling. */
    };

    /** @brief Exact recyclable slot reference paired with a stable simulation identity. */
    struct CpuParticleHandle final {
        ParticleBufferId buffer{};                                               /**< Owning buffer incarnation. */
        ParticleSimulationId particle{};                                         /**< Stable identity that never follows slot reuse. */
        std::uint32_t slot{VfxIdentity<ParticleBufferIdentityTag>::InvalidSlot}; /**< Recyclable owner slot. */
        std::uint32_t generation{};                                              /**< Non-zero non-wrapping slot generation. */

        /** @brief Checks representation only, not current residency. @return True when every dimension is usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return buffer.IsValid() && particle.IsValid() && slot != VfxIdentity<ParticleBufferIdentityTag>::InvalidSlot && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const CpuParticleHandle &) const noexcept = default;
    };

    /** @brief Mutable owner-thread view over the packed live prefix of every SoA stream. */
    struct CpuParticleSoAView final {
        std::span<float> positionX, positionY, positionZ; /**< Emitter-local position components. */
        std::span<float> velocityX, velocityY, velocityZ; /**< Velocity components. */
        std::span<float> sizeX, sizeY;                    /**< Billboard or mesh scale components. */
        std::span<float> rotation, angularVelocity;       /**< Rotation and angular velocity. */
        std::span<std::uint32_t> packedColor;             /**< Backend-neutral packed linear color. */
        std::span<float> age, maximumAge;                 /**< Current and terminal age in seconds. */
        std::span<std::uint32_t> customFlags;             /**< Private compiled bit payload. */
        std::array<std::span<float>, CpuParticleBufferHardLimits::CustomFloatStreams> customFloats{}; /**< Private float payloads. */
        std::uint32_t customFloatStreamCount{}; /**< Leading valid entries in customFloats. */
    };

    /** @brief Allocation-free capacity and churn diagnostics. */
    struct CpuParticleBufferStatistics final {
        std::uint32_t capacity{};      /**< Immutable configured slot count. */
        std::uint32_t active{};        /**< Packed live particle count. */
        std::uint32_t available{};     /**< Reusable free slots. */
        std::uint32_t retired{};       /**< Permanently retired generation-exhausted slots. */
        std::uint32_t peakActive{};    /**< Lifetime live-particle high-water mark. */
        std::uint64_t failedSpawns{};  /**< Capacity or identity admission failures. */
        std::uint64_t staleAccesses{}; /**< Rejected stale handle lookups or kills. */
        std::size_t allocatedBytes{};  /**< Exact aligned storage bytes owned by the buffer. */
    };

    /**
     * @brief Move-only preallocated CPU particle storage owned by one VFX simulation thread.
     *
     * Create is the only allocation phase. Spawn, kill, lookup, clear, view and shutdown are
     * bounded and allocation-free. Mutating and view-producing methods are owner-thread only;
     * callers must not retain a view across mutation. Killing uses swap-remove compaction while
     * stable handles continue to address owner slots through the indirection tables. Shutdown is
     * idempotent, closes admission and invalidates every live slot without wrapping generations.
     */
    class CpuParticleBuffer final {
    public:
        CpuParticleBuffer(const CpuParticleBuffer &) = delete;
        CpuParticleBuffer &operator=(const CpuParticleBuffer &) = delete;
        /** @brief Transfers sole storage ownership. @param other Buffer whose ownership is transferred. */
        CpuParticleBuffer(CpuParticleBuffer &&other) noexcept;

        /**
         * @brief Replaces this buffer by transferring sole storage ownership.
         * @param other Buffer whose ownership is transferred.
         * @return This buffer after the transfer.
         */
        CpuParticleBuffer &operator=(CpuParticleBuffer &&other) noexcept;

        /** @brief Releases the prepared storage after all simulation and extraction readers have quiesced. */
        ~CpuParticleBuffer();

        /**
         * @brief Preallocates one cache-line-aligned storage block and fixed bookkeeping tables.
         * @param info Exact buffer identity, capacity, custom-stream count and byte ceiling.
         * @return Buffer or a typed identity, limit, overflow or allocation failure.
         * @pre Setup/admission boundary; not frame-hot.
         */
        [[nodiscard]] static Result<CpuParticleBuffer> Create(const CpuParticleBufferCreateInfo &info);

        /**
         * @brief Appends one zero-initialized particle to the packed live prefix.
         * @param particle Strictly newer stable simulation identity than every prior successful spawn.
         * @return Generation-safe slot handle or a typed thread, lifecycle, identity or capacity failure.
         */
        [[nodiscard]] Result<CpuParticleHandle> Spawn(ParticleSimulationId particle);

        /**
         * @brief Removes one exact live particle using deterministic swap-remove compaction.
         * @param handle Exact handle returned by Spawn.
         * @return Success or a typed thread, malformed, foreign or stale-handle failure.
         */
        [[nodiscard]] Result<void> Kill(const CpuParticleHandle &handle);

        /**
         * @brief Resolves a live handle to its current packed dense index.
         * @param handle Exact handle returned by Spawn.
         * @return Dense index or a typed thread, malformed, foreign or stale-handle failure.
         */
        [[nodiscard]] Result<std::uint32_t> ResolveDenseIndex(const CpuParticleHandle &handle);

        /**
         * @brief Copies a prepared source generation into this buffer without allocating.
         * @param source Owner-thread buffer with the same identity, capacity, payload layout, and byte budget.
         * @return Success or a typed lifecycle, thread, or compatibility failure.
         */
        [[nodiscard]] Result<void> CopyFrom(const CpuParticleBuffer &source);

        /**
         * @brief Stable-compacts the live prefix to the supplied survivor order.
         * @param survivors Current live handles in strictly increasing dense-index order.
         * @return Success or a typed lifecycle, thread, malformed, stale, or compaction-order failure.
         */
        [[nodiscard]] Result<void> CompactStable(std::span<const CpuParticleHandle> survivors);

        /** @brief Returns an owner-thread mutable view of the packed live prefix. @return View or thread/lifecycle failure. */
        [[nodiscard]] Result<CpuParticleSoAView> View();

        /** @brief Invalidates all live slots while retaining allocation and monotonic identity history. @return Success or thread failure.
         */
        [[nodiscard]] Result<void> Clear();

        /** @brief Idempotently closes admission and invalidates every live slot. @return Success or thread failure. */
        [[nodiscard]] Result<void> Shutdown();

        /**
         * @brief Returns current allocation-free diagnostics on the owner thread.
         * @return Capacity, churn and byte counters.
         */
        [[nodiscard]] CpuParticleBufferStatistics Statistics() const noexcept;

    private:
        /** @brief Adopts one completely initialized internal state. @param state Sole mutable storage owner. */
        explicit CpuParticleBuffer(std::unique_ptr<Detail::CpuParticleBufferState> state) noexcept;
        std::unique_ptr<Detail::CpuParticleBufferState> state_;
    };
}  // namespace Horo::Vfx
