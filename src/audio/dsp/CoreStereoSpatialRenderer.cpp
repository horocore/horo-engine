#include "Horo/Audio/CoreStereoSpatialRenderer.h"

#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

namespace Horo::Audio {
    namespace {
        /** @brief Admit a closed finite interval without allowing NaN through comparisons. */
        bool InRange(const float value, const float low, const float high) noexcept {
            return std::isfinite(value) && value >= low && value <= high;
        }

        /** @brief Validate only the geometry used by control-side spatial evaluation. */
        bool ValidMotion(const AudioSpatialMotion &motion) noexcept {
            const auto q = motion.current.orientation;
            const double norm = static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y + static_cast<double>(q.z) * q.z +
                                static_cast<double>(q.w) * q.w;
            return Math::IsFinite(motion.current.position) && Math::IsFinite(motion.velocity) && std::isfinite(norm) &&
                   std::abs(norm - 1.0) <= 0.0001;
        }

        /** @brief Reject malformed curves/cones before evaluating any divisions or angles. */
        bool ValidSettings(const AudioStereoSpatialSettings &settings) noexcept {
            return settings.curve <= AudioDistanceCurve::InverseSquare && std::isfinite(settings.minimumDistance) &&
                   settings.minimumDistance > 0.0F && std::isfinite(settings.maximumDistance) &&
                   settings.maximumDistance > settings.minimumDistance && InRange(settings.innerConeRadians, 0.0F, 2.0F * Math::Pi) &&
                   InRange(settings.outerConeRadians, settings.innerConeRadians, 2.0F * Math::Pi) &&
                   InRange(settings.outerConeGain, 0.0F, 1.0F) && InRange(settings.spread, 0.0F, 1.0F) &&
                   InRange(settings.width, 0.0F, 1.0F) && std::isfinite(settings.speedOfSound) && settings.speedOfSound > 0.0F &&
                   InRange(settings.dopplerScale, 0.0F, 4.0F) && settings.smoothingFrames <= 16384;
        }

        /** @brief Double-precision geometry avoids overflow for finite extreme world positions. */
        struct Separation final {
            Math::Vec3 direction;
            double distance{};
        };

        /** @brief Normalize listener-to-source direction; coincident sources have no directional preference. */
        Separation Separate(const Math::Vec3 source, const Math::Vec3 listener) noexcept {
            const double x = static_cast<double>(source.x) - listener.x;
            const double y = static_cast<double>(source.y) - listener.y;
            const double z = static_cast<double>(source.z) - listener.z;
            const double distance = std::hypot(x, y, z);
            if (distance == 0.0)
                return {};
            return {{static_cast<float>(x / distance), static_cast<float>(y / distance), static_cast<float>(z / distance)}, distance};
        }

        /** @brief Normalize distance curves continuously to both authored endpoints. */
        float DistanceGain(const double distance, const AudioStereoSpatialSettings &settings) noexcept {
            const double minimum = settings.minimumDistance;
            const double maximum = settings.maximumDistance;
            if (distance <= minimum)
                return 1.0F;
            if (distance >= maximum)
                return 0.0F;
            if (settings.curve == AudioDistanceCurve::Linear)
                return static_cast<float>((maximum - distance) / (maximum - minimum));
            const double exponent = settings.curve == AudioDistanceCurve::Inverse ? 1.0 : 2.0;
            // expm1 retains precision when authored endpoints are adjacent representable floats.
            const double numerator = std::expm1(exponent * std::log(maximum / distance));
            const double denominator = std::expm1(exponent * std::log(maximum / minimum));
            return static_cast<float>(numerator / denominator);
        }

        /** @brief Source forward is negative Z; cone angles are full apex angles. */
        float ConeGain(const AudioSpatialSource &source, const Separation separation, const AudioStereoSpatialSettings &settings) noexcept {
            if (separation.distance == 0.0)
                return 1.0F;
            const auto forward = source.motion.current.orientation.Rotate({0.0F, 0.0F, -1.0F});
            const float angle = std::acos(std::clamp(Math::Dot(forward, -separation.direction), -1.0F, 1.0F));
            const float inner = settings.innerConeRadians * 0.5F;
            const float outer = settings.outerConeRadians * 0.5F;
            if (angle <= inner)
                return 1.0F;
            if (angle >= outer)
                return settings.outerConeGain;
            return 1.0F + (settings.outerConeGain - 1.0F) * (angle - inner) / (outer - inner);
        }

