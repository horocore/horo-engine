#pragma once

#include "PlatformServicesTestSupport.h"

#include <array>

namespace Horo::PlatformServices::TestSupport {
    /** @brief Creates a Horo-owned session partition without a raw account identifier. */
    inline PlatformSubjectHandle CloudSubject(const std::uint64_t generation = 1, const std::byte nonce = std::byte{1},
                                              const PlatformProviderGeneration provider = {1}) {
        PlatformSessionCandidate candidate{.phase = PlatformSessionPhase::Active,
                                           .generation = {generation},
                                           .providerGeneration = provider,
                                           .accessRevision = {1}};
        candidate.capabilities.services.fill(PlatformSessionAccessState::Granted);
        PlatformSubjectNonce identity;
        identity.bytes.back() = nonce;
        candidate.subjectNonce = identity;
        const auto snapshot = BuildPlatformSessionSnapshot(candidate);
        REQUIRE(snapshot.HasValue());
        return *snapshot.Value().Subject();
    }

    /** @brief Constructs one complete immutable coordinator-style mutation intent. */
    inline CloudBlobWriteRequest CloudWrite(const PlatformSubjectHandle &subject = CloudSubject()) {
        const auto key = CloudSaveObjectKey::Copy(std::array{std::byte{3}});
        const auto bytes = CloudBlobOwnedBytes::Copy(std::array{std::byte{42}});
        REQUIRE(key.HasValue());
        REQUIRE(bytes.HasValue());
        CloudMutationId mutation;
        mutation.bytes.back() = std::byte{1};
        return {.subject = subject,
                .key = key.Value(),
                .bytes = bytes.Value(),
                .expectedDigest = ComputeSha256(bytes.Value().Bytes()),
                .precondition = CloudCreateIfAbsent{},
                .mutation = mutation};
    }

    /** @brief Produces complete commit evidence; it does not emulate durable remote storage. */
    inline CloudMutationResult CloudCommit(const CloudBlobWriteRequest &request) {
        const auto revision = ProviderObjectRevision::Copy(std::array{std::byte{7}});
        REQUIRE(revision.HasValue());
        return {.subject = request.subject,
                .sessionGeneration = request.subject.SessionGeneration(),
                .key = request.key,
                .mutation = request.mutation,
                .committedObject = CloudObjectHead{.key = request.key,
                                                   .revision = revision.Value(),
                                                   .sizeBytes = request.bytes.Bytes().size(),
                                                   .transportDigest = request.expectedDigest}};
    }
}  // namespace Horo::PlatformServices::TestSupport
