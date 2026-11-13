#include "Horo/PlatformServices/PlatformSessionBinding.h"
#include "PlatformServicesTestSupport.h"

namespace Horo::PlatformServices {
    namespace {
        using TestSupport::RequireError;

        ProviderAccountBindingId Binding(const PlatformSessionBindingIssuer &issuer, const std::byte scope = std::byte{1},
                                         const std::byte pseudonym = std::byte{2}, const PlatformProviderId provider = {41}) {
            ProviderAccountBindingCandidate candidate{.provider = provider};
            candidate.productEnvironmentScope.front() = scope;
            candidate.protectedPseudonym.front() = pseudonym;
            auto built = BuildProviderAccountBindingId(issuer, candidate);
            REQUIRE(built.HasValue());
            return built.Value();
        }

        PlatformSessionSnapshot BindingSession(const std::uint64_t generation = 1, const std::uint64_t revision = 1,
                                               const PlatformSessionAccessState access = PlatformSessionAccessState::Granted,
                                               const PlatformProviderGeneration providerGeneration = {7}) {
            PlatformSessionCandidate candidate{.phase = PlatformSessionPhase::Active,
                                               .generation = {generation},
                                               .providerGeneration = providerGeneration,
                                               .accessRevision = {revision}};
            PlatformSubjectNonce nonce;
            nonce.bytes.front() = static_cast<std::byte>(generation);
            candidate.subjectNonce = nonce;
            candidate.capabilities.services.fill(PlatformSessionAccessState::Granted);
            candidate.capabilities.services[static_cast<std::size_t>(PlatformServiceKind::Cloud)] = access;
            auto built = BuildPlatformSessionSnapshot(candidate);
            REQUIRE(built.HasValue());
            return built.Value();
        }
    }  // namespace

    TEST_CASE("Protected binding handoff rejects malformed and inconsistent evidence", "[platform-services][session][binding]") {
        auto issuer = PlatformSessionBindingIssuer::Create();
        ProviderAccountBindingCandidate candidate{.provider = {41}};
        RequireError(BuildProviderAccountBindingId(issuer, candidate), PlatformIdentityBindingErrors::BindingFailed);
        candidate.productEnvironmentScope.front() = std::byte{1};
        RequireError(BuildProviderAccountBindingId(issuer, candidate), PlatformIdentityBindingErrors::BindingFailed);
        candidate.protectedPseudonym.front() = std::byte{2};
        candidate.provider = {};
        RequireError(BuildProviderAccountBindingId(issuer, candidate), PlatformIdentityBindingErrors::BindingFailed);
        const auto provider = TestSupport::AvailableCapabilities();
        const auto session = BindingSession();
        RequireError(BuildPlatformSessionBindingEvidence(issuer, {}, provider, session), PlatformIdentityBindingErrors::BindingFailed);
        RequireError(BuildPlatformSessionBindingEvidence(issuer, Binding(issuer), TestSupport::AvailableCapabilities({8}), session),
                     PlatformIdentityBindingErrors::BindingFailed);
        auto malformed = provider;
        malformed.provider = {};
        const auto rejected = BuildPlatformSessionBindingEvidence(issuer, Binding(issuer), malformed, session);
        RequireError(rejected, PlatformIdentityBindingErrors::BindingFailed);
        REQUIRE(rejected.ErrorValue().cause.Get() != nullptr);
        CHECK(rejected.ErrorValue().cause.Get()->code.Value() == BackendErrors::InvalidCapabilitySnapshot.code.Value());
        RequireError(BuildPlatformSessionBindingEvidence(issuer, Binding(issuer, std::byte{1}, std::byte{2}, {42}), provider, session),
                     PlatformIdentityBindingErrors::BindingMismatch);
        RequireError(ValidatePlatformSessionBindingAccess(provider, session, {}, Binding(issuer), PlatformServiceKind::Cloud),
                     PlatformIdentityBindingErrors::BindingFailed);
    }