        /** @brief Clamp radial speeds before division and bound the final authored-pitch product. */
        double Doppler(const AudioSpatialSource &source, const AudioSpatialListener &listener, const Separation separation,
                       const AudioStereoSpatialSettings &settings) noexcept {
            if (!source.playback.enableDoppler || source.motion.discontinuous || listener.motion.discontinuous ||
                separation.distance == 0.0)
                return 1.0;
            const auto radial = [&](const Math::Vec3 velocity) {
                const double projection = static_cast<double>(velocity.x) * separation.direction.x +
                                          static_cast<double>(velocity.y) * separation.direction.y +
                                          static_cast<double>(velocity.z) * separation.direction.z;
                const double limit = 0.9 * settings.speedOfSound;
                return std::clamp(projection * settings.dopplerScale, -limit, limit);
            };
            return (settings.speedOfSound + radial(listener.motion.velocity)) / (settings.speedOfSound + radial(source.motion.velocity));
        }

        /** @brief Exact hard-pan endpoints avoid tiny residuals; ordinary mono pan preserves equal power. */
        std::array<float, 2> Pan(const float pan) noexcept {
            if (pan <= -1.0F)
                return {1.0F, 0.0F};
            if (pan >= 1.0F)
                return {0.0F, 1.0F};
            const float angle = (pan + 1.0F) * Math::Pi * 0.25F;
            return {std::cos(angle), std::sin(angle)};
        }

        /** @brief Compare bounded sample ranges without pointer ordering across allocations. */
        bool Overlap(const std::uintptr_t left, const std::uint64_t leftBytes, const std::uintptr_t right,
                     const std::uint64_t rightBytes) noexcept {
            if (leftBytes == 0 || rightBytes == 0)
                return false;
            return left <= right ? right - left < leftBytes : left - right < rightBytes;
        }

        /** @brief Use a total address representation solely for validating borrowed sample ranges. */
        std::uintptr_t SampleAddress(const float *sample) noexcept {
            return reinterpret_cast<std::uintptr_t>(sample);
        }

        /** @brief Validate all call memory before either stream history or samples change. */
        bool ValidBuffers(const AudioResamplerInput input, const AudioResamplerOutput output, const AudioResamplerDescriptor &descriptor,
                          const CoreStereoSpatialRenderer &owner) noexcept {
            if (input.planes.size() != descriptor.channels || output.planes.size() != 2 ||
                input.frames > descriptor.maximumOutputFrames * 64U + 2 || output.capacity > descriptor.maximumOutputFrames)
                return false;
            const auto valid = [&](const auto plane, const std::uint32_t frames) {
                return plane.size() >= frames &&
                       (frames == 0 || (plane.data() != nullptr && reinterpret_cast<std::uintptr_t>(plane.data()) % 64 == 0)) &&
                       !Overlap(SampleAddress(plane.data()), static_cast<std::uint64_t>(frames) * sizeof(float),
                                reinterpret_cast<std::uintptr_t>(&owner), sizeof(owner));
            };
            for (const auto plane : input.planes)
                if (!valid(plane, input.frames))
                    return false;
            for (std::size_t channel = 0; channel < output.planes.size(); ++channel) {
                const auto plane = output.planes[channel];
                if (!valid(plane, output.capacity))
                    return false;
                for (const auto source : input.planes)
                    if (Overlap(SampleAddress(plane.data()), static_cast<std::uint64_t>(output.capacity) * sizeof(float),
                                SampleAddress(source.data()), static_cast<std::uint64_t>(input.frames) * sizeof(float)))
                        return false;
                for (std::size_t earlier = 0; earlier < channel; ++earlier)
                    if (Overlap(SampleAddress(plane.data()), static_cast<std::uint64_t>(output.capacity) * sizeof(float),
                                SampleAddress(output.planes[earlier].data()), static_cast<std::uint64_t>(output.capacity) * sizeof(float)))
                        return false;
            }
            return true;
        }

