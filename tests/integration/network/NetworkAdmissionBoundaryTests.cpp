#include "NetworkAdmissionProductTestSupport.h"

namespace Horo::Network {
    using namespace AdmissionTests;

    template <typename Capture>
    concept BypassesProductAdmissionCapture =
        requires(const NetworkAdmissionPolicy &policy, NetworkAdmissionProviders providers,
                 std::shared_ptr<const MessageCodecRegistry> codecs) { Capture{{}, policy, providers, codecs, {}}; };
    static_assert(!BypassesProductAdmissionCapture<Application::Internal::NetworkProductAdmission>);

    TEST_CASE("Actual product admission contains standard and nonstandard foreign provider exceptions without granting gameplay",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        constexpr std::array sites{ProviderThrowSite::Entropy, ProviderThrowSite::Channel,    ProviderThrowSite::Certificate,
                                   ProviderThrowSite::Peer,    ProviderThrowSite::Credential, ProviderThrowSite::Key,
                                   ProviderThrowSite::Proof};
        for (const bool nonstandard : {false, true}) {
            for (const auto site : sites) {
                CAPTURE(nonstandard, static_cast<int>(site));
                ProductPair pair;
                pair.authorities[0]->throwSite = site;
                pair.authorities[0]->throwNonStandard = nonstandard;
                pair.Start();
                pair.Frames();
                CHECK(pair.hosts[1]->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702}).HasError());
                CHECK(pair.handlers[0]->calls == 0);
                CHECK(pair.handlers[1]->calls == 0);
                CHECK_FALSE(pair.link->ends[0].open);
            }
        }
    }

    TEST_CASE("Throwing transport retirement retains the original canonical terminal and performs cleanup once",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        for (const bool nonstandard : {false, true}) {
            ProductPair pair;
            pair.Start();
            pair.Frames();
            pair.Gameplay(1);
            pair.Frames(1);
            auto peer = pair.handlers[1]->lastSession.lock();
            REQUIRE(peer);
            auto worldOwner = pair.owners[1]->sessions->WorldBinding().lock();
            REQUIRE(worldOwner);
            const auto closesBeforeRetirement = pair.link->ends[1].closes;
            pair.link->ends[1].throwClose = true;
            pair.link->ends[1].throwCloseNonStandard = nonstandard;
            pair.owners[1]->sessionSource->Advance(Duration::FromNanoseconds(400));
            pair.Frames(1);
            REQUIRE(peer->Terminal());
            CHECK(peer->Terminal()->kind == PeerSessionTerminalKind::CredentialExpired);
            const auto closes = pair.link->ends[1].closes;
            REQUIRE(closes == closesBeforeRetirement + 1);
            const auto tick = pair.owners[1]->sessionClock->PollTick();
            REQUIRE(tick.HasValue());
            REQUIRE(worldOwner->Reconcile(tick.Value()).HasValue());
            CHECK(pair.link->ends[1].closes == closes);  // Repeated world retirement never takes native cleanup ownership.
            CHECK(worldOwner->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702}).HasError());
            pair.hosts[1]->Shutdown();
            pair.hosts[1]->Shutdown();
            CHECK(pair.link->ends[1].closes == closes);
            CHECK(pair.link->ends[1].shutdowns == 1);
            CHECK_FALSE(pair.link->ends[1].open);
            CHECK(peer->Terminal()->kind == PeerSessionTerminalKind::CredentialExpired);
            CHECK_FALSE(pair.hosts[1]->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702}).HasValue());
        }
    }
}  // namespace Horo::Network
