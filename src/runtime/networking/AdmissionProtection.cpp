#include "Horo/Network/AdmissionProtection.h"

#include "Horo/Foundation/Sha256.h"
#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <string_view>

namespace Horo::Network {
    namespace {
        [[nodiscard]] bool NonZero(const AuthenticationDigest &value) noexcept {
            return std::ranges::any_of(value, [](const std::byte byte) {
                return byte != std::byte{};
            });
        }

        void HashNumber(Sha256Builder &hash, const std::uint64_t value, const std::size_t width) noexcept {
            std::array<std::byte, 8> bytes{};
            for (std::size_t index = 0; index < width; ++index)
                bytes[index] = static_cast<std::byte>(value >> ((width - index - 1) * 8));
            (void)hash.Update(std::span{bytes}.first(width));
        }

        void HashBytes(Sha256Builder &hash, const std::span<const std::byte> bytes) noexcept {
            (void)hash.Update(bytes);
        }

    }  // namespace

    /** @copydoc ComputeAdmissionTranscriptDigest */
    AuthenticationDigest ComputeAdmissionTranscriptDigest(const HandshakeSelection &selection,
                                                          const AuthenticationChallenge &challenge) noexcept {
        Sha256Builder hash;
        constexpr std::string_view domain = "horo.network.admission.v1";
        HashBytes(hash, std::as_bytes(std::span{domain.data(), domain.size()}));
        HashNumber(hash, challenge.contractVersion, 4);
        HashNumber(hash, selection.connection.Slot(), 4);
        HashNumber(hash, selection.connection.Generation(), 4);
        HashNumber(hash, selection.sessionGeneration.Value(), 8);
        HashNumber(hash, selection.protocol.Value(), 2);
        HashNumber(hash, selection.version.major, 2);
        HashNumber(hash, selection.version.minor, 2);
        HashNumber(hash, selection.schemaFingerprint, 8);
        HashNumber(hash, selection.features.count, 8);
        for (std::size_t index = 0; index < std::min(selection.features.count, MaximumHandshakeFeatures); ++index)
            HashNumber(hash, selection.features.values[index].Value(), 2);
        HashNumber(hash, static_cast<std::uint8_t>(selection.compression), 1);
        HashNumber(hash, selection.transport.capabilityRevision, 8);
        for (const bool admitted : selection.transport.admittedDelivery)
            HashNumber(hash, admitted ? 1 : 0, 1);
        HashNumber(hash, selection.transport.channelCount, 4);
        HashNumber(hash, selection.transport.maximumMessageBytes, 8);
        HashNumber(hash, selection.transport.deadlinesEnabled ? 1 : 0, 1);
        HashNumber(hash, selection.transport.maximumDeadlineMilliseconds, 4);
        HashNumber(hash, challenge.policy.Value(), 8);
        HashNumber(hash, challenge.policyRevision, 8);
        HashBytes(hash, challenge.clientNonce);
        HashBytes(hash, challenge.serverNonce);
        AuthenticationDigest result{};
        const auto digest = hash.Finalize();
        for (std::size_t index = 0; index < result.size(); ++index)
            result[index] = static_cast<std::byte>(digest.bytes[index]);
        return result;
    }

    /** @copydoc AdmissionProtection::Create */
    Result<AdmissionProtection> AdmissionProtection::Create(const AdmissionProtectionPolicy &policy) {
        if (!policy.minimumSecureVersion.IsValid() || policy.maximumPending == 0 || policy.maximumPending > MaximumProtectedAdmissions ||
            policy.maximumPendingPerSource == 0 || policy.maximumPendingPerSource > policy.maximumPending ||
            policy.maximumAttemptsPerWindow == 0 || policy.maximumParseFailuresPerWindow == 0 ||
            policy.maximumVerifierCallsPerWindow == 0 || policy.maximumDiagnosticsPerWindow == 0 || policy.maximumBytesPerWindow == 0 ||
            policy.windowTicks == 0)
            return Result<AdmissionProtection>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        return Result<AdmissionProtection>::Success(AdmissionProtection{policy});
    }

