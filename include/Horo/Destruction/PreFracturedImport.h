#pragma once

/**
 * @file PreFracturedImport.h
 * @brief Bounded semantic validation and explicit publication of pre-fractured source candidates.
 */

#include "Horo/Assets/PreFracturedSource.h"
#include "Horo/Destruction/DestructibleDescriptor.h"
#include "Horo/Foundation/CancellationToken.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Destruction {
    /** @brief Version of the HoroChunk_<id> token and detached candidate semantics. */
    inline constexpr std::uint32_t CurrentPreFracturedImportSchemaVersion = 1;

    namespace PreFracturedImportErrors {
        extern const ErrorCodeDescriptor InvalidSchema;    /**< Normalized source schema is unsupported. */
        extern const ErrorCodeDescriptor MissingChunk;     /**< A source node lacks the required authored identity token. */
        extern const ErrorCodeDescriptor DuplicateChunk;   /**< Two source nodes claim one authored identity. */
        extern const ErrorCodeDescriptor NonFinite;        /**< Geometry or transform contains non-finite values. */
        extern const ErrorCodeDescriptor InvalidTopology;  /**< Open, non-manifold, degenerate, or inconsistently wound mesh. */
        extern const ErrorCodeDescriptor InvalidHierarchy; /**< Parent reference is invalid, cyclic, or too deep. */
        extern const ErrorCodeDescriptor InvalidMaterial;  /**< One triangle lacks a valid source material assignment. */
        extern const ErrorCodeDescriptor LimitExceeded;    /**< Chunk, geometry, byte, or work budget exceeded. */
        extern const ErrorCodeDescriptor Cancelled;        /**< Detached preparation cancelled before publication. */
        extern const ErrorCodeDescriptor StaleCandidate;   /**< Candidate refers to a replaced owner revision. */
        extern const ErrorCodeDescriptor Shutdown;         /**< Owner closed publication admission. */
    }  // namespace PreFracturedImportErrors

    /** @brief One validated stable semantic chunk; no source display name or backend handle is its identity. */
    struct PreFracturedChunk final {
        DestructionChunkId id{};                     /**< Authored non-zero token parsed from HoroChunk_<id>. */
        DestructionChunkId parent{};                 /**< Nearest authored mesh ancestor, or invalid for a root. */
        std::string sourcePath;                      /**< Source context retained for diagnostics. */
        std::array<double, 12> geometryToWorld{};    /**< Validated canonical source transform. */
        std::vector<std::array<float, 3>> positions; /**< Canonical world-space positions. */
        std::vector<std::uint32_t> triangleIndices;  /**< Closed, oriented, manifold triangles. */
        std::vector<std::string> triangleMaterials;  /**< Exact material assignment per triangle. */
    };

    /** @brief Detached, fully validated candidate; acceptance is a separate owner action. */
    class PreFracturedCandidate final {
    public:
        PreFracturedCandidate(const PreFracturedCandidate &) = default;
        PreFracturedCandidate &operator=(const PreFracturedCandidate &) = default;
        PreFracturedCandidate(PreFracturedCandidate &&) noexcept = default;
        PreFracturedCandidate &operator=(PreFracturedCandidate &&) noexcept = default;

        /** @brief Returns the bounded immutable chunk table. @return Borrowed stable-ID-ordered chunks. */
        [[nodiscard]] const std::vector<PreFracturedChunk> &Chunks() const noexcept {
            return chunks_;
        }

        /** @brief Returns the diagnostic source label. @return Borrowed source label. */
        [[nodiscard]] const std::string &SourceName() const noexcept {
            return sourceName_;
        }

        /** @brief Returns the validated geometry/material byte estimate. @return Bounded candidate bytes. */
        [[nodiscard]] std::uint64_t EstimatedBytes() const noexcept {
            return estimatedBytes_;
        }

        /** @brief Returns the exact cook import schema. @return Version of token and candidate semantics. */
        [[nodiscard]] std::uint32_t SchemaVersion() const noexcept {
            return schemaVersion_;
        }

    private:
        friend Result<PreFracturedCandidate> ValidatePreFracturedSource(const Assets::PreFracturedSource &, const DestructionLimits &,
                                                                        const CancellationToken &);
        PreFracturedCandidate() = default;
        std::string sourceName_;
        std::vector<PreFracturedChunk> chunks_;
        std::uint64_t estimatedBytes_{};
        std::uint32_t schemaVersion_{CurrentPreFracturedImportSchemaVersion};
    };

    /**
     * @brief Validates detached normalized source under explicit product limits.
     * @param source Owned source snapshot; display names must contain HoroChunk_<canonical decimal ID>, optionally followed by __label.
     * @param limits Explicit finite limits no greater than engine hard limits.
     * @param cancellation Cooperative cancellation observed throughout bounded work.
     * @return Detached candidate sorted by stable ID, or a typed error carrying source-context diagnostics.
     */
    [[nodiscard]] Result<PreFracturedCandidate> ValidatePreFracturedSource(const Assets::PreFracturedSource &source,
                                                                           const DestructionLimits &limits,
                                                                           const CancellationToken &cancellation);

    /**
     * @brief Parses and validates an FBX source into a detached candidate without publication.
     * @param bytes Complete borrowed FBX bytes.
     * @param sourceName Source label used in all diagnostics.
     * @param limits Explicit finite product limits.
     * @param cancellation Cooperative cancellation token.
     * @return Validated candidate or source/cook typed error, with no partial output.
     */
    [[nodiscard]] Result<PreFracturedCandidate> PreparePreFracturedFbx(std::span<const std::uint8_t> bytes, std::string_view sourceName,
                                                                       const DestructionLimits &limits,
                                                                       const CancellationToken &cancellation);

    struct PreFracturedOwnerRevisionTag;
    /** @brief Non-zero owner-issued revision fencing detached candidate acceptance. */
    using PreFracturedOwnerRevision = DestructionStableIdentity<PreFracturedOwnerRevisionTag>;

    /** @brief Host-owned single-thread publication boundary; callers serialize access on the owning thread. */
    class PreFracturedImportOwner final {
    public:
        /** @brief Creates an empty owner at revision one. */
        PreFracturedImportOwner();
        PreFracturedImportOwner(const PreFracturedImportOwner &) = delete;
        PreFracturedImportOwner &operator=(const PreFracturedImportOwner &) = delete;
        PreFracturedImportOwner(PreFracturedImportOwner &&) = delete;
        PreFracturedImportOwner &operator=(PreFracturedImportOwner &&) = delete;

        /** @brief Returns the current immutable candidate, or null before first acceptance. @return Shared immutable snapshot. */
        [[nodiscard]] std::shared_ptr<const PreFracturedCandidate> Snapshot() const noexcept {
            return current_;
        }

        /** @brief Returns the revision to capture before detached work. @return Non-wrapping owner revision. */
        [[nodiscard]] PreFracturedOwnerRevision Revision() const noexcept {
            return revision_;
        }

        /** @brief Returns the current revision's cancellation token for detached preparation. @return Borrowed-state token. */
        [[nodiscard]] CancellationToken Token() const noexcept {
            return cancellation_.Token();
        }

        /**
         * @brief Atomically replaces the current candidate only for the captured revision.
         * @param candidate Complete detached candidate produced by validation.
         * @param expectedRevision Revision captured before preparation began.
         * @return Success, or stale/shutdown error; on failure the prior snapshot is retained.
         */
        [[nodiscard]] Result<void> Accept(PreFracturedCandidate candidate, PreFracturedOwnerRevision expectedRevision);

        /**
         * @brief Retires detached work when the authoring source changes without accepting a candidate.
         * @return Success after advancing the owner revision and cancelling its prior token, or a typed shutdown/exhaustion error.
         * @post The last accepted immutable snapshot remains available until explicit replacement or owner destruction.
         */
        [[nodiscard]] Result<void> Invalidate();

        /** @brief Closes acceptance and retires pending revisions without discarding a published snapshot. */
        void Shutdown() noexcept;

    private:
        std::shared_ptr<const PreFracturedCandidate> current_;
        CancellationSource cancellation_;
        PreFracturedOwnerRevision revision_{};
        bool shutdown_{};
    };
}  // namespace Horo::Destruction
