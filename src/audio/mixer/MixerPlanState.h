#pragma once

#include "Horo/Audio/MixerGraphCompiler.h"

#include <array>
#include <new>

namespace Horo::Audio {
    struct MixerRenderPlan::ConstructionKey final {};

    namespace MixerDetail {
        /** @brief Only control destruction releases the plan's aligned backing allocation. */
        struct StorageDelete final {
            void operator()(std::byte *bytes) const noexcept {
                ::operator delete[](bytes, std::align_val_t{AudioDSPMemoryAlignment});
            }
        };

        using Storage = std::unique_ptr<std::byte, StorageDelete>;

        /** @brief Fixed storage and immutable descriptor for one prepared insert. */
        struct Insert final {
            MixerEffectDescriptor effect; /**< Retained stable identity and authored parameters, independent of asset lifetime. */
            std::unique_ptr<IAudioDSPNode> node;
            AudioDSPNodeDescriptor descriptor;
            std::vector<AudioDSPParameterValue> parameters;
            std::size_t stateOffset{};
        };

        /** @brief Callback-exclusive prepared bus bindings; pointer arrays never resize after compilation. */
        struct BusState final {
            AudioProcessingFormat format;
            std::array<AudioSample *, MaximumAudioChannels> pre{};
            std::array<AudioSample *, MaximumAudioChannels> post{};
            std::array<AudioSample *, MaximumAudioChannels> work{};
            std::vector<Insert> inserts;
            float gain{1.0F}; /**< Callback value, separate from the immutable compiled fader descriptor. */
            bool muted{};
        };

        /** @brief Mutable route value and immutable authored route role retained by the prepared plan. */
        struct RouteState final {
            float gain{1.0F};
            MixerRouteKind kind{MixerRouteKind::Primary};
        };

        /** @brief Control-resolved physical cells; every pointer belongs to this retained plan generation. */
        struct ParameterProjection final {
            AudioAutomationParameter binding;
            float *value{};
            float *target{};
        };

        /** @brief Align a previously bounded reservation size to the DSP storage contract. */
        inline std::size_t Align(const std::size_t bytes) noexcept {
            return (bytes + AudioDSPMemoryAlignment - 1) & ~(AudioDSPMemoryAlignment - 1);
        }

        /** @brief Fixed callback entry; metadata and storage were admitted before publication. */
        MixerRenderStatus RenderPlan(MixerRenderPlan::State &state, std::span<const MixerVoiceInput> voices,
                                     const AudioPlanarBlockView &output) noexcept;
        /** @brief Validate exact sealed binding and clock before touching retained signal state. */
        bool ValidateAutomationContext(const MixerRenderPlan::State &state, const MixerAutomationRenderContext &context,
                                       std::uint32_t frames, std::span<AudioAutomationValueSelector> selectors) noexcept;
        /** @brief Write the complete prepared projection at one sample. */
        bool ProjectAutomation(MixerRenderPlan::State &state, const AudioParameterAutomation &automation,
                               std::span<const AudioAutomationValueSelector> selectors) noexcept;
        /** @brief Sample automation into physical cells before processing the actual compiled graph. */
        MixerRenderResult RenderAutomatedPlan(MixerRenderPlan::State &state, std::span<const MixerVoiceInput> voices,
                                              const AudioPlanarBlockView &output, const MixerAutomationRenderContext &automation) noexcept;
        /** @brief Write canonical silence only within caller-admitted storage. */
        void Silence(const AudioPlanarBlockView &output) noexcept;
        /** @brief Shared control-only profile admission, avoiding divergent runtime/compiler validation. */
        Result<void> ValidateProfile(const MixerCompileProfile &profile);
        /** @brief Bound invalid-graph diagnostics to one deterministic stable-ID cycle, if endpoints are resolvable. */
        std::string CycleEvidence(const MixerAssetSchema &asset);
    }  // namespace MixerDetail

    /** @brief Detached metadata and storage; nodes are destroyed before the storage they borrow. */
    struct MixerRenderPlan::State final {
        MixerPlanIdentity identity;
        MixerCompileProfile profile;
        std::vector<MixerCompiledBus> buses;
        std::vector<MixerCompiledRoute> routes;
        std::size_t storageBytes{};
        std::size_t scratchOffset{};
        std::size_t scratchBytes{};
        MixerDetail::Storage storage;
        std::vector<MixerDetail::BusState> processing;
        std::vector<MixerDetail::RouteState> routeState;
        std::vector<MixerDetail::ParameterProjection> projections;
        std::size_t metadataBytes{};
        std::uint64_t sampleOperations{};
        bool automationBound{};

        AudioMemoryHandle handle; /**< Assigned once before release-publication, never changed while visible. */
        std::uint64_t sequence{};
    };
}  // namespace Horo::Audio