    void AdmissionProtection::Refresh(Counters &counters, const std::uint64_t nowTick) const noexcept {
        if (counters.windowStart == 0 || nowTick - counters.windowStart >= policy_.windowTicks)
            counters = Counters{.windowStart = nowTick};
    }

    AdmissionProtection::Source *AdmissionProtection::FindOrReserveSource(const AdmissionSourceId source,
                                                                          const std::uint64_t nowTick) noexcept {
        for (auto &entry : sources_) {
            if (entry.id == source)
                return &entry;
        }
        for (auto &entry : sources_) {
            if (entry.id.value == 0 || (entry.pending == 0 && nowTick - entry.counters.windowStart >= policy_.windowTicks)) {
                entry = Source{.id = source};
                return &entry;
            }
        }
        return nullptr;
    }

    AdmissionProtection::Peer *AdmissionProtection::FindPeer(const ConnectionHandle connection,
                                                             const NetworkOperationGeneration generation) noexcept {
        for (auto &peer : peers_) {
            if (peer.active && peer.connection == connection && peer.generation == generation)
                return &peer;
        }
        return nullptr;
    }

    /** @copydoc AdmissionProtection::Begin */
    Result<void> AdmissionProtection::Begin(const AdmissionSourceId source, const HandshakeSelection &selection,
                                            const AuthenticationChallenge &challenge, const std::uint64_t nowTick) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(NetworkErrors::SessionShuttingDown));
        if (source.value == 0 || nowTick == 0 || nowTick < lastTick_ || !selection.connection.IsValid() ||
            !selection.sessionGeneration.IsValid() || !selection.protocol.IsValid() || !selection.version.IsValid() ||
            challenge.contractVersion != AuthenticationContractVersion || challenge.connection != selection.connection ||
            challenge.sessionGeneration != selection.sessionGeneration || selection.schemaFingerprint == 0 ||
            selection.features.count > MaximumHandshakeFeatures || selection.compression >= HandshakeCompression::Count ||
            !challenge.policy.IsValid() || challenge.policyRevision == 0 || !NonZero(challenge.transcriptDigest) ||
            !NonZero(challenge.clientNonce) || !NonZero(challenge.serverNonce) || challenge.clientNonce == challenge.serverNonce)
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionBindingInvalid));
        if (challenge.transcriptDigest != ComputeAdmissionTranscriptDigest(selection, challenge))
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionBindingInvalid));
        if (selection.version.major != policy_.minimumSecureVersion.major || selection.version < policy_.minimumSecureVersion)
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionDowngradeRejected));
        for (const auto &peer : peers_) {
            if (peer.active && peer.connection == selection.connection)
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleOperationStale));
        }
        for (std::size_t index = 0; index < nonceCount_; ++index) {
            if (nonceHistory_[index].first == challenge.clientNonce || nonceHistory_[index].second == challenge.clientNonce ||
                nonceHistory_[index].first == challenge.serverNonce || nonceHistory_[index].second == challenge.serverNonce)
                return Result<void>::Failure(MakeError(NetworkErrors::AdmissionReplayRejected));
        }
        if (pending_ >= policy_.maximumPending)
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionLimitExceeded));
        auto *sourceState = FindOrReserveSource(source, nowTick);
        if (sourceState == nullptr || sourceState->pending >= policy_.maximumPendingPerSource)
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionLimitExceeded));
        auto peer = std::ranges::find_if(peers_, [](const Peer &entry) {
            return !entry.active;
        });
        if (peer == peers_.end())
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionLimitExceeded));

        *peer = Peer{.connection = selection.connection,
                     .generation = selection.sessionGeneration,
                     .source = source,
                     .clientNonce = challenge.clientNonce,
                     .serverNonce = challenge.serverNonce,
                     .active = true};
        nonceHistory_[nextNonce_] = {challenge.clientNonce, challenge.serverNonce};
        nextNonce_ = (nextNonce_ + 1) % MaximumProtectedAdmissions;
        nonceCount_ = std::min(nonceCount_ + 1, MaximumProtectedAdmissions);
        ++sourceState->pending;
        ++pending_;
        lastTick_ = nowTick;
        return Result<void>::Success();
    }

    bool AdmissionProtection::CanCharge(const Counters &counters, const AdmissionWork work, const std::uint64_t amount,
                                        const AdmissionProtectionPolicy &policy) noexcept {
        using enum AdmissionWork;
        switch (work) {
            case AuthenticationAttempt:
                return counters.attempts < policy.maximumAttemptsPerWindow;
            case ParseFailure:
                return counters.parseFailures < policy.maximumParseFailuresPerWindow;
            case VerifierCall:
                return counters.verifierCalls < policy.maximumVerifierCallsPerWindow;
            case ParsedBytes:
                return amount <= policy.maximumBytesPerWindow - counters.bytes;
            case Diagnostic:
                return counters.diagnostics < policy.maximumDiagnosticsPerWindow;
            case Count:
                return false;
        }
        return false;
    }

    void AdmissionProtection::AddCharge(Counters &counters, const AdmissionWork work, const std::uint64_t amount) noexcept {
        using enum AdmissionWork;
        switch (work) {
            case AuthenticationAttempt:
                ++counters.attempts;
                break;
            case ParseFailure:
                ++counters.parseFailures;
                break;
            case VerifierCall:
                ++counters.verifierCalls;
                break;
            case ParsedBytes:
                counters.bytes += amount;
                break;
            case Diagnostic:
                ++counters.diagnostics;
                break;
            case Count:
                break;
        }
    }

    void AdmissionProtection::Release(Peer &peer) noexcept {
        if (!peer.active)
            return;
        for (auto &source : sources_) {
            if (source.id == peer.source) {
                --source.pending;
                break;
            }
        }
        --pending_;
        peer.active = false;
    }

    /** @copydoc AdmissionProtection::Charge */
    Result<void> AdmissionProtection::Charge(const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration,
                                             const AdmissionWork work, const std::uint64_t amount, const std::uint64_t nowTick) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(NetworkErrors::SessionShuttingDown));
        auto *peer = FindPeer(connection, sessionGeneration);
        if (peer == nullptr)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleOperationStale));
        if (nowTick == 0 || nowTick < lastTick_ || work >= AdmissionWork::Count || amount == 0 ||
            (work != AdmissionWork::ParsedBytes && amount != 1))
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        auto source = std::ranges::find_if(sources_, [peer](const Source &entry) {
            return entry.id == peer->source;
        });
        if (source == sources_.end())
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        Refresh(peer->counters, nowTick);
        Refresh(source->counters, nowTick);
        Refresh(global_, nowTick);
        lastTick_ = nowTick;
        if (!CanCharge(peer->counters, work, amount, policy_) || !CanCharge(source->counters, work, amount, policy_) ||
            !CanCharge(global_, work, amount, policy_)) {
            Release(*peer);
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionLimitExceeded));
        }
        AddCharge(peer->counters, work, amount);
        AddCharge(source->counters, work, amount);
        AddCharge(global_, work, amount);
        return Result<void>::Success();
    }

    /** @copydoc AdmissionProtection::End */
    Result<void> AdmissionProtection::End(const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(NetworkErrors::SessionShuttingDown));
        auto *peer = FindPeer(connection, sessionGeneration);
        if (peer == nullptr)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleOperationStale));
        Release(*peer);
        return Result<void>::Success();
    }

    /** @copydoc AdmissionProtection::Shutdown */
    void AdmissionProtection::Shutdown() noexcept {
        if (shutdown_)
            return;
        for (auto &peer : peers_)
            Release(peer);
        shutdown_ = true;
    }
}  // namespace Horo::Network
