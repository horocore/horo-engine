#include "Horo/Audio/MixerGraphCompiler.h"

#include <type_traits>

static_assert(std::is_trivially_copyable_v<Horo::Audio::MixerSnapshotAsset>);
static_assert(std::is_trivially_copyable_v<Horo::Audio::PreparedMixerSnapshot>);
static_assert(!std::is_copy_constructible_v<Horo::Audio::MixerSnapshotTransitions>);
static_assert(Horo::Audio::MaximumMixerSnapshotParameters * 2 <= Horo::Audio::MaximumScheduledAudioCommands);

static_assert(std::is_trivially_copyable_v<Horo::Audio::AudioAutomationValueSelector>);
static_assert(requires(Horo::Audio::MixerRenderPlan &plan, std::span<const Horo::Audio::AudioAutomationParameter> bindings) {
    plan.BindAutomation(bindings);
});

static_assert(std::is_trivially_copyable_v<Horo::Audio::MixerSnapshotTransitionRequest>);
