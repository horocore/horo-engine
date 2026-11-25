#include "../../../apps/HoroEditor/app/MacOnDeviceSpeechProvider.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::Editor {
    TEST_CASE("Mac speech does not start a recognizer in an unpackaged test executable", "[agent][immersive][speech][macos]") {
        REQUIRE(ObserveMacSpeechAuthorization() == MacSpeechAuthorization::Unavailable);
        CHECK(RequestMacSpeechAuthorization() == MacSpeechAuthorization::Unavailable);
        MacOnDeviceSpeechProvider provider;
        const Audio::AudioProcessingFormat format{48'000, Audio::MakeAudioSpeakerLayout(Audio::AudioSpeakerPreset::Mono)};
        CHECK_FALSE(provider.Open("en-US", format, {}));
    }

    TEST_CASE("Mac speech authorization requires the main thread", "[agent][immersive][speech][macos]") {
        MacSpeechAuthorization authorization{};
        std::jthread worker([&authorization] {
            authorization = RequestMacSpeechAuthorization();
        });
        worker.join();
        CHECK(authorization == MacSpeechAuthorization::Unavailable);
    }
}  // namespace Horo::Editor
