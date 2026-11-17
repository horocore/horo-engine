#if HORO_TEST_WASAPI
#include "Horo/Audio/Internal/WasapiAudioBackend.h"
#endif
#if HORO_TEST_COREAUDIO
#include "Horo/Audio/Internal/CoreAudioBackend.h"
#endif
#if HORO_TEST_PIPEWIRE
#include "Horo/Audio/Internal/PipeWireAudioBackend.h"
#endif

#if defined(_WINDOWS_) || defined(__audioclient_h__) || defined(__MMDeviceAPI_h__)
#error "Native audio SDK headers escaped the concrete target"
#endif

#include <catch2/catch_test_macros.hpp>

using namespace Horo::Audio;
using namespace Horo::Audio::Backend;

TEST_CASE("Concrete native audio construction is inert and validates owner identity", "[audio][native-factory]") {
    const NativeAudioBackendConfig config{AudioRuntimeId::Create(47).Value(), 7};
#if HORO_TEST_WASAPI
    auto backend = CreateWasapiAudioBackend(config);
    REQUIRE(backend.HasValue());
    REQUIRE(backend.Value()->Kind() == AudioBackendKind::WASAPI);
    REQUIRE(backend.Value()->Owner() == config.owner);
    REQUIRE(CreateWasapiAudioBackend({}).HasError());
#endif
#if HORO_TEST_COREAUDIO
    auto backend = CreateCoreAudioBackend(config);
    REQUIRE(backend.HasValue());
    REQUIRE(backend.Value()->Kind() == AudioBackendKind::CoreAudio);
    REQUIRE(backend.Value()->Owner() == config.owner);
    REQUIRE(CreateCoreAudioBackend({}).HasError());
#endif
#if HORO_TEST_PIPEWIRE
    // Requiring the explicit process library lease must fail before any service connection.
    REQUIRE(CreatePipeWireAudioBackend(config, {}).HasError());
    auto library = InitializePipeWireAudioLibrary();
    REQUIRE(library.HasValue());
    auto backend = CreatePipeWireAudioBackend(config, library.Value());
    REQUIRE(backend.HasValue());
    REQUIRE(backend.Value()->Kind() == AudioBackendKind::PipeWire);
    REQUIRE(backend.Value()->Owner() == config.owner);
    REQUIRE(CreatePipeWireAudioBackend({}, library.Value()).HasError());
#endif
}
