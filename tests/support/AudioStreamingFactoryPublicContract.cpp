#include <Horo/Audio/AudioStreamingService.h>
#include <type_traits>
#include <utility>

namespace {
    using Horo::Audio::AudioStreamingLimits;
    using Horo::Audio::AudioStreamingService;
    using Horo::Audio::AudioStreamPackageSource;
    using Horo::JobSystem;

    static_assert(std::is_same_v<decltype(AudioStreamPackageSource::context), Horo::BorrowedCallbackContext>);
    static_assert(std::is_same_v<decltype(Horo::Audio::AudioStreamDecoderProvider::context), Horo::BorrowedCallbackContext>);
    static_assert(std::is_trivially_copyable_v<Horo::BorrowedCallbackContext>);

    // Consumers may use the checked factory, but cannot manufacture its private authority.
    template <typename Service>
    concept HasCheckedFactory = requires(JobSystem &jobs, AudioStreamPackageSource source, AudioStreamingLimits limits) {
        Service::Create(jobs, std::move(source), limits);
    };

    template <typename Service>
    concept HasBraceKeyBypass = requires(JobSystem &jobs, AudioStreamPackageSource source, AudioStreamingLimits limits) {
        Service({}, jobs, std::move(source), limits);
    };

    static_assert(std::is_final_v<AudioStreamingService>);
    static_assert(!std::is_default_constructible_v<AudioStreamingService>);
    static_assert(!std::is_constructible_v<AudioStreamingService, JobSystem &, AudioStreamPackageSource, AudioStreamingLimits>);
    static_assert(!HasBraceKeyBypass<AudioStreamingService>);
    static_assert(HasCheckedFactory<AudioStreamingService>);
}  // namespace