    TEST_CASE("Same binding reauthentication requires fresh evidence and cannot retarget another account or scope",
              "[platform-services][session][binding][qualification]") {
        auto issuer = PlatformSessionBindingIssuer::Create();
        const auto provider = TestSupport::AvailableCapabilities();
        const auto expected = Binding(issuer);
        const auto oldSession = BindingSession();
        const auto oldEvidence = BuildPlatformSessionBindingEvidence(issuer, expected, provider, oldSession);
        REQUIRE(oldEvidence.HasValue());
        CHECK(oldEvidence.Value().IsValid());
        CHECK(oldEvidence.Value().Subject() == *oldSession.Subject());
        REQUIRE(ValidatePlatformSessionBindingAccess(provider, oldSession, oldEvidence.Value(), expected, PlatformServiceKind::Cloud)
                    .HasValue());
        const auto current = BindingSession(4, 6);
        RequireError(ValidatePlatformSessionBindingAccess(provider, current, oldEvidence.Value(), expected, PlatformServiceKind::Cloud),
                     PlatformSessionErrors::StaleSession);
        const auto fresh = BuildPlatformSessionBindingEvidence(issuer, expected, provider, current);
        REQUIRE(fresh.HasValue());
        REQUIRE(ValidatePlatformSessionBindingAccess(provider, current, fresh.Value(), expected, PlatformServiceKind::Cloud).HasValue());
        RequireError(ValidatePlatformSessionBindingAccess(provider, current, fresh.Value(), Binding(issuer, std::byte{1}, std::byte{3}),
                                                          PlatformServiceKind::Cloud),
                     PlatformIdentityBindingErrors::BindingMismatch);
        RequireError(ValidatePlatformSessionBindingAccess(provider, current, fresh.Value(), Binding(issuer, std::byte{2}),
                                                          PlatformServiceKind::Cloud),
                     PlatformIdentityBindingErrors::BindingMismatch);
        const auto reloadedProvider = TestSupport::AvailableCapabilities({8});
        const auto reloadedSession = BindingSession(7, 9, PlatformSessionAccessState::Granted, {8});
        RequireError(ValidatePlatformSessionBindingAccess(reloadedProvider, reloadedSession, fresh.Value(), expected,
                                                          PlatformServiceKind::Cloud),
                     PlatformSessionErrors::StaleSession);
        const auto rebound = BuildPlatformSessionBindingEvidence(issuer, expected, reloadedProvider, reloadedSession);
        REQUIRE(rebound.HasValue());
        REQUIRE(
            ValidatePlatformSessionBindingAccess(reloadedProvider, reloadedSession, rebound.Value(), expected, PlatformServiceKind::Cloud)
                .HasValue());
    }

    TEST_CASE("Protected binding never bypasses provider availability or current consent restrictions",
              "[platform-services][session][binding][privacy]") {
        auto issuer = PlatformSessionBindingIssuer::Create();
        const auto binding = Binding(issuer);
        const auto provider = TestSupport::AvailableCapabilities();
        const auto oldEvidence = BuildPlatformSessionBindingEvidence(issuer, binding, provider, BindingSession());
        REQUIRE(oldEvidence.HasValue());
        const auto restricted = BindingSession(1, 2, PlatformSessionAccessState::Restricted);
        RequireError(ValidatePlatformSessionBindingAccess(provider, restricted, oldEvidence.Value(), binding, PlatformServiceKind::Cloud),
                     PlatformSessionErrors::StaleAccessPolicy);
        const auto fresh = BuildPlatformSessionBindingEvidence(issuer, binding, provider, restricted);
        REQUIRE(fresh.HasValue());
        RequireError(ValidatePlatformSessionBindingAccess(provider, restricted, fresh.Value(), binding, PlatformServiceKind::Cloud),
                     PlatformSessionErrors::AccessRestricted);
        auto unavailable = provider;
        auto &cloud = unavailable.services[static_cast<std::size_t>(PlatformServiceKind::Cloud)];
        cloud.availability = PlatformServiceAvailability::Unavailable;
        cloud.binding.reset();
        cloud.limits = {};
        cloud.unavailableReason = PlatformServiceUnavailableReason::ServiceUnsupported;
        RequireError(ValidatePlatformSessionBindingAccess(unavailable, BindingSession(), oldEvidence.Value(), binding,
                                                          PlatformServiceKind::Cloud),
                     BackendErrors::ServiceUnavailable);
        cloud.unavailableReason = PlatformServiceUnavailableReason::NullProviderSelected;
        RequireError(ValidatePlatformSessionBindingAccess(unavailable, BindingSession(), oldEvidence.Value(), binding,
                                                          PlatformServiceKind::Cloud),
                     BackendErrors::NullProvider);
    }