        /** @brief Preserve headroom while replacing overflow/subnormals with positive-zero silence. */
        float SafeOutput(const double value, std::uint32_t &sanitized) noexcept {
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
                ++sanitized;
                return 0.0F;
            }
            return std::abs(value) < std::numeric_limits<float>::min() ? 0.0F : static_cast<float>(value);
        }
    }  // namespace

    /** @copydoc PrepareAudioStereoSpatialTarget */
    Result<AudioStereoSpatialTarget> PrepareAudioStereoSpatialTarget(const AudioSpatialSource &source, const AudioSpatialListener *listener,
                                                                     const AudioStereoSpatialSettings &settings,
                                                                     const std::uint32_t channels) {
        if ((channels != 1 && channels != 2) || !InRange(source.playback.gain, 0.0F, 16.0F) || !std::isfinite(source.playback.pitch) ||
            source.playback.pitch <= 0.0F || source.playback.pitch > 8.0F || source.playback.spatialMode > AudioSpatialMode::ThreeD)
            return Result<AudioStereoSpatialTarget>::Failure(MakeError(AudioErrors::ResamplerInvalid));
        AudioStereoSpatialTarget target{.pitch = std::clamp(static_cast<double>(source.playback.pitch), 0.125, 8.0)};
        if (source.playback.spatialMode == AudioSpatialMode::TwoD) {
            if (channels == 1) {
                const auto pan = Pan(0.0F);
                target.matrix = {pan[0] * source.playback.gain, 0.0F, pan[1] * source.playback.gain, 0.0F};
            } else {
                target.matrix = {source.playback.gain, 0.0F, 0.0F, source.playback.gain};
            }
            return Result<AudioStereoSpatialTarget>::Success(target);
        }
        if (!listener || !ValidMotion(source.motion) || !ValidMotion(listener->motion) || !ValidSettings(settings))
            return Result<AudioStereoSpatialTarget>::Failure(MakeError(AudioErrors::ResamplerInvalid));
        const auto separation = Separate(source.motion.current.position, listener->motion.current.position);
        const auto right = listener->motion.current.orientation.Rotate({1.0F, 0.0F, 0.0F});
        target.pan = std::clamp(Math::Dot(separation.direction, right), -1.0F, 1.0F);
        target.attenuation = DistanceGain(separation.distance, settings) * ConeGain(source, separation, settings);
        target.pitch = std::clamp(source.playback.pitch * Doppler(source, *listener, separation, settings), 0.125, 8.0);
        const float gain = source.playback.gain * target.attenuation;
        if (channels == 1) {
            const auto pan = Pan(target.pan * (1.0F - settings.spread));
            target.matrix = {gain * pan[0], 0.0F, gain * pan[1], 0.0F};
        } else {
            const auto left = Pan(std::clamp(target.pan - settings.width, -1.0F, 1.0F) * (1.0F - settings.spread));
            const auto rightPan = Pan(std::clamp(target.pan + settings.width, -1.0F, 1.0F) * (1.0F - settings.spread));
            const float scale = gain * (std::sqrt(0.5F) + (1.0F - std::sqrt(0.5F)) * settings.width);
            target.matrix = {scale * left[0], scale * rightPan[0], scale * left[1], scale * rightPan[1]};
        }
        return Result<AudioStereoSpatialTarget>::Success(target);
    }

    /** @copydoc CoreStereoSpatialRenderer::Create */
    Result<CoreStereoSpatialRenderer> CoreStereoSpatialRenderer::Create(const AudioResamplerDescriptor &descriptor,
                                                                        const std::uint64_t maximumCoefficientBytes) {
        if (descriptor.stage != AudioResamplerStage::ClipToMix || descriptor.quality != AudioResamplerQuality::Linear ||
            (descriptor.channels != 1 && descriptor.channels != 2) || descriptor.pitch != 1.0 || descriptor.playbackSpeed != 1.0)
            return Result<CoreStereoSpatialRenderer>::Failure(MakeError(AudioErrors::ResamplerInvalid));
        const AudioResamplerBudget budget{2ULL * descriptor.channels * descriptor.maximumOutputFrames,
                                          2ULL * descriptor.channels * sizeof(float), 1};
        auto plan = AudioResamplerPlan::Prepare(descriptor, budget);
        if (plan.HasError())
            return Result<CoreStereoSpatialRenderer>::Failure(plan.ErrorValue());
        auto converter = AudioResampler::Create(plan.Value(), maximumCoefficientBytes);
        if (converter.HasError())
            return Result<CoreStereoSpatialRenderer>::Failure(converter.ErrorValue());
        return Result<CoreStereoSpatialRenderer>::Success(CoreStereoSpatialRenderer{std::move(converter).Value(), descriptor});
    }

    /** @copydoc CoreStereoSpatialRenderer::CoreStereoSpatialRenderer */
    CoreStereoSpatialRenderer::CoreStereoSpatialRenderer(AudioResampler converter, const AudioResamplerDescriptor &descriptor) noexcept
        : converter_(std::move(converter)), descriptor_(descriptor) {}

    /** @copydoc CoreStereoSpatialRenderer::Update */
    Result<void> CoreStereoSpatialRenderer::Update(const AudioSpatialSource &source, const AudioSpatialListener *listener,
                                                   const AudioStereoSpatialSettings &settings) {
        const bool threeD = source.playback.spatialMode == AudioSpatialMode::ThreeD;
        if (!converter_.Plan() || !source.identity.IsValid() || (threeD && (!listener || !listener->identity.IsValid())) ||
            settings.smoothingFrames > 16384)
            return Result<void>::Failure(MakeError(AudioErrors::ResamplerInvalid));
        AudioSpatialSource copied = source;
        const AudioSpatialIdentity listenerIdentity = threeD ? listener->identity : AudioSpatialIdentity{};
        const std::uint64_t listenerRevision = threeD ? listener->motion.discontinuityRevision : 0;
        const bool changedIdentity = !initialized_ || source.identity != source_ || listenerIdentity != listener_;
        const bool teleport = source.motion.discontinuous || (threeD && listener->motion.discontinuous) ||
                              source.motion.discontinuityRevision != sourceRevision_ || listenerRevision != listenerRevision_;
        if (teleport || changedIdentity)
            copied.motion.discontinuous = true;
        auto prepared = PrepareAudioStereoSpatialTarget(copied, threeD ? listener : nullptr, settings, descriptor_.channels);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        const auto &next = prepared.Value();
        // Pitch admission is transactional. Reset cannot precede a potentially rejected rate/pitch combination.
        if (!converter_.SetLinearPitch(next.pitch, teleport || changedIdentity ? 0 : settings.smoothingFrames))
            return Result<void>::Failure(MakeError(AudioErrors::ResamplerInvalid));
        if (changedIdentity) {
            converter_.Reset();
            (void)converter_.SetLinearPitch(next.pitch);
        }
        target_ = next;
        remaining_ = changedIdentity ? 0 : settings.smoothingFrames;
        if (remaining_ == 0)
            matrix_ = target_.matrix;
        source_ = source.identity;
        listener_ = listenerIdentity;
        sourceRevision_ = source.motion.discontinuityRevision;
        listenerRevision_ = listenerRevision;
        initialized_ = true;
        return Result<void>::Success();
    }

    /** @copydoc CoreStereoSpatialRenderer::Target */
    AudioStereoSpatialTarget CoreStereoSpatialRenderer::Target() const noexcept {
        return target_;
    }

    /** @copydoc CoreStereoSpatialRenderer::Reset */
    void CoreStereoSpatialRenderer::Reset() noexcept {
        converter_.Reset();
        (void)converter_.SetLinearPitch(target_.pitch);
        matrix_ = target_.matrix;
        remaining_ = 0;
    }

    /** @copydoc CoreStereoSpatialRenderer::EmitStereo */
    void CoreStereoSpatialRenderer::EmitStereo(const AudioResamplerOutput output, AudioResamplerProgress &progress) noexcept {
        if (remaining_ != 0) {
            const auto remaining = static_cast<float>(remaining_);
            for (std::size_t coefficient = 0; coefficient < matrix_.size(); ++coefficient)
                matrix_[coefficient] += (target_.matrix[coefficient] - matrix_[coefficient]) / remaining;
            --remaining_;
        }
        const double left = outputCells_[0].samples[0];
        const double right = descriptor_.channels == 2 ? outputCells_[1].samples[0] : 0.0;
        output.planes[0][progress.produced] = SafeOutput(left * matrix_[0] + right * matrix_[1], progress.sanitizedSamples);
        output.planes[1][progress.produced] = SafeOutput(left * matrix_[2] + right * matrix_[3], progress.sanitizedSamples);
        ++progress.produced;
    }

    /** @copydoc CoreStereoSpatialRenderer::Process */
    AudioResamplerProgress CoreStereoSpatialRenderer::Process(const AudioResamplerInput input, const AudioResamplerOutput output) noexcept {
        using enum AudioResamplerStatus;
        if (!initialized_ || !converter_.Plan())
            return {.status = InvalidState};
        if (!ValidBuffers(input, output, descriptor_, *this))
            return {.status = InvalidBuffer};
        std::array<std::span<const float>, 2> inputs;
        std::array<std::span<float>, 2> outputs;
        for (std::uint32_t channel = 0; channel < descriptor_.channels; ++channel) {
            inputs[channel] = std::span<const float>{inputCells_[channel].samples}.first(1);
            outputs[channel] = std::span<float>{outputCells_[channel].samples}.first(1);
        }
        AudioResamplerProgress progress{.status = OutputFull};
        while (progress.produced < output.capacity) {
            const std::uint32_t offered = progress.consumed < input.frames ? 1 : 0;
            if (offered != 0)
                for (std::uint32_t channel = 0; channel < descriptor_.channels; ++channel)
                    inputCells_[channel].samples[0] = input.planes[channel][progress.consumed];
            const auto result = converter_.Process({{inputs.data(), descriptor_.channels},
                                                    offered,
                                                    input.endOfStream && progress.consumed + offered == input.frames},
                                                   {{outputs.data(), descriptor_.channels}, 1});
            progress.consumed += result.consumed;
            progress.sanitizedSamples += result.sanitizedSamples;
            if (result.produced != 0)
                EmitStereo(output, progress);
            if (result.status == Complete || result.status == InvalidState || result.status == InvalidBuffer ||
                (result.status == InputNeeded && progress.consumed == input.frames)) {
                progress.status = result.status;
                break;
            }
        }
        return progress;
    }
}  // namespace Horo::Audio
