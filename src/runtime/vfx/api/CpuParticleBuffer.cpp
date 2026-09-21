#include "Horo/Vfx/CpuParticleBuffer.h"

#include "Horo/Vfx/VfxErrors.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <utility>

namespace Horo::Vfx {
    namespace {
        constexpr std::uint32_t InvalidDenseIndex = std::numeric_limits<std::uint32_t>::max();
        constexpr std::size_t FixedFloatStreamCount = 12;
        constexpr std::size_t UnsignedStreamCount = 6;

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        [[nodiscard]] bool AddAlignedStream(std::size_t &bytes, const std::size_t elementSize, const std::uint32_t capacity) noexcept {
            constexpr std::size_t alignment = CpuParticleBufferHardLimits::StreamAlignment;
            if (bytes > std::numeric_limits<std::size_t>::max() - (alignment - 1U))
                return false;
            bytes = (bytes + alignment - 1U) & ~(alignment - 1U);
            if (capacity > std::numeric_limits<std::size_t>::max() / elementSize)
                return false;
            const std::size_t streamBytes = static_cast<std::size_t>(capacity) * elementSize;
            if (bytes > std::numeric_limits<std::size_t>::max() - streamBytes)
                return false;
            bytes += streamBytes;
            return true;
        }

        [[nodiscard]] Result<std::size_t> RequiredBytes(const CpuParticleBufferCreateInfo &info) {
            std::size_t bytes{};
            const std::size_t floatStreams = FixedFloatStreamCount + info.customFloatStreams;
            for (std::size_t stream = 0; stream < floatStreams; ++stream) {
                if (!AddAlignedStream(bytes, sizeof(float), info.capacity))
                    return Failure<std::size_t>(VfxErrors::ParticleBufferInvalid);
            }
            for (std::size_t stream = 0; stream < UnsignedStreamCount; ++stream) {
                if (!AddAlignedStream(bytes, sizeof(std::uint32_t), info.capacity))
                    return Failure<std::size_t>(VfxErrors::ParticleBufferInvalid);
            }
            if (!AddAlignedStream(bytes, sizeof(std::uint64_t), info.capacity))
                return Failure<std::size_t>(VfxErrors::ParticleBufferInvalid);
            return Result<std::size_t>::Success(bytes);
        }
    }  // namespace

    namespace Detail {
        struct CpuParticleStreams final {
            std::array<float *, FixedFloatStreamCount> floats{};
            std::array<float *, CpuParticleBufferHardLimits::CustomFloatStreams> customFloats{};
            std::uint32_t *packedColor{};
            std::uint32_t *customFlags{};
            std::uint32_t *denseToSlot{};
            std::uint32_t *slotToDense{};
            std::uint32_t *slotGeneration{};
            std::uint32_t *freeSlots{};
            std::uint64_t *simulationBySlot{};
        };

        struct CpuParticleMetrics final {
            std::uint32_t peakActive{};
            std::uint64_t failedSpawns{};
            std::uint64_t staleAccesses{};
        };

        struct CpuParticleBufferState final {
            CpuParticleBufferState() = default;
            CpuParticleBufferState(const CpuParticleBufferState &) = delete;
            CpuParticleBufferState &operator=(const CpuParticleBufferState &) = delete;
            CpuParticleBufferState(CpuParticleBufferState &&) = delete;
            CpuParticleBufferState &operator=(CpuParticleBufferState &&) = delete;

            ParticleBufferId buffer{};
            std::uint32_t capacity{};
            std::uint32_t customFloatStreamCount{};
            std::size_t allocatedBytes{};
            std::byte *storage{};
            CpuParticleStreams streams{};
            std::thread::id ownerThread{};
            std::uint64_t lastSimulationIdentity{};
            std::uint32_t active{};
            std::uint32_t freeCount{};
            std::uint32_t retired{};
            CpuParticleMetrics metrics{};
            bool shutDown{};

            ~CpuParticleBufferState() {
                if (storage != nullptr)
                    ::operator delete(storage, std::align_val_t{CpuParticleBufferHardLimits::StreamAlignment});
            }
        };
    }  // namespace Detail

