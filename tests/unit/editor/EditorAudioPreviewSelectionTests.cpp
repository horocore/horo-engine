#include "EditorAudioPreviewHost.h"
#include "EditorAudioPreviewOptions.h"
#include "Horo/Audio/AudioErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <initializer_list>
#include <string>
#include <vector>

namespace Horo::Editor {
    namespace {
        /** @brief Keep argv storage alive while exercising the actual process-boundary parser. */
        std::optional<Audio::AudioBackendKind> Parse(const std::initializer_list<std::string_view> values) {
            std::vector<std::string> owned;
            owned.reserve(values.size());
            for (const auto value : values)
                owned.emplace_back(value);
            std::vector<char *> arguments;
            for (auto &value : owned)
                arguments.push_back(value.data());
            return Detail::ParseEditorAudioPreviewArguments(arguments);
        }

        TEST_CASE("Preview backend arguments select an exact peer and preserve it for process restart", "[audio][editor][preview]") {
            using enum Audio::AudioBackendKind;
            constexpr std::array peers{SDL3Audio, WASAPI, CoreAudio, PipeWire, NullAudio};
            CHECK(Parse({}) == SDL3Audio);
            CHECK(Parse({"horo-editor", "--renderer", "vulkan"}) == SDL3Audio);
            for (const auto peer : peers) {
                const auto name = Detail::AudioPreviewBackendName(peer);
                REQUIRE_FALSE(name.empty());
                CHECK(Parse({"horo-editor", "--audio-preview-backend", name}) == peer);
                const auto joined = "--audio-preview-backend=" + std::string{name};
                CHECK(Parse({"horo-editor", joined}) == peer);
            }
            CHECK(Detail::AudioPreviewBackendName(static_cast<Audio::AudioBackendKind>(255)).empty());
        }

        TEST_CASE("Malformed or repeated preview backend selection never revives the SDL default", "[audio][editor][preview]") {
            CHECK_FALSE(Parse({"horo-editor", "--audio-preview-backend"}));
            CHECK_FALSE(Parse({"horo-editor", "--audio-preview-backend="}));
            CHECK_FALSE(Parse({"horo-editor", "--audio-preview-backend", "WASAPI"}));
            CHECK_FALSE(Parse({"horo-editor", "--audio-preview-backend=automatic"}));
            CHECK_FALSE(Parse({"horo-editor", "--audio-preview-backend", "--renderer", "vulkan"}));
            CHECK_FALSE(Parse({"horo-editor", "--audio-preview-backend=sdl3", "--audio-preview-backend=sdl3"}));
            CHECK_FALSE(Parse({"horo-editor", "--audio-preview-backend=null", "--audio-preview-backend", "wasapi"}));
            CHECK_FALSE(Parse({"horo-editor", "--audio-preview-backend=unknown", "--audio-preview-backend=null"}));
        }

        TEST_CASE("Preview composition never substitutes a peer and shares its root factory across distinct owners",
                  "[audio][editor][preview][headless]") {
            using enum Audio::AudioBackendKind;

            struct Peer final {
                Audio::AudioBackendKind backend;
                bool installed;
            };

            constexpr std::array peers{Peer{SDL3Audio, true}, Peer{WASAPI, HORO_EDITOR_AUDIO_WASAPI != 0},
                                       Peer{CoreAudio, HORO_EDITOR_AUDIO_COREAUDIO != 0}, Peer{PipeWire, HORO_EDITOR_AUDIO_PIPEWIRE != 0},
                                       Peer{NullAudio, true}};
            const auto first = Audio::AudioRuntimeId::Create(501).Value();
            const auto second = Audio::AudioRuntimeId::Create(502).Value();
            for (const auto peer : peers) {
                CAPTURE(static_cast<int>(peer.backend));
                auto selected = SelectEditorAudioPreviewOutput(peer.backend);
                REQUIRE(selected.HasValue() == peer.installed);
                if (!peer.installed) {
                    CHECK(selected.ErrorValue().code.Value() == AudioComponentPreviewErrors::ProviderUnavailable.code.Value());
                    continue;
                }
                auto firstOutput = selected.Value()(first);
                auto secondOutput = selected.Value()(second);
                if (peer.backend == SDL3Audio && HORO_EDITOR_AUDIO_SDL3 == 0) {
                    REQUIRE(firstOutput.HasError());
                    REQUIRE(secondOutput.HasError());
                    CHECK(firstOutput.ErrorValue().code.Value() == AudioComponentPreviewErrors::ProviderUnavailable.code.Value());
                    CHECK(secondOutput.ErrorValue().code.Value() == AudioComponentPreviewErrors::ProviderUnavailable.code.Value());
                    continue;
                }
                REQUIRE(firstOutput.HasValue());
                REQUIRE(secondOutput.HasValue());
                REQUIRE(firstOutput.Value().output);
                REQUIRE(secondOutput.Value().output);
                CHECK(firstOutput.Value().output->Kind() == peer.backend);
                CHECK(secondOutput.Value().output->Kind() == peer.backend);
                CHECK(firstOutput.Value().output->Owner() == first);
                CHECK(secondOutput.Value().output->Owner() == second);
                CHECK(firstOutput.Value().output.get() != secondOutput.Value().output.get());
                CHECK(static_cast<bool>(firstOutput.Value().advanceControl));
                CHECK(static_cast<bool>(firstOutput.Value().advanceCallback) == (peer.backend == NullAudio));
            }
            const auto unknown = SelectEditorAudioPreviewOutput(static_cast<Audio::AudioBackendKind>(255));
            REQUIRE(unknown.HasError());
            CHECK(unknown.ErrorValue().code.Value() == Audio::AudioErrors::IdentityInvalid.code.Value());
        }
    }  // namespace
}  // namespace Horo::Editor
