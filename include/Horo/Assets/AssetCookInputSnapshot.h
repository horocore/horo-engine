#pragma once

/** @file AssetCookInputSnapshot.h
 * @brief Host-captured immutable source and identity-sidecar bytes for one cook operation.
 */
#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Foundation/CancellationToken.h"

namespace Horo::Assets {
    /** @brief Read-only input views; storage is owned by the snapshot, not the source filesystem. */
    struct AssetCookPinnedSource final {
        AssetRecord record;
        std::vector<std::uint8_t> bytes;
        Sha256Digest sourceDigest;
        Sha256Digest metadataDigest; /**< Canonical schema-1 identity/type metadata, excluding bookkeeping and paths. */
    };

    /**
     * @brief Immutable bounded capture made only by reading and validating real source/sidecar pairs.
     * @details Capture is load/tooling work. The host must complete source migration and acquire its project
     * read authority first. No migration, registry activation or source mutation occurs here. Copies retain
     * the same storage; views remain valid while a snapshot copy exists. Filesystem checks detect byte drift,
     * but are not a replacement for the host's project lease through the publication commit barrier.
     */
    class AssetCookInputSnapshot final {
    public:
        /**
         * @brief Reads every pinned registry source and its committed identity sidecar transactionally.
         * @param sourceRoot Absolute, non-symlink project root protected by the host's read authority.
         * @param registry Exact immutable registry used for path, identity and type validation.
         * @param limits Positive per-source and asset-count bounds.
         * @param maximumCapturedBytes Positive aggregate bound for source and sidecar byte storage.
         * @param cancellation Checked between every bounded file read and before returning the candidate.
         * @return Owned complete snapshot or typed read, metadata, capacity, cancellation or stale-input failure.
         * @throws std::bad_alloc on bounded storage allocation failure; no authoritative state is changed.
         */
        [[nodiscard]] static Result<AssetCookInputSnapshot> Capture(const std::filesystem::path &sourceRoot, AssetRegistrySnapshot registry,
                                                                    const AssetCookLimits &limits, std::uint64_t maximumCapturedBytes,
                                                                    const CancellationToken &cancellation = {});
        /** @brief Returns the exact pinned registry. @return Borrowed immutable registry. */
        [[nodiscard]] const AssetRegistrySnapshot &Registry() const noexcept;
        /** @brief Returns the native root read by the host capture. @return Borrowed absolute source root. */
        [[nodiscard]] const std::filesystem::path &SourceRoot() const noexcept;
        /** @brief Returns the ordered identity/type/source/metadata commitment for the entire capture.
         * @return Canonical closure digest, excluding native paths and process-local registry counters. */
        [[nodiscard]] Sha256Digest ClosureDigest() const;
        /** @brief Returns captured sources in ascending AssetId order. @return Borrowed immutable inputs. */
        [[nodiscard]] std::span<const AssetCookPinnedSource> Sources() const noexcept;
        /** @brief Finds one captured source. @param id Stable asset identity. @return Borrowed value or null. */
        [[nodiscard]] const AssetCookPinnedSource *Find(AssetId id) const noexcept;
        /**
         * @brief Rechecks actual source and sidecar bytes against this capture without publishing anything.
         * @param cancellation Checked between bounded reads.
         * @return Success only when every captured file remains identical, otherwise typed failure.
         * @details Intended for host-owned publication barriers while the project lease remains held.
         */
        [[nodiscard]] Result<void> VerifyUnchanged(const CancellationToken &cancellation = {}) const;

    private:
        struct State;

        explicit AssetCookInputSnapshot(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<const State> state_;
    };
}  // namespace Horo::Assets