    namespace {
        [[nodiscard]] Result<void> OwnerThreadResult(const Detail::CpuParticleBufferState &state) {
            if (std::this_thread::get_id() != state.ownerThread)
                return Failure<void>(VfxErrors::ParticleBufferThreadViolation);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateOperationalState(const Detail::CpuParticleBufferState *const state) {
            if (state == nullptr || state->shutDown)
                return Failure<void>(VfxErrors::ParticleBufferShutDown);
            return OwnerThreadResult(*state);
        }

        template <typename T> T *TakeStream(Detail::CpuParticleBufferState &state, std::size_t &offset) noexcept {
            constexpr std::size_t alignment = CpuParticleBufferHardLimits::StreamAlignment;
            offset = (offset + alignment - 1U) & ~(alignment - 1U);
            auto *const stream = static_cast<T *>(static_cast<void *>(state.storage + offset));
            offset += static_cast<std::size_t>(state.capacity) * sizeof(T);
            return stream;
        }

        void BindStreams(Detail::CpuParticleBufferState &state) noexcept {
            std::size_t offset{};
            for (float *&stream : state.streams.floats)
                stream = TakeStream<float>(state, offset);
            for (std::uint32_t index = 0; index < state.customFloatStreamCount; ++index)
                state.streams.customFloats[index] = TakeStream<float>(state, offset);
            state.streams.packedColor = TakeStream<std::uint32_t>(state, offset);
            state.streams.customFlags = TakeStream<std::uint32_t>(state, offset);
            state.streams.denseToSlot = TakeStream<std::uint32_t>(state, offset);
            state.streams.slotToDense = TakeStream<std::uint32_t>(state, offset);
            state.streams.slotGeneration = TakeStream<std::uint32_t>(state, offset);
            state.streams.freeSlots = TakeStream<std::uint32_t>(state, offset);
            state.streams.simulationBySlot = TakeStream<std::uint64_t>(state, offset);
        }

        void ZeroDenseParticle(Detail::CpuParticleBufferState &state, const std::uint32_t dense) noexcept {
            for (float *const stream : state.streams.floats)
                stream[dense] = 0.0F;
            for (std::uint32_t index = 0; index < state.customFloatStreamCount; ++index)
                state.streams.customFloats[index][dense] = 0.0F;
            state.streams.packedColor[dense] = 0;
            state.streams.customFlags[dense] = 0;
        }

        void MoveDenseParticle(Detail::CpuParticleBufferState &state, const std::uint32_t destination,
                               const std::uint32_t source) noexcept {
            for (float *const stream : state.streams.floats)
                stream[destination] = stream[source];
            for (std::uint32_t index = 0; index < state.customFloatStreamCount; ++index)
                state.streams.customFloats[index][destination] = state.streams.customFloats[index][source];
            state.streams.packedColor[destination] = state.streams.packedColor[source];
            state.streams.customFlags[destination] = state.streams.customFlags[source];
        }

        [[nodiscard]] Result<std::uint32_t> ResolveHandle(Detail::CpuParticleBufferState &state, const CpuParticleHandle &handle) {
            if (!handle.IsValid() || handle.buffer != state.buffer || handle.slot >= state.capacity)
                return Failure<std::uint32_t>(VfxErrors::ParticleHandleInvalid);
            const std::uint32_t dense = state.streams.slotToDense[handle.slot];
            if (dense == InvalidDenseIndex || dense >= state.active || state.streams.slotGeneration[handle.slot] != handle.generation ||
                state.streams.simulationBySlot[handle.slot] != handle.particle.Value()) {
                ++state.metrics.staleAccesses;
                return Failure<std::uint32_t>(VfxErrors::ParticleHandleStale);
            }
            return Result<std::uint32_t>::Success(dense);
        }

        void ClearLiveSlots(Detail::CpuParticleBufferState &state) noexcept {
            for (std::uint32_t dense = 0; dense < state.active; ++dense) {
                const std::uint32_t slot = state.streams.denseToSlot[dense];
                state.streams.slotToDense[slot] = InvalidDenseIndex;
                state.streams.simulationBySlot[slot] = 0;
                if (state.streams.slotGeneration[slot] == std::numeric_limits<std::uint32_t>::max())
                    ++state.retired;
                else
                    ++state.streams.slotGeneration[slot];
            }
            state.active = 0;
            state.freeCount = 0;
            for (std::uint32_t slot = state.capacity; slot > 0; --slot) {
                const std::uint32_t index = slot - 1U;
                if (state.streams.slotGeneration[index] != std::numeric_limits<std::uint32_t>::max())
                    state.streams.freeSlots[state.freeCount++] = index;
            }
        }
    }  // namespace

