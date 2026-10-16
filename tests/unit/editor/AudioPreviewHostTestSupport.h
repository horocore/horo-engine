#pragma once
#include "EditorAudioPreviewHost.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/Internal/NullAudioBackend.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <fstream>
#include <string_view>
#include <vector>

namespace Horo::Editor::AudioPreviewTest {
    /** @brief Headless-only observation of the actual Null peer's production RenderPort, never a fake output implementation. */
    struct OutputEvidence final {
        float peak{};
        bool finite{true};
        std::size_t renderingBlocks{};
        bool destroyed{};
        bool closedAtDestruction{};
        std::size_t cancellations{};
        std::size_t acknowledgements{};
        std::size_t idleControlRejections{};
    };

    class ObservedNull final : public Audio::Backend::AudioBackend {
    public:
        std::unique_ptr<Audio::Backend::NullAudioBackend> output;
        std::shared_ptr<OutputEvidence> evidence;
        Audio::Backend::RenderPort port;

        ObservedNull(Audio::AudioRuntimeId owner, std::shared_ptr<OutputEvidence> capture)
            : output(std::move(Audio::Backend::CreateNullAudioBackend({owner, 1, 1, 1})).Value()), evidence(std::move(capture)) {}

        ~ObservedNull() override {
            evidence->closedAtDestruction = output->State() == Audio::Backend::NullAudioBackendState::Closed;
            evidence->destroyed = true;
        }

        Audio::AudioBackendKind Kind() const noexcept override {
            return output->Kind();
        }

        Audio::AudioRuntimeId Owner() const noexcept override {
            return output->Owner();
        }

        Result<Audio::Backend::OperationId> Begin(const Audio::Backend::Request &request,
                                                  const Audio::AudioMonotonicTimestamp &deadline) override {
            if (const auto *start = std::get_if<Audio::Backend::Start>(&request)) {
                port = start->render;
                return output->Begin(Audio::Backend::Start{start->epoch, {this, Render}}, deadline);
            }
            return output->Begin(request, deadline);
        }

        Result<void> CommitRendering(const Audio::AudioDeviceEpoch &epoch) override {
            return output->CommitRendering(epoch);
        }

        Result<Audio::Backend::CancelDisposition> Cancel(const Audio::Backend::OperationId &operation) override {
            auto result = output->Cancel(operation);
            if (result.HasValue())
                ++evidence->cancellations;
            return result;
        }

        Result<std::optional<Audio::Backend::Completion>> Poll(const Audio::Backend::OperationId &operation) override {
            return output->Poll(operation);
        }

        Result<void> AcknowledgeCompletion(const Audio::Backend::OperationId &operation) override {
            auto result = output->AcknowledgeCompletion(operation);
            if (result.HasValue())
                ++evidence->acknowledgements;
            return result;
        }

        std::size_t DrainEvents(std::span<Audio::Backend::Event> values) noexcept override {
            return output->DrainEvents(values);
        }

        Audio::Backend::AudioCallbackViolationDrain DrainSafetyViolations(
            std::span<Audio::Backend::AudioCallbackViolation> values) noexcept override {
            return output->DrainSafetyViolations(values);
        }

        static Audio::Backend::RenderResult Render(void *context, const Audio::Backend::RenderInvocation &invocation) noexcept {
            auto &self = *static_cast<ObservedNull *>(context);
            const auto result = self.port.process(self.port.context, invocation);
            if (invocation.phase == Audio::Backend::RenderPhase::Rendering) {
                ++self.evidence->renderingBlocks;
                for (const auto *plane : invocation.output.planes)
                    for (std::uint32_t i = 0; i < invocation.output.validFrames; ++i) {
                        self.evidence->finite = self.evidence->finite && std::isfinite(plane[i]);
                        self.evidence->peak = std::max(self.evidence->peak, std::abs(plane[i]));
                    }
            }
            return result;
        }

        Result<void> AdvanceControl() {
            auto result = output->AdvanceControl();
            if (result.HasError() && result.ErrorValue().code.Value() == Audio::AudioErrors::RuntimeInactive.code.Value())
                ++evidence->idleControlRejections;
            return result;
        }

