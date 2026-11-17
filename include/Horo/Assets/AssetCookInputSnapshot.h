#pragma once

/** @file AssetCookInputSnapshot.h
 * @brief Host-captured immutable source and identity-sidecar bytes for one cook operation.
 */
#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Foundation/CancellationToken.h"

#include <memory>

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
        /**
         * @brief Captures only explicitly selected registered source/sidecar pairs using the ordinary capture checks.
         * @param sourceRoot Absolute project root protected by the host lease. @param registry Original pinned publication.
         * @param selected Unique stable asset identities; missing or duplicate identities reject the entire capture.
         * @param limits Per-source and selected-count bounds. @param maximumCapturedBytes Aggregate retained byte budget.
         * @param cancellation Cooperative stop fence checked between reads and before publication.
         * @return Complete selected capture retaining the original registry revision, or typed read/identity/capacity failure.
         * @throws std::bad_alloc on bounded allocation failure; no source or registry is modified.
         * @details Selection is explicit source-loading policy, not dependency discovery. The caller supplies a complete
         * closure before consuming the snapshot. Containment and observed digest checks match Capture; this does not
         * establish an operating-system atomic snapshot or retained-directory identity capability.
         */
        [[nodiscard]] static Result<AssetCookInputSnapshot> CaptureSelected(
            const std::filesystem::path &sourceRoot, AssetRegistrySnapshot registry, std::span<const AssetId> selected,
            const AssetCookLimits &limits, std::uint64_t maximumCapturedBytes, const CancellationToken &cancellation = {});
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
        /** @brief Shares the ordinary bounded pair capture without replacing the authoritative registry publication. */
        [[nodiscard]] static Result<AssetCookInputSnapshot> CaptureRecords(
            const std::filesystem::path &sourceRoot, AssetRegistrySnapshot registry, std::span<const AssetRecord> records,
            const AssetCookLimits &limits, std::uint64_t maximumCapturedBytes, const CancellationToken &cancellation);
        friend class AssetCookKeyProjection;
        struct State;

        explicit AssetCookInputSnapshot(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<const State> state_;
    };

    /** @brief Explicit domain producer declaration of all source inputs affecting one artifact.
     * The producer must derive this complete closure from its actual immutable graph and provider contracts.
     * Generic callers that cannot establish completeness leave key projection absent and retain whole-capture keys.
     */
    struct AssetCookKeySelection final {
        AssetId asset;
        std::vector<AssetId> inputs;
        Sha256Digest producerSettings;
    };

    /** @brief Immutable validated per-artifact selections backed by one real complete input capture.
     * This is cache identity only: the original complete capture and host publication fences remain authoritative.
     */
    class AssetCookKeyProjection final {
    public:
        /** @brief Validates complete producer selections and computes canonical digests from real pinned pairs.
         * @param inputs Real whole-operation source capture. @param selections Unique artifact selections including self.
         * @param maximumAssets Finite artifact and per-selection bound; aggregate references are capped at 1,048,576.
         * @return Immutable projection or typed invalid/capacity error; no file, cache or output mutation.
         */
        [[nodiscard]] static Result<std::shared_ptr<const AssetCookKeyProjection>> Create(const AssetCookInputSnapshot &inputs,
                                                                                          std::vector<AssetCookKeySelection> selections,
                                                                                          std::size_t maximumAssets);
        /** @brief Tests exact capture authority, independently from semantic key identity. @param inputs Candidate pin.
         * @return True only for the same complete capture evidence and registry publication/root.
         */
        [[nodiscard]] bool Matches(const AssetCookInputSnapshot &inputs) const;
        /** @brief Finds one complete immutable selection. @param asset Artifact. @return Borrowed selection or null. */
        [[nodiscard]] const AssetCookKeySelection *Find(AssetId asset) const noexcept;
        /** @brief Returns the actual-pair and producer commitment for an admitted artifact.
         * @param asset Artifact with a matching selection. @return Digest, or zero when absent.
         */
        [[nodiscard]] Sha256Digest Digest(AssetId asset) const noexcept;

    private:
        /** @brief Retains the exact immutable capture after bounded selection admission. */
        AssetCookKeyProjection(const AssetCookInputSnapshot &capture, std::vector<AssetCookKeySelection> selections,
                               std::vector<Sha256Digest> digests);
        AssetCookInputSnapshot capture_;
        std::vector<AssetCookKeySelection> selections_;
        std::vector<Sha256Digest> digests_;
    };
}  // namespace Horo::Assets
