#include "GameModuleHostTestHelpers.h"

using namespace Horo;
using namespace Horo::Gameplay;
using namespace Horo::Runtime;
using namespace Horo::Gameplay::HostTest;

namespace {
    /** @brief Mutates only the declared native version and proves rejection before any host decoder callback. */
    void RequireVersionRejection(const ImmutableSaveArchive &output, SceneContentWorldTest::WorldFixture &fixture,
                                 const SaveContentProjectPolicy &policy, const SaveParticipantId &participant) {
        auto admitted = SaveArchiveReader{}.Read(output.bytes);
        REQUIRE(admitted.HasValue());
        auto declarationBytes = admitted.Value().SelectChunk(SaveContentRequirementsRecord());
        REQUIRE(declarationBytes.HasValue());
        REQUIRE(declarationBytes.Value());
        auto declarations = DecodeSaveContentRequirements(*declarationBytes.Value());
        REQUIRE(declarations.HasValue());
        auto changedDeclarations = std::move(declarations).Value();
        auto nativeDeclaration = std::ranges::find(changedDeclarations, participant, &SaveContentRequirement::owner);
        REQUIRE(nativeDeclaration != changedDeclarations.end());
        auto &moduleVersion = std::get<SaveModuleContentRequirement>(nativeDeclaration->content);
        ++moduleVersion.version;
        const auto incompatible = SceneContentWorldTest::RewriteRequirements(output, changedDeclarations);
        const auto decoderCalls = fixture.source.decoder.calls;
        CHECK(ReconciledSaveContent::Prepare(*fixture.source.installed, incompatible, policy, fixture.source.cancellation.Token())
                  .HasError());
        CHECK(fixture.source.decoder.calls == decoderCalls);
    }

    template <typename T>
    concept FabricablePersistenceInstallation = requires { T{{}, {}, {}}; };
    static_assert(!FabricablePersistenceInstallation<Horo::Runtime::GameplayPersistenceInstallation>);
}  // namespace

TEST_CASE("actual installed SlotPlayer capture survives native retirement and re-admits its schema2 content declarations",
          "[unit][gameplay][save][content]") {
    using namespace Horo::Runtime;
    GameModuleHost host;
    auto loadedResult = host.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
    REQUIRE(loadedResult.HasValue());
    auto loaded = std::move(loadedResult).Value();
    const auto participant = SaveParticipantId::Parse("game.tests.durable3").Value();
    auto receiptResult = loaded->AcquireInstalledPersistence(participant);
    REQUIRE(receiptResult.HasValue());
    auto receipt = std::move(receiptResult).Value();
    const auto descriptor = receipt.Descriptor().participant;
    REQUIRE(descriptor.scope == SaveParticipantScope::SlotPlayer);
    auto adapter = receipt.AcquireAdapter();
    REQUIRE(adapter);
    std::vector<GameplayPersistenceInstallation> receipts;
    receipts.push_back(std::move(receipt));
    SceneContentWorldTest::WorldFixture fixture{std::move(receipts), {{descriptor, adapter}}};
    auto accepted = fixture.Capture();
    auto encoded = accepted.ReSave(fixture.Header(), SceneContentTest::V<ArchiveFormatVersion>());
    if (encoded.HasError()) {
        INFO("native content re-save: " << encoded.ErrorValue().code.Value() << ": " << encoded.ErrorValue().message);
        REQUIRE(encoded.HasValue());
    }
    REQUIRE(encoded.HasValue());
    const auto output = encoded.Value().Archive();
    auto policy = fixture.source.policy;
    policy.compatibility.saveSchemaVersions.direct = {SceneContentTest::V<SaveSchemaVersion>(2), SceneContentTest::V<SaveSchemaVersion>(2)};
    const auto schema = SceneContentTest::V<ParticipantSchemaVersion>();
    policy.compatibility.participants = {{participant, {{schema, schema}, {}}, false, {}},
                                         {SaveParticipantId::Parse("project.state").Value(), {{schema, schema}, {}}, false, {}}};
    std::ranges::sort(policy.compatibility.participants, {}, &SaveParticipantCompatibility::participant);
    auto readmitted = ReconciledSaveContent::Prepare(*fixture.source.installed, output, policy, fixture.source.cancellation.Token());
    REQUIRE(readmitted.HasValue());
    CHECK(std::ranges::any_of(readmitted.Value().Diagnostics(), [participant](const SaveContentDiagnostic &entry) {
        return entry.requirement.owner == participant;
    }));
    auto next = PrepareSavedSceneBootstrap(fixture.source.descriptor, SceneContentTest::SceneType(), std::move(readmitted).Value(),
                                           &fixture.source.decoder);
    REQUIRE(next.HasValue());
    RequireVersionRejection(output, fixture, policy, participant);
    fixture.service->Shutdown();
    RequireRestartRequired(loaded->PrepareReload());
    CHECK(ReconciledSaveContent::Prepare(*fixture.source.installed, output, policy, fixture.source.cancellation.Token()).HasError());
    loaded.reset();
    adapter.reset();
    auto worker = std::async(std::launch::async, [accepted, header = fixture.Header()] {
        return accepted.ReSave(header, SceneContentTest::V<ArchiveFormatVersion>());
    });
    auto afterRetirement = worker.get();
    REQUIRE(afterRetirement.HasValue());
    CHECK(afterRetirement.Value().Summary().canonicalState == encoded.Value().Summary().canonicalState);
}
