#pragma once

/** @file AudioDSPNode.h
 * @brief Prepared, bounded and callback-safe multi-I/O audio DSP processing contract.
 */

#include "Horo/Audio/AudioIdentity.h"
#include "Horo/Audio/AudioPlanarBlock.h"
#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Audio {
    inline constexpr std::size_t AudioDSPMemoryAlignment = 64;
    inline constexpr std::size_t MaximumAudioDSPPorts = 16;
    inline constexpr std::size_t MaximumAudioDSPParameters = 128;
    inline constexpr std::size_t MaximumAudioDSPStateBytes = 64U * 1024U * 1024U;
    inline constexpr std::size_t MaximumAudioDSPScratchBytes = 64U * 1024U * 1024U;
    inline constexpr std::uint32_t MaximumAudioDSPLatencyFrames = MaximumAudioCallbackFrames * 64;
    inline constexpr std::uint32_t MaximumAudioDSPTailFrames = MaximumAudioCallbackFrames * 256;

    /** @brief Distinguishes the signal role of a node input. Outputs are always node results. */
    enum class AudioDSPPortKind : std::uint8_t {
        Main,
        Sidechain,
        Auxiliary
    };

    /** @brief Declares a bounded input or output channel contract before node activation. */
    struct AudioDSPPortDescriptor final {
        AudioDSPPortKind kind{AudioDSPPortKind::Main};
        AudioProcessingFormat format;
        std::uint32_t maximumFrames{MaximumAudioCallbackFrames};
        bool optional{}; /**< Only optional inputs may be disconnected; an output is always required. */
    };

    /** @brief Declares one stable parameter and its finite admitted range. */
    struct AudioDSPParameterDescriptor final {
        AudioParameterId identity;
        float minimum{};
        float maximum{1.0F};
        float defaultValue{};
    };

    /** @brief Fixed memory and real-time guarantees charged before a node can activate. */
    struct AudioDSPMemoryRequirements final {
        std::size_t stateBytes{};
        std::size_t scratchBytes{};
        std::size_t alignment{AudioDSPMemoryAlignment};
    };

    /** @brief Declares the complete shape, memory, timing and callback contract of one node. */
    struct AudioDSPNodeDescriptor final {
        std::vector<AudioDSPPortDescriptor> inputs;
        std::vector<AudioDSPPortDescriptor> outputs;
        std::vector<AudioDSPParameterDescriptor> parameters;
        AudioDSPMemoryRequirements memory;
        std::uint32_t maximumFrames{MaximumAudioCallbackFrames};
        std::uint32_t latencyFrames{};
        std::uint32_t tailFrames{};
        bool supportsBypass{true};
        bool supportsReset{true};
        bool inPlace{};
        bool allocationFree{true};
        bool blockingFree{true};
        bool loggingFree{true};
        bool externalCallbackFree{true};
    };

    /** @brief Preparation-time storage supplied by the host; node preparation does not own or resize it. */
    struct AudioDSPPrepareContext final {
        std::uint32_t maximumFrames{};
        std::span<std::byte> stateStorage;
        std::span<std::byte> scratchStorage;
    };

    /** @brief One value snapshot delivered in descriptor order without callback-side lookup. */
    struct AudioDSPParameterValue final {
        AudioParameterId identity;
        float value{};
        float target{};
        std::uint32_t rampFrames{};
    };

    /** @brief A connected or explicitly disconnected port binding for one process call. */
    struct AudioDSPPortBuffer final {
        bool connected{};
        AudioPlanarBlockView block;
    };

    /** @brief Host-selected behavior for a bypassed node. */
    enum class AudioDSPBypassMode : std::uint8_t {
        Process,
        CopyMainInput,
        Silence
    };

    /** @brief Fixed process fault identities that may be returned from the callback. */
    enum class AudioDSPFault : std::uint8_t {
        None,
        InvalidInput,
        InvalidOutput,
        NonFiniteOutput,
        WorkBudgetExceeded,
        InternalError
    };

    /** @brief Allocation-free process outcome; detailed diagnostics remain on the control path. */
    enum class AudioDSPProcessStatus : std::uint8_t {
        Processed,
        Bypassed,
        Tail,
        Rejected,
        Fault
    };

    /** @brief Exact bounded result from one callback invocation. */
    struct AudioDSPProcessResult final {
        AudioDSPProcessStatus status{AudioDSPProcessStatus::Rejected};
        AudioDSPFault fault{AudioDSPFault::None};
        std::uint32_t processedFrames{};
        std::uint32_t remainingTailFrames{};
    };

    /**
     * @brief Borrowed callback inputs, outputs, parameters and prepared memory for one block.
     *
     * Inputs and outputs are ordered exactly like the immutable node descriptor. An optional
     * input with connected=false is semantic silence. The spans and every block view are borrowed
     * for the call only. Process must not retain them, allocate, block, log, query ambient state,
     * or invoke an unbounded callback.
     */
    struct AudioDSPProcessContext final {
        std::span<const AudioDSPPortBuffer> inputs;
        std::span<AudioDSPPortBuffer> outputs;
        std::span<const AudioDSPParameterValue> parameters;
        std::span<std::byte> stateStorage;
        std::span<std::byte> scratchStorage;
        std::uint32_t frames{};
        AudioDSPBypassMode bypass{AudioDSPBypassMode::Process};
    };

    /**
     * @brief Validates all pre-activation shape, memory, timing and real-time declarations.
     * @param descriptor Candidate node contract.
     * @return Success when every requirement is explicit and bounded; otherwise a typed audio error.
     */
    [[nodiscard]] Result<void> ValidateAudioDSPNodeDescriptor(const AudioDSPNodeDescriptor &descriptor);

    /**
     * @brief Validates host storage against the node's complete admitted memory requirements.
     * @param descriptor Candidate node contract, validated on the control path.
     * @param preparation Host-owned storage and the selected processing-frame limit.
     * @return Success when preparation can proceed without resizing or allocating storage.
     */
    [[nodiscard]] Result<void> ValidateAudioDSPPreparation(const AudioDSPNodeDescriptor &descriptor,
                                                           const AudioDSPPrepareContext &preparation);

    /**
     * @brief Validates one callback invocation without reading sample contents.
     * @param descriptor Previously validated and immutable node contract.
     * @param process Borrowed callback buffers, parameters and prepared storage.
     * @return True when the invocation's dynamic bounds are admitted.
     * @note This is bounded callback-safe validation; it does not allocate, lock or dereference samples.
     * The descriptor is intentionally not structurally revalidated here; admission owns that work.
     */
    [[nodiscard]] bool ValidateAudioDSPProcess(const AudioDSPNodeDescriptor &descriptor, const AudioDSPProcessContext &process) noexcept;

    /**
     * @brief Prepared node implementation used by first-party C++ mixer strategies.
     *
     * Descriptor() is stable before activation. Prepare runs off the callback and receives all
     * state/scratch storage required by the descriptor. Process and Reset are called only by the
     * admitted owner according to the graph generation lifetime.
     */
    class IAudioDSPNode {
    public:
        virtual ~IAudioDSPNode() = default;
        [[nodiscard]] virtual const AudioDSPNodeDescriptor &Descriptor() const noexcept = 0;
        [[nodiscard]] virtual Result<void> Prepare(const AudioDSPPrepareContext &context) = 0;
        [[nodiscard]] virtual AudioDSPProcessResult Process(const AudioDSPProcessContext &context) noexcept = 0;
        virtual void Reset() noexcept = 0;
    };
}  // namespace Horo::Audio