    /** @copydoc CpuParticleBuffer::~CpuParticleBuffer */
    CpuParticleBuffer::~CpuParticleBuffer() = default;

    /** @copydoc CpuParticleBuffer::CpuParticleBuffer */
    CpuParticleBuffer::CpuParticleBuffer(CpuParticleBuffer &&other) noexcept = default;

    /** @copydoc CpuParticleBuffer::operator= */
    CpuParticleBuffer &CpuParticleBuffer::operator=(CpuParticleBuffer &&other) noexcept = default;

    /** @copydoc CpuParticleBuffer::CpuParticleBuffer */
    CpuParticleBuffer::CpuParticleBuffer(std::unique_ptr<Detail::CpuParticleBufferState> state) noexcept : state_(std::move(state)) {}

    /** @copydoc CpuParticleBuffer::Create */
    Result<CpuParticleBuffer> CpuParticleBuffer::Create(const CpuParticleBufferCreateInfo &info) {
        if (!info.buffer.IsValid() || info.capacity == 0 || info.capacity > CpuParticleBufferHardLimits::Particles ||
            info.customFloatStreams > CpuParticleBufferHardLimits::CustomFloatStreams || info.maximumBytes == 0 ||
            info.maximumBytes > CpuParticleBufferHardLimits::Bytes)
            return Failure<CpuParticleBuffer>(VfxErrors::ParticleBufferInvalid);
        const auto required = RequiredBytes(info);
        if (required.HasError() || required.Value() > info.maximumBytes || required.Value() > CpuParticleBufferHardLimits::Bytes)
            return Failure<CpuParticleBuffer>(VfxErrors::ParticleBufferInvalid);

        try {
            auto state = std::make_unique<Detail::CpuParticleBufferState>();
            state->buffer = info.buffer;
            state->capacity = info.capacity;
            state->customFloatStreamCount = info.customFloatStreams;
            state->allocatedBytes = required.Value();
            state->storage = static_cast<std::byte *>(
                ::operator new(state->allocatedBytes, std::align_val_t{CpuParticleBufferHardLimits::StreamAlignment}));
            std::memset(state->storage, 0, state->allocatedBytes);
            state->ownerThread = std::this_thread::get_id();
            BindStreams(*state);
            state->freeCount = state->capacity;
            for (std::uint32_t index = 0; index < state->capacity; ++index) {
                state->streams.slotToDense[index] = InvalidDenseIndex;
                state->streams.slotGeneration[index] = 1;
                state->streams.freeSlots[index] = state->capacity - index - 1U;
            }
            return Result<CpuParticleBuffer>::Success(CpuParticleBuffer{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Failure<CpuParticleBuffer>(VfxErrors::ParticleBufferAllocationFailed);
        }
    }

    /** @copydoc CpuParticleBuffer::Spawn */
    Result<CpuParticleHandle> CpuParticleBuffer::Spawn(const ParticleSimulationId particle) {
        if (const auto state = ValidateOperationalState(state_.get()); state.HasError())
            return Result<CpuParticleHandle>::Failure(state.ErrorValue());
        if (!particle.IsValid() || particle.Value() <= state_->lastSimulationIdentity) {
            ++state_->metrics.failedSpawns;
            return Failure<CpuParticleHandle>(VfxErrors::ParticleSimulationIdentityInvalid);
        }
        if (state_->freeCount == 0) {
            ++state_->metrics.failedSpawns;
            return Failure<CpuParticleHandle>(VfxErrors::ParticleBufferCapacityExceeded);
        }

        const std::uint32_t slot = state_->streams.freeSlots[--state_->freeCount];
        const std::uint32_t dense = state_->active++;
        state_->streams.denseToSlot[dense] = slot;
        state_->streams.slotToDense[slot] = dense;
        state_->streams.simulationBySlot[slot] = particle.Value();
        state_->lastSimulationIdentity = particle.Value();
        ZeroDenseParticle(*state_, dense);
        state_->metrics.peakActive = std::max(state_->metrics.peakActive, state_->active);
        return Result<CpuParticleHandle>::Success(
            {.buffer = state_->buffer, .particle = particle, .slot = slot, .generation = state_->streams.slotGeneration[slot]});
    }

    /** @copydoc CpuParticleBuffer::Kill */
    Result<void> CpuParticleBuffer::Kill(const CpuParticleHandle &handle) {
        if (auto state = ValidateOperationalState(state_.get()); state.HasError())
            return state;
        const auto resolved = ResolveHandle(*state_, handle);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());

        const std::uint32_t dense = resolved.Value();
        if (const std::uint32_t last = state_->active - 1U; dense != last) {
            MoveDenseParticle(*state_, dense, last);
            const std::uint32_t movedSlot = state_->streams.denseToSlot[last];
            state_->streams.denseToSlot[dense] = movedSlot;
            state_->streams.slotToDense[movedSlot] = dense;
        }
        --state_->active;
        state_->streams.slotToDense[handle.slot] = InvalidDenseIndex;
        state_->streams.simulationBySlot[handle.slot] = 0;
        if (state_->streams.slotGeneration[handle.slot] == std::numeric_limits<std::uint32_t>::max())
            ++state_->retired;
        else {
            ++state_->streams.slotGeneration[handle.slot];
            state_->streams.freeSlots[state_->freeCount++] = handle.slot;
        }
        return Result<void>::Success();
    }

    /** @copydoc CpuParticleBuffer::ResolveDenseIndex */
    Result<std::uint32_t> CpuParticleBuffer::ResolveDenseIndex(const CpuParticleHandle &handle) {
        if (const auto state = ValidateOperationalState(state_.get()); state.HasError())
            return Result<std::uint32_t>::Failure(state.ErrorValue());
        return ResolveHandle(*state_, handle);
    }

    /** @copydoc CpuParticleBuffer::CopyFrom */
    Result<void> CpuParticleBuffer::CopyFrom(const CpuParticleBuffer &source) {
        if (const auto state = ValidateOperationalState(state_.get()); state.HasError())
            return state;
        if (const auto sourceState = ValidateOperationalState(source.state_.get()); sourceState.HasError())
            return sourceState;
        if (this == &source)
            return Result<void>::Success();
        if (state_->buffer != source.state_->buffer || state_->capacity != source.state_->capacity ||
            state_->customFloatStreamCount != source.state_->customFloatStreamCount ||
            state_->allocatedBytes != source.state_->allocatedBytes)
            return Failure<void>(VfxErrors::ParticleBufferInvalid);

        std::memcpy(state_->storage, source.state_->storage, state_->allocatedBytes);
        state_->lastSimulationIdentity = source.state_->lastSimulationIdentity;
        state_->active = source.state_->active;
        state_->freeCount = source.state_->freeCount;
        state_->retired = source.state_->retired;
        state_->metrics = source.state_->metrics;
        return Result<void>::Success();
    }

    /** @copydoc CpuParticleBuffer::CompactStable */
    Result<void> CpuParticleBuffer::CompactStable(const std::span<const CpuParticleHandle> survivors) {
        if (auto state = ValidateOperationalState(state_.get()); state.HasError())
            return state;
        if (survivors.size() > state_->capacity)
            return Failure<void>(VfxErrors::ParticleStableCompactionInvalid);

        const std::uint32_t oldActive = state_->active;
        std::uint32_t previousDense = InvalidDenseIndex;
        for (std::size_t index = 0; index < survivors.size(); ++index) {
            const auto resolved = ResolveHandle(*state_, survivors[index]);
            if (resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            const std::uint32_t sourceDense = resolved.Value();
            if (previousDense != InvalidDenseIndex && sourceDense <= previousDense)
                return Failure<void>(VfxErrors::ParticleStableCompactionInvalid);
            state_->streams.freeSlots[index] = sourceDense;
            previousDense = sourceDense;
        }

        for (std::uint32_t dense = 0; dense < oldActive; ++dense) {
            const std::uint32_t slot = state_->streams.denseToSlot[dense];
            state_->streams.slotToDense[slot] = InvalidDenseIndex;
        }

        const auto newActive = static_cast<std::uint32_t>(survivors.size());
        for (std::uint32_t dense = 0; dense < newActive; ++dense) {
            const CpuParticleHandle &handle = survivors[dense];
            MoveDenseParticle(*state_, dense, state_->streams.freeSlots[dense]);
            state_->streams.denseToSlot[dense] = handle.slot;
            state_->streams.slotToDense[handle.slot] = dense;
            state_->streams.simulationBySlot[handle.slot] = handle.particle.Value();
        }

        for (std::uint32_t slot = 0; slot < state_->capacity; ++slot) {
            if (state_->streams.slotToDense[slot] != InvalidDenseIndex || state_->streams.simulationBySlot[slot] == 0)
                continue;
            state_->streams.simulationBySlot[slot] = 0;
            if (state_->streams.slotGeneration[slot] == std::numeric_limits<std::uint32_t>::max())
                ++state_->retired;
            else
                ++state_->streams.slotGeneration[slot];
        }

        for (std::uint32_t dense = newActive; dense < oldActive; ++dense)
            ZeroDenseParticle(*state_, dense);
        state_->active = newActive;
        state_->freeCount = 0;
        for (std::uint32_t slot = state_->capacity; slot > 0; --slot) {
            const std::uint32_t index = slot - 1U;
            if (state_->streams.slotToDense[index] == InvalidDenseIndex &&
                state_->streams.slotGeneration[index] != std::numeric_limits<std::uint32_t>::max())
                state_->streams.freeSlots[state_->freeCount++] = index;
        }
        return Result<void>::Success();
    }

    /** @copydoc CpuParticleBuffer::View */
    Result<CpuParticleSoAView> CpuParticleBuffer::View() {
        if (const auto state = ValidateOperationalState(state_.get()); state.HasError())
            return Result<CpuParticleSoAView>::Failure(state.ErrorValue());
        const std::size_t count = state_->active;
        CpuParticleSoAView view{
            .positionX = {state_->streams.floats[0], count},
            .positionY = {state_->streams.floats[1], count},
            .positionZ = {state_->streams.floats[2], count},
            .velocityX = {state_->streams.floats[3], count},
            .velocityY = {state_->streams.floats[4], count},
            .velocityZ = {state_->streams.floats[5], count},
            .sizeX = {state_->streams.floats[6], count},
            .sizeY = {state_->streams.floats[7], count},
            .rotation = {state_->streams.floats[8], count},
            .angularVelocity = {state_->streams.floats[9], count},
            .packedColor = {state_->streams.packedColor, count},
            .age = {state_->streams.floats[10], count},
            .maximumAge = {state_->streams.floats[11], count},
            .customFlags = {state_->streams.customFlags, count},
            .customFloatStreamCount = state_->customFloatStreamCount,
        };
        for (std::uint32_t index = 0; index < state_->customFloatStreamCount; ++index)
            view.customFloats[index] = {state_->streams.customFloats[index], count};
        return Result<CpuParticleSoAView>::Success(view);
    }

    /** @copydoc CpuParticleBuffer::Clear */
    Result<void> CpuParticleBuffer::Clear() {
        if (auto state = ValidateOperationalState(state_.get()); state.HasError())
            return state;
        ClearLiveSlots(*state_);
        return Result<void>::Success();
    }

    /** @copydoc CpuParticleBuffer::Shutdown */
    Result<void> CpuParticleBuffer::Shutdown() {
        if (state_ == nullptr)
            return Result<void>::Success();
        if (auto owner = OwnerThreadResult(*state_); owner.HasError())
            return owner;
        if (!state_->shutDown) {
            ClearLiveSlots(*state_);
            state_->shutDown = true;
        }
        return Result<void>::Success();
    }

    /** @copydoc CpuParticleBuffer::Statistics */
    CpuParticleBufferStatistics CpuParticleBuffer::Statistics() const noexcept {
        if (state_ == nullptr)
            return {};
        return {.capacity = state_->capacity,
                .active = state_->active,
                .available = state_->freeCount,
                .retired = state_->retired,
                .peakActive = state_->metrics.peakActive,
                .failedSpawns = state_->metrics.failedSpawns,
                .staleAccesses = state_->metrics.staleAccesses,
                .allocatedBytes = state_->allocatedBytes};
    }
}  // namespace Horo::Vfx
