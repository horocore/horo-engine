#include "AllocationProbe.h"
#include "Horo/Audio/CoreStereoSpatialRenderer.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

namespace Horo::Audio {
    namespace {
        constexpr float CenterGain = 0.70710678118F;

        AudioSpatialIdentity Identity(const std::uint32_t slot) {
            return {{AudioRuntimeId::Create(1).Value(), 1, 1}, slot, 1};
        }

        struct Scene final {
            AudioSpatialSource source;
            AudioSpatialListener listener;
            AudioStereoSpatialSettings settings;

            Scene() {
                source.identity = Identity(1);
                listener.identity = Identity(2);
                source.motion.discontinuous = listener.motion.discontinuous = false;
                source.motion.current.position = {0.0F, 0.0F, -1.0F};
                settings.smoothingFrames = 0;
            }

            AudioStereoSpatialTarget Target(const std::uint32_t channels = 1) const {
                auto result = PrepareAudioStereoSpatialTarget(source, &listener, settings, channels);
                REQUIRE(result.HasValue());
                return result.Value();
            }

            void Update(CoreStereoSpatialRenderer &renderer) const {
                REQUIRE(renderer.Update(source, &listener, settings).HasValue());
            }
        };

        CoreStereoSpatialRenderer Renderer(const std::uint32_t channels = 1) {
            auto result = CoreStereoSpatialRenderer::Create({AudioResamplerStage::ClipToMix, AudioResamplerQuality::Linear, 48000, 48000,
                                                             channels, 256},
                                                            1'000'000);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        struct Buffers final {
            alignas(64) std::array<float, 1024> left{};
            alignas(64) std::array<float, 1024> right{};
            alignas(64) std::array<float, 256> outputLeft{};
            alignas(64) std::array<float, 256> outputRight{};
            std::array<std::span<const float>, 2> inputs{left, right};
            std::array<std::span<float>, 2> outputs{outputLeft, outputRight};

            AudioResamplerProgress Run(CoreStereoSpatialRenderer &renderer, const std::uint32_t channels = 1,
                                       const std::uint32_t frames = 512, const std::uint32_t capacity = 128, const bool end = false) {
                return renderer.Process({{inputs.data(), channels}, frames, end}, {outputs, capacity});
            }
        };

        TEST_CASE("Reference spatial scenes normalize distance curves and listener-relative pan", "[audio][spatial][core]") {
            Scene scene;
            scene.settings.minimumDistance = 2.0F;
            scene.settings.maximumDistance = 10.0F;
            for (const auto curve : {AudioDistanceCurve::Linear, AudioDistanceCurve::Inverse, AudioDistanceCurve::InverseSquare}) {
                scene.settings.curve = curve;
                for (const float distance : {0.0F, 1.0F, 2.0F, 6.0F, 10.0F, 100.0F}) {
                    scene.source.motion.current.position = {0.0F, 0.0F, -distance};
                    const auto target = scene.Target();
                    const double midpoint = curve == AudioDistanceCurve::Linear    ? 0.5
                                            : curve == AudioDistanceCurve::Inverse ? 1.0 / 6.0
                                                                                   : 2.0 / 27.0;
                    const double gain = distance <= 2 ? 1.0 : distance >= 10 ? 0.0 : midpoint;
                    CHECK(target.attenuation == Catch::Approx(gain).margin(1e-6));
                    CHECK(target.matrix[0] == Catch::Approx(gain * CenterGain).margin(1e-6));
                    CHECK(target.matrix[2] == Catch::Approx(gain * CenterGain).margin(1e-6));
                }
            }
            scene.settings = {};
            for (const auto position : {Math::Vec3{-1, 0, 0}, Math::Vec3{0, 0, -1}, Math::Vec3{1, 0, 0}, Math::Vec3{0, 1, 0}}) {
                scene.source.motion.current.position = position;
                const auto target = scene.Target();
                CHECK(target.pan == Catch::Approx(position.x));
                CHECK(target.matrix[0] * target.matrix[0] + target.matrix[2] * target.matrix[2] == Catch::Approx(1.0));
            }
            scene.listener.motion.current.orientation = Math::Quaternion::FromAxisAngle({0, 1, 0}, Math::Pi / 2);
            scene.source.motion.current.position = {0, 0, -1};
            CHECK(scene.Target().pan == Catch::Approx(1.0).margin(1e-6));
        }

        TEST_CASE("Directivity cones interpolate full angles and spread and width render PCM", "[audio][spatial][core]") {
            Scene scene;
            scene.settings.innerConeRadians = Math::Pi / 3;
            scene.settings.outerConeRadians = Math::Pi;
            scene.settings.outerConeGain = 0.2F;
            scene.source.motion.current.position = {0, 0, 1};  // forward points toward listener
            CHECK(scene.Target().attenuation == Catch::Approx(1.0));
            scene.source.motion.current.orientation = Math::Quaternion::FromAxisAngle({0, 1, 0}, Math::Pi / 3);
            CHECK(scene.Target().attenuation == Catch::Approx(0.6).margin(1e-6));
            scene.source.motion.current.orientation = Math::Quaternion::FromAxisAngle({0, 1, 0}, Math::Pi);
            CHECK(scene.Target().attenuation == Catch::Approx(0.2).margin(1e-6));
            scene.source.motion.current.position = {};
            CHECK(scene.Target().attenuation == 1.0F);
            scene.settings = {};
            auto stereo = Renderer(2);
            Buffers buffers;
            buffers.left.fill(1.0F);
            buffers.right.fill(0.25F);
            scene.Update(stereo);
            REQUIRE(buffers.Run(stereo, 2).produced == 128);
            CHECK(buffers.outputLeft[0] == Catch::Approx(1.0));
            CHECK(buffers.outputRight[0] == Catch::Approx(0.25));
            scene.settings.width = 0;
            scene.Update(stereo);
            stereo.Reset();
            REQUIRE(buffers.Run(stereo, 2).produced == 128);
            CHECK(buffers.outputLeft[0] == Catch::Approx(0.625));
            CHECK(buffers.outputRight[0] == Catch::Approx(0.625));
            auto mono = Renderer();
            scene.source.motion.current.position = {1, 0, 0};
            scene.settings.spread = 1;
            scene.Update(mono);
            REQUIRE(buffers.Run(mono).produced == 128);
            CHECK(buffers.outputLeft[0] == Catch::Approx(CenterGain));
            CHECK(buffers.outputRight[0] == Catch::Approx(CenterGain));
        }

        TEST_CASE("Source and listener velocities produce bounded Doppler and actual pitch-shifted PCM", "[audio][spatial][core]") {
            Scene scene;
            scene.source.playback.enableDoppler = true;
            scene.source.motion.current.position = {1, 0, 0};
            scene.settings.speedOfSound = 100;
            scene.source.motion.velocity = {-20, 0, 0};
            CHECK(scene.Target().pitch == Catch::Approx(1.25));
            scene.listener.motion.velocity = {10, 0, 0};
            CHECK(scene.Target().pitch == Catch::Approx(1.375));
            scene.source.motion.velocity = {20, 0, 0};
            scene.listener.motion.velocity = {};
            CHECK(scene.Target().pitch == Catch::Approx(1.0 / 1.2));
            scene.source.motion.velocity = {0, 20, 0};
            CHECK(scene.Target().pitch == 1.0);
            scene.source.motion.velocity = {-20, 0, 0};
            auto renderer = Renderer();
            scene.Update(renderer);  // fresh identity suppresses Doppler
            CHECK(renderer.Target().pitch == 1.0);
            scene.Update(renderer);
            CHECK(renderer.Target().pitch == Catch::Approx(1.25));
            Buffers buffers;
            for (std::size_t frame = 0; frame < buffers.left.size(); ++frame)
                buffers.left[frame] = static_cast<float>(frame);
            REQUIRE(buffers.Run(renderer).produced == 128);
            for (std::size_t frame = 0; frame < 128; ++frame) {
                CHECK(buffers.outputLeft[frame] == 0.0F);
                CHECK(buffers.outputRight[frame] == Catch::Approx(frame * 1.25).margin(1e-4));
            }
            for (const float speed : {-std::numeric_limits<float>::max(), std::numeric_limits<float>::max()}) {
                scene.source.motion.velocity.x = speed;
                scene.listener.motion.velocity.x = -speed;
                CHECK(std::isfinite(scene.Target().pitch));
                CHECK(scene.Target().pitch >= 0.125);
                CHECK(scene.Target().pitch <= 8.0);
            }
            scene.source.motion.discontinuous = true;
            CHECK(scene.Target().pitch == 1.0);
            scene.source.motion.discontinuous = false;
            scene.listener.motion.discontinuous = true;
            CHECK(scene.Target().pitch == 1.0);
        }

        TEST_CASE("Teleport revisions snap pitch and retain gain smoothing with no callback allocations", "[audio][spatial][core]") {
            Scene scene;
            scene.source.playback.enableDoppler = true;
            scene.settings.speedOfSound = 100;
            scene.source.motion.current.position = {1, 0, 0};
            scene.source.motion.velocity = {-20, 0, 0};
            auto renderer = Renderer();
            scene.Update(renderer);
            scene.Update(renderer);
            Buffers buffers;
            buffers.left.fill(1);
            REQUIRE(buffers.Run(renderer).produced == 128);
            scene.source.motion.current.position = {-1, 0, 0};
            scene.source.motion.discontinuityRevision = 1;
            scene.settings.smoothingFrames = 8;
            scene.Update(renderer);
            CHECK(renderer.Target().pitch == 1.0);
            const auto before = Tests::AllocationProbe::Count();
            const auto frees = Tests::AllocationProbe::FreeCount();
            const auto result = buffers.Run(renderer);
            const auto after = Tests::AllocationProbe::Count();
            const auto afterFrees = Tests::AllocationProbe::FreeCount();
            REQUIRE(result.produced == 128);
            CHECK(after == before);
            CHECK(afterFrees == frees);
            for (std::size_t frame = 0; frame < 8; ++frame) {
                CHECK(buffers.outputLeft[frame] == Catch::Approx((frame + 1) / 8.0));
                CHECK(buffers.outputRight[frame] == Catch::Approx(1.0 - (frame + 1) / 8.0));
            }
            scene.source.identity.generation = 2;
            scene.Update(renderer);
            CHECK(renderer.Target().pitch == 1.0);
        }

        TEST_CASE("Spatial ramps and resampling are invariant to callback block partition", "[audio][spatial][core]") {
            Scene scene;
            scene.source.playback.enableDoppler = true;
            scene.settings.speedOfSound = 100;
            auto whole = Renderer();
            auto split = Renderer();
            scene.Update(whole);
            scene.Update(split);
            scene.source.motion.current.position = {1, 0, 0};
            scene.source.motion.velocity = {-20, 0, 0};
            scene.settings.smoothingFrames = 128;
            scene.Update(whole);
            scene.Update(split);
            Buffers all;
            Buffers piece;
            for (std::size_t frame = 0; frame < all.left.size(); ++frame)
                all.left[frame] = static_cast<float>(std::sin(frame * 0.03));
            REQUIRE(all.Run(whole, 1, 512, 128).produced == 128);
            std::uint32_t consumed = 0;
            for (std::uint32_t offset = 0; offset < 128; offset += 16) {
                std::copy_n(all.left.begin() + consumed, 512 - consumed, piece.left.begin());
                const auto result = piece.Run(split, 1, 512 - consumed, 16);
                REQUIRE(result.produced == 16);
                consumed += result.consumed;
                for (std::uint32_t frame = 0; frame < 16; ++frame) {
                    CHECK(piece.outputLeft[frame] == Catch::Approx(all.outputLeft[offset + frame]).margin(1e-6));
                    CHECK(piece.outputRight[frame] == Catch::Approx(all.outputRight[offset + frame]).margin(1e-6));
                }
            }
        }

        TEST_CASE("2D rendering ignores listeners and spatial geometry and preserves stereo", "[audio][spatial][core]") {
            Scene scene;
            scene.source.playback.spatialMode = AudioSpatialMode::TwoD;
            scene.source.playback.enableDoppler = true;
            scene.source.motion.velocity.x = std::numeric_limits<float>::quiet_NaN();
            scene.settings.maximumDistance = -1;
            auto renderer = Renderer(2);
            REQUIRE(renderer.Update(scene.source, nullptr, scene.settings).HasValue());
            Buffers buffers;
            buffers.left.fill(2);
            buffers.right.fill(-3);
            REQUIRE(buffers.Run(renderer, 2).produced == 128);
            CHECK(buffers.outputLeft[0] == 2.0F);
            CHECK(buffers.outputRight[0] == -3.0F);
            CHECK(renderer.Target().pitch == 1.0);
        }

        TEST_CASE("Malformed spatial updates and overlapping buffers reject transactionally", "[audio][spatial][core]") {
            Scene scene;
            auto renderer = Renderer();
            scene.Update(renderer);
            const auto prior = renderer.Target();
            scene.settings.minimumDistance = 0;
            REQUIRE(renderer.Update(scene.source, &scene.listener, scene.settings).HasError());
            CHECK(renderer.Target().matrix == prior.matrix);
            scene.settings = {};
            scene.source.motion.current.orientation = {0, 0, 0, 0};
            REQUIRE(renderer.Update(scene.source, &scene.listener, scene.settings).HasError());
            scene.source.motion.current.orientation = {};
            REQUIRE(renderer.Update(scene.source, nullptr, scene.settings).HasError());
            Buffers buffers;
            buffers.outputLeft.fill(9);
            buffers.outputs[1] = buffers.outputLeft;
            CHECK(buffers.Run(renderer).status == AudioResamplerStatus::InvalidBuffer);
            CHECK(buffers.outputLeft[0] == 9);
            buffers.outputs[1] = buffers.outputRight;
            buffers.outputs[0] = std::span<float>{buffers.left}.first(256);
            CHECK(buffers.Run(renderer).status == AudioResamplerStatus::InvalidBuffer);
            buffers.outputs[0] = buffers.outputLeft;
            buffers.inputs[0] = std::span<const float>{buffers.left}.subspan(1);
            CHECK(buffers.Run(renderer).status == AudioResamplerStatus::InvalidBuffer);
            buffers.inputs[0] = buffers.left;
            CHECK(buffers.Run(renderer, 1, 512, 0).produced == 0);
        }

        TEST_CASE("Spatial admission rejects unsupported conversion and hostile control values", "[audio][spatial][core]") {
            AudioResamplerDescriptor descriptor{AudioResamplerStage::ClipToMix, AudioResamplerQuality::Linear, 48000, 48000, 1, 256};
            CHECK(CoreStereoSpatialRenderer::Create(descriptor, 0).HasError());
            descriptor.quality = AudioResamplerQuality::Sinc32;
            CHECK(CoreStereoSpatialRenderer::Create(descriptor, 1'000'000).HasError());
            descriptor.quality = AudioResamplerQuality::Linear;
            descriptor.channels = 3;
            CHECK(CoreStereoSpatialRenderer::Create(descriptor, 1'000'000).HasError());
            Scene scene;
            const auto invalid = [&] {
                return PrepareAudioStereoSpatialTarget(scene.source, &scene.listener, scene.settings, 1).HasError();
            };
            scene.settings.outerConeRadians = -1;
            CHECK(invalid());
            scene.settings = {};
            scene.settings.width = std::numeric_limits<float>::quiet_NaN();
            CHECK(invalid());
            scene.settings = {};
            scene.source.playback.gain = 17;
            CHECK(invalid());
            scene.source.playback.gain = 1;
            scene.source.playback.pitch = 0;
            CHECK(invalid());
            scene.source.playback.pitch = 1;
            scene.source.motion.current.position.x = std::numeric_limits<float>::max();
            scene.listener.motion.current.position.x = -std::numeric_limits<float>::max();
            CHECK(scene.Target().attenuation == 0);
            scene.source.motion.current.position = {0, 0, -1};
            scene.listener.motion.current.position = {};
            scene.settings.minimumDistance = 1;
            scene.settings.maximumDistance = std::nextafter(1.0F, 2.0F);
            scene.settings.curve = AudioDistanceCurve::InverseSquare;
            CHECK(scene.Target().attenuation == 1);
            auto renderer = Renderer();
            scene.source.identity = {};
            CHECK(renderer.Update(scene.source, &scene.listener, scene.settings).HasError());
            Buffers buffers;
            CHECK(buffers.Run(renderer).status == AudioResamplerStatus::InvalidState);
        }

        TEST_CASE("Spatial stream EOF drains bounded tail and sanitizes hostile PCM", "[audio][spatial][core]") {
            Scene scene;
            auto renderer = Renderer();
            scene.Update(renderer);
            Buffers buffers;
            CHECK(buffers.Run(renderer, 1, 0, 128, true).status == AudioResamplerStatus::Complete);
            renderer.Reset();
            buffers.left.fill(1);
            buffers.left[0] = std::numeric_limits<float>::quiet_NaN();
            const auto result = buffers.Run(renderer, 1, 16, 128, true);
            CHECK(result.status == AudioResamplerStatus::Complete);
            CHECK(result.consumed == 16);
            CHECK(result.produced <= 18);
            CHECK(result.sanitizedSamples > 0);
            for (std::uint32_t frame = 0; frame < result.produced; ++frame) {
                CHECK(std::isfinite(buffers.outputLeft[frame]));
                CHECK(std::isfinite(buffers.outputRight[frame]));
            }
            CHECK(buffers.Run(renderer, 1, 0, 128, true).produced == 0);
        }
    }  // namespace
}  // namespace Horo::Audio