        Result<void> AdvanceCallback() {
            using enum Audio::Backend::NullAudioBackendState;
            if (output->State() == Priming || output->State() == Rendering || output->State() == Quiescing) {
                auto rendered = output->AdvanceCallback();
                if (rendered.HasError())
                    return Result<void>::Failure(rendered.ErrorValue());
            }
            return Result<void>::Success();
        }
    };

    /** @brief Explicit headless composition; every protocol call still goes to the production Null peer. */
    inline EditorAudioPreviewOutputFactory ObservedOutput(std::shared_ptr<OutputEvidence> evidence) {
        return [evidence = std::move(evidence)](Audio::AudioRuntimeId owner) -> Result<EditorAudioPreviewBackend> {
            auto output = std::make_unique<ObservedNull>(owner, evidence);
            auto *driver = output.get();
            return Result<EditorAudioPreviewBackend>::Success({std::move(output), [driver] {
                return driver->AdvanceControl();
            }, [driver] {
                return driver->AdvanceCallback();
            }});
        };
    }

    struct ClipProject final {
        std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("horo-audio-inspector-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Assets::AssetRegistry registry;
        Assets::AssetId clip = Assets::AssetId::Parse("11111111-1111-4111-8111-111111111111").Value();
        Assets::AssetId workspace = Assets::AssetId::Parse("22222222-2222-4222-8222-222222222222").Value();
        ClipProject();

        /** @brief Returns the exact registered native fixture path without narrowing its non-ASCII UTF-8 spelling. */
        std::filesystem::path Source() const {
            return root / std::filesystem::path{u8"Assets/çok ses/tone.wav"};
        }

        ~ClipProject() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        AudioComponentPreviewRequest Request() const {
            AudioComponentPreviewRequest request{.workspace = workspace,
                                                 .revision = 1,
                                                 .object = 7,
                                                 .projectRoot = root,
                                                 .assets = registry.Snapshot()};
            request.source.sound = Audio::AudioSoundReference::ForClip(Audio::AudioClipId::Create(clip).Value()).Value();
            request.source.identity = {{Audio::AudioRuntimeId::Create(1).Value(), 1, 1}, 1, 1};
            request.source.playback.spatialMode = Audio::AudioSpatialMode::TwoD;
            request.source.playback.gain = 0.5F;
            request.source.playback.loop = true;
            return request;
        }
    };

    inline ClipProject::ClipProject() {
        const auto nativeSource = Source().lexically_relative(root);
        std::filesystem::create_directories(root / nativeSource.parent_path());
        const auto utf8 = nativeSource.generic_u8string();
        std::string source;
        for (const char8_t byte : utf8)
            source.push_back(static_cast<char>(byte));
        std::vector<unsigned char> wav;
        const auto text = [&wav](std::string_view value) {
            wav.insert(wav.end(), value.begin(), value.end());
        };
        const auto word = [&wav](std::uint32_t value, std::size_t bytes) {
            for (std::size_t i = 0; i < bytes; ++i)
                wav.push_back(static_cast<unsigned char>((value >> (i * 8)) & 255U));
        };
        text("RIFF");
        word(36 + 8192, 4);
        text("WAVEfmt ");
        word(16, 4);
        word(1, 2);
        word(1, 2);
        word(48'000, 4);
        word(96'000, 4);
        word(2, 2);
        word(16, 2);
        text("data");
        word(8192, 4);
        for (std::size_t i = 0; i < 4096; ++i)
            word(16384, 2);
        std::ofstream output{root / nativeSource, std::ios::binary};
        output.write(reinterpret_cast<const char *>(wav.data()), static_cast<std::streamsize>(wav.size()));
        output.close();
        const auto report = registry.Publish({{clip, Assets::AssetTypeId::Parse("core.audio").Value(), ProjectPath::Parse(source).Value(),
                                               ProjectPath::Parse(source + ".horo").Value()}});
        REQUIRE(report.status == Assets::AssetRegistryBuildStatus::Complete);
    }

    /** @brief Bounded exact output/control progression, including cleanup after a fatal assertion. */
    inline void Pump(EditorAudioPreviewHost &host, std::size_t steps = 12) {
        for (std::size_t i = 0; i < steps; ++i)
            REQUIRE(host.Pump().HasValue());
    }
}  // namespace Horo::Editor::AudioPreviewTest