    TEST_CASE("Sign-out invalidates protected binding evidence without creating a replacement session authority",
              "[platform-services][session][binding][lifecycle]") {
        auto issuer = PlatformSessionBindingIssuer::Create();
        const auto binding = Binding(issuer);
        const auto provider = TestSupport::AvailableCapabilities();
        const auto evidence = BuildPlatformSessionBindingEvidence(issuer, binding, provider, BindingSession());
        REQUIRE(evidence.HasValue());
        PlatformSessionCandidate candidate{.phase = PlatformSessionPhase::Closing,
                                           .generation = {2},
                                           .providerGeneration = {7},
                                           .accessRevision = {2},
                                           .reason = PlatformSessionReason::UserSignedOut};
        candidate.capabilities.services.fill(PlatformSessionAccessState::Unavailable);
        const auto closing = BuildPlatformSessionSnapshot(candidate);
        REQUIRE(closing.HasValue());
        RequireError(ValidatePlatformSessionBindingAccess(provider, closing.Value(), evidence.Value(), binding, PlatformServiceKind::Cloud),
                     PlatformSessionErrors::Closing);
        RequireError(BuildPlatformSessionBindingEvidence(issuer, binding, provider, closing.Value()), PlatformSessionErrors::Closing);
    }

    TEST_CASE("A foreign issuer cannot forge evidence from a retained expected binding", "[platform-services][session][binding][issuer]") {
        auto issuer = PlatformSessionBindingIssuer::Create();
        auto foreign = PlatformSessionBindingIssuer::Create();
        const auto binding = Binding(issuer);
        const auto foreignBinding = Binding(foreign);
        CHECK(binding != foreignBinding);
        const auto provider = TestSupport::AvailableCapabilities();
        const auto session = BindingSession();
        RequireError(BuildPlatformSessionBindingEvidence(foreign, binding, provider, session),
                     PlatformIdentityBindingErrors::BindingMismatch);
        const auto forged = BuildPlatformSessionBindingEvidence(foreign, foreignBinding, provider, session);
        REQUIRE(forged.HasValue());
        RequireError(ValidatePlatformSessionBindingAccess(provider, session, forged.Value(), binding, PlatformServiceKind::Cloud),
                     PlatformIdentityBindingErrors::BindingMismatch);
        auto moved = std::move(issuer);
        CHECK_FALSE(issuer.IsOpen());
        const auto evidence = BuildPlatformSessionBindingEvidence(moved, binding, provider, session);
        REQUIRE(evidence.HasValue());
        moved.Close();
        moved.Close();
        RequireError(BuildProviderAccountBindingId(moved, {}), PlatformIdentityBindingErrors::MappingUnavailable);
        CHECK_FALSE(binding.IsValid());
        CHECK_FALSE(evidence.Value().IsValid());
        RequireError(BuildPlatformSessionBindingEvidence(moved, binding, provider, session),
                     PlatformIdentityBindingErrors::MappingUnavailable);
        RequireError(ValidatePlatformSessionBindingAccess(provider, session, evidence.Value(), binding, PlatformServiceKind::Cloud),
                     PlatformIdentityBindingErrors::BindingFailed);
    }

    TEST_CASE("Protected bindings do not extend their private issuer lifetime", "[platform-services][session][binding][lifetime]") {
        ProviderAccountBindingId retained;
        {
            auto issuer = PlatformSessionBindingIssuer::Create();
            retained = Binding(issuer);
            REQUIRE(retained.IsValid());
        }
        CHECK_FALSE(retained.IsValid());
    }
}  // namespace Horo::PlatformServices
