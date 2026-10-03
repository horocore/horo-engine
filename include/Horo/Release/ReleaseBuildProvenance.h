#pragma once

/**
 * @file ReleaseBuildProvenance.h
 * @brief Canonical input and unsigned output evidence for reproducible release builds.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Release/ReleasePreflight.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Release {
    /** @brief One declared, non-secret environment identity affecting unsigned bytes. */
    struct ReleaseEnvironmentIdentity final {
        std::string name;
        Sha256Digest digest; /**< Hash of a declared, non-secret normalized value. */
        bool operator==(const ReleaseEnvironmentIdentity &) const noexcept = default;
    };

    /** @brief One unsigned file, before platform signing changes any bytes. */
    struct ReleaseUnsignedFile final {
        std::string path;
        std::uint64_t size{};
        Sha256Digest digest;
        bool operator==(const ReleaseUnsignedFile &) const noexcept = default;
    };

    /** @brief Frozen inputs and normalized unsigned output; paths are relative and public safe. */
    struct ReleaseBuildProvenanceData final {
        ReleaseFrozenIdentities frozen;
        Sha256Digest buildScript;
        std::uint64_t sourceEpochSeconds{}; /**< Declared timestamp used by deterministic generators. */
        std::vector<std::string> features;
        std::vector<ReleaseEnvironmentIdentity> environment;
        std::vector<ReleaseUnsignedFile> files;
    };

    /** @brief One bounded field or relative file path that differs between two unsigned builds. */
    struct ReleaseBuildVariance final {
        std::string field;
        bool operator==(const ReleaseBuildVariance &) const noexcept = default;
    };

    /** @brief Immutable canonical provenance for reproducible unsigned bytes only. */
    class ReleaseBuildProvenance final {
    public:
        /**
         * @brief Validates public-safe evidence, normalizes ordering, and seals canonical bytes.
         * @param data Captured frozen input identities and unsigned file inventory.
         * @return Provenance or a typed invalid-output failure.
         */
        [[nodiscard]] static Result<ReleaseBuildProvenance> Create(ReleaseBuildProvenanceData data);

        /**
         * @brief Parses exact canonical schema-v1 provenance bytes.
         * @param json Serialized provenance.
         * @return Validated provenance or a typed invalid-output failure.
         */
        [[nodiscard]] static Result<ReleaseBuildProvenance> ParseCanonical(std::string_view json);

        /** @brief Returns exact canonical bytes. @return Immutable JSON. */
        [[nodiscard]] const std::string &CanonicalJson() const noexcept;
        /** @brief Returns SHA-256 of canonical unsigned provenance. @return Stable digest. */
        [[nodiscard]] const Sha256Digest &Digest() const noexcept;
        /** @brief Returns typed normalized evidence. @return Borrowed immutable data. */
        [[nodiscard]] const ReleaseBuildProvenanceData &Data() const noexcept;
        /**
         * @brief Reports changed input fields and unsigned files in deterministic order.
         * @param other Rebuilt candidate to compare.
         * @return Bounded differences; empty only when canonical evidence matches.
         */
        [[nodiscard]] std::vector<ReleaseBuildVariance> Compare(const ReleaseBuildProvenance &other) const;

    private:
        ReleaseBuildProvenance(ReleaseBuildProvenanceData data, std::string json, const Sha256Digest &digest);

        ReleaseBuildProvenanceData data_;
        std::string json_;
        Sha256Digest digest_;
    };

    /**
     * @brief Captures exact unsigned file bytes from a quiescent private build tree.
     * @param root Private output directory; never serialized into provenance.
     * @param inputs Frozen identities, environment digests, and normalization policy; files must be empty.
     * @return Canonical provenance or a typed invalid-output failure for missing, unsafe, or unreadable content.
     */
    [[nodiscard]] Result<ReleaseBuildProvenance> CaptureReleaseBuildProvenance(const std::filesystem::path &root,
                                                                               ReleaseBuildProvenanceData inputs);
}  // namespace Horo::Release
