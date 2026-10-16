#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "RuntimeUiCapabilityTestSupport.h"

#include <catch2/catch_session.hpp>
#include <charconv>
#include <cstring>

namespace {
    const char *artifact{};
    std::uint64_t revision{};
}  // namespace

TEST_CASE("External Runtime UI SDK retains only host-issued storage after actual native image unload", "[runtime-ui][sdk][native]") {
    using namespace Horo;
    using namespace Horo::Tests::UiAccess;
    Fixture fixture;
    auto grant = fixture.Grant();
    grant.moduleId = "game.ui_sdk";
    auto anchor = Take(fixture.service.Issue(grant));
    auto host = std::make_unique<Gameplay::GameModuleHost>(Gameplay::GameModuleHostOptions{{}, {.runtimeUi = anchor}});
    auto candidate = Take(host->Load(artifact, {"game.ui_sdk", Gameplay::CurrentGameplayBuildFingerprint(), revision}));
    REQUIRE_FALSE(candidate.diagnostic);
    auto module = std::move(candidate.owner);
    auto image = Take(Platform::LoadDynamicLibrary(module->LoadedArtifactPath().string()));
    void *symbol = image->GetSymbol("HoroRuntimeUiSdkCopyClient");
    REQUIRE(symbol != nullptr);
    using Copy = void (*)(std::shared_ptr<const RuntimeUiClient> *);
    Copy copy{};
    static_assert(sizeof(copy) == sizeof(symbol));
    std::memcpy(&copy, &symbol, sizeof(copy));
    std::shared_ptr<const RuntimeUiClient> escaped;
    copy(&escaped);
    REQUIRE(escaped != nullptr);
    REQUIRE(escaped->Validate().HasValue());
    module.reset();
    copy = nullptr;
    image.reset();
    CHECK(escaped->Validate().HasError());
    CHECK(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 1);
    CHECK(Take(anchor->ScreenGuard()).revision.Value() == 1);
    std::weak_ptr<const RuntimeUiClient> weak = escaped;
    escaped.reset();
    CHECK(weak.expired());
    weak.reset();
    host.reset();
    anchor.reset();
    fixture.service.Shutdown();
    REQUIRE(fixture.service.CollectRetired().HasValue());
    CHECK(Take(fixture.service.CanReclaim()));
}

int main(int argc, char **argv) {
    if (argc != 3)
        return 1;
    artifact = argv[1];
    const std::string_view text{argv[2]};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), revision);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !revision)
        return 2;
    return Catch::Session{}.run(1, argv);
}
