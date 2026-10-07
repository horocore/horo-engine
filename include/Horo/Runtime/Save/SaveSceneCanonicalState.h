#pragma once

/** @file SaveSceneCanonicalState.h
 * @brief Semantic schema2 canonical aggregation for a qualified scene composition without persistent datasets.
 */
#include "Horo/Runtime/Save/SaveArchiveFraming.h"
#include "Horo/Runtime/Save/SaveArchiveMetadata.h"
#include "Horo/Runtime/Save/SaveArchiveReader.h"
#include "Horo/Runtime/Save/SaveCanonicalCodec.h"

#include <variant>

namespace Horo::Runtime {
    class ValidatedSaveArchive;

    /** @brief Closed storage representation of an optional unknown record; no decoded semantics are asserted. */
    struct SaveSceneCanonicalOpaque final {
        SaveChunkCodec codec;
        std::uint64_t storedByteLength{};
        std::uint64_t decodedByteLength{};
        std::uint32_t alignment{};
        Sha256Digest decodedHash;
        std::span<const std::byte> storedBytes;
    };
    /** @brief Persistent schema2 record representation, independent of a later release's support decisions. */
    enum class SaveSceneCanonicalRepresentation : std::uint8_t {
        Known = 1,
        Opaque = 2
    };

    /** @brief One directory-owned record; the schema2 required layout records whether its canonical payload or opaque frame is used. */
    struct SaveSceneCanonicalRecord final {
        SaveRecordId record;
        std::variant<std::span<const std::byte>, SaveSceneCanonicalOpaque> payload;
    };

    /** @brief Present participant tuple in stable identity order; omitted optional participants are absent. */
    struct SaveSceneCanonicalParticipant final {
        SaveParticipantId participant;
        ParticipantSchemaVersion schema;
        std::span<const SaveSceneCanonicalRecord> records;
    };

    /** @brief First qualified whole-scene canonical aggregate version; archive framing and participant codecs remain independent. */
    inline constexpr std::uint32_t SaveSceneCanonicalSchemaVersion = 2;

    /** @brief One persisted storage classification, authenticated as a required schema2 canonical record. */
    struct SaveSceneCanonicalLayoutEntry final {
        SaveRecordId record;
        SaveSceneCanonicalRepresentation representation;
        [[nodiscard]] auto operator<=>(const SaveSceneCanonicalLayoutEntry &) const noexcept = default;
    };

    /** @brief Returns the reserved required layout owner. @return Schema-stable participant identity. */
    [[nodiscard]] SaveParticipantId SaveSceneCanonicalLayoutParticipant();
    /** @brief Returns the reserved layout record. @return Schema-stable record identity. */
    [[nodiscard]] SaveRecordId SaveSceneCanonicalLayoutRecord();
    /** @brief Encodes schema1 layout metadata for a schema2 aggregate.
     * @param entries Complete directory coverage in strictly increasing record identity order, including the layout itself as Known.
     * @return Bounded owned canonical bytes or a typed duplicate, classification, self-entry or capacity error.
     * @details This pure codec validates syntax; the owning archive admission must also check exact directory coverage and owner/schema.
     */
    [[nodiscard]] Result<CanonicalEncodedValue> EncodeSaveSceneCanonicalLayout(std::span<const SaveSceneCanonicalLayoutEntry> entries);
    /** @brief Decodes the closed layout without interpreting opaque payloads.
     * @param bytes Exact decoded reserved record bytes.
     * @return Ordered classifications, or a typed malformed, version, duplicate, self-entry or capacity error.
     */
    [[nodiscard]] Result<std::vector<SaveSceneCanonicalLayoutEntry>> DecodeSaveSceneCanonicalLayout(std::span<const std::byte> bytes);
    /** @brief Authenticates schema2 layout and logical state against an integrity-admitted archive before callbacks.
     * @param archive Production-reader admission proof retaining its source lifetime.
     * @param maximumBytes Finite aggregate decoded/stored logical-byte budget.
     * @return Complete directory classifications, or typed coverage, owner/schema, required-opaque, codec, budget or hash failure.
     * @details Required owners can never be classified opaque. Optional opaque frames retain their source representation even when a
     * later release understands their codec. This checks archive semantics, not installed content or world publication authority.
     */
    [[nodiscard]] Result<std::vector<SaveSceneCanonicalLayoutEntry>> ValidateSaveSceneCanonicalArchive(const ValidatedSaveArchive &archive,
                                                                                                       std::uint64_t maximumBytes = 64ULL
                                                                                                                                    << 20U);

    /** @brief Owned schema2 authority for preserving authenticated opaque frames without granting a decoder.
     * @details This load/background value pins production-reader shared archive storage. It cannot be default-constructed,
     * copied or fabricated from layout DTOs. Generic reader inspection remains strict and always verifies decoded integrity.
     */
    class ValidatedSaveSceneCanonicalPreservation final {
    public:
        ValidatedSaveSceneCanonicalPreservation(ValidatedSaveSceneCanonicalPreservation &&) noexcept = default;
        ValidatedSaveSceneCanonicalPreservation &operator=(ValidatedSaveSceneCanonicalPreservation &&) noexcept = default;
        ValidatedSaveSceneCanonicalPreservation(const ValidatedSaveSceneCanonicalPreservation &) = delete;
        ValidatedSaveSceneCanonicalPreservation &operator=(const ValidatedSaveSceneCanonicalPreservation &) = delete;
        /** @brief Mints authority only after authenticating the actual schema2 archive's complete canonical layout/hash.
         * @param archive Production-reader proof with owned shared backing; borrowed or moved-from storage is rejected.
         * @param maximumBytes Finite canonical validation budget. Raw hash work is debited before validation/materialization.
         * @return Owned immutable preservation proof or typed lifetime, version, layout, integrity or capacity failure.
         */
        [[nodiscard]] static Result<ValidatedSaveSceneCanonicalPreservation> Create(const ValidatedSaveArchive &archive,
                                                                                    std::uint64_t maximumBytes = 64ULL << 20U);
        /** @brief Retains optional unknown records and every authenticated opaque frame without reinterpretation.
         * @param policy Direct-read release policy. Drop permissions are rejected by this non-dropping contract.
         * @param maximumPreservedBytes Finite total stored-copy budget; shared reader work is charged before allocation.
         * @return Exact owned bytes/directory metadata. Known unknown-owner records still use real decode/hash verification.
         */
        [[nodiscard]] Result<SaveUnknownDataReport> InspectUnknownData(const SaveCompatibilityPolicy &policy,
                                                                       std::uint64_t maximumPreservedBytes) const;
        /** @brief Verifies exact opaque/unknown preservation between two independently authenticated archives.
         * @param destination Independently minted candidate authority, before publication.
         * @param policy Same explicit non-dropping release policy for both archives.
         * @param maximumPreservedBytes Finite per-archive stored-copy budget.
         * @return Success or a typed loss/change/integrity error naming the source record.
         */
        [[nodiscard]] Result<void> VerifyRoundTrip(const ValidatedSaveSceneCanonicalPreservation &destination,
                                                   const SaveCompatibilityPolicy &policy, std::uint64_t maximumPreservedBytes) const;

    private:
        /** @brief Retains authenticated immutable archive backing and complete owned classification.
         * @param archive Actual shared-storage reader proof. @param layout Successfully authenticated complete layout.
         */
        explicit ValidatedSaveSceneCanonicalPreservation(ValidatedSaveArchive archive,
                                                         std::vector<SaveSceneCanonicalLayoutEntry> layout) noexcept;
        /** @brief Debits bounded work on the existing shared reader ledger before hash/copy allocation.
         * @param archive Pinned reader authority. @param bytes Checked finite work amount. @return True when admitted.
         */
        [[nodiscard]] static bool Charge(const ValidatedSaveArchive &archive, std::uint64_t bytes) noexcept;
        /** @brief Collects one optional owner's authenticated representation under shared finite copy accounting.
         * @param owner Actual manifest ownership. @param known Direct-read release support.
         * @param maximumPreservedBytes Aggregate stored-byte cap. @param retained Current debit. @param report Owned output.
         * @return Success or typed owner, decode, integrity or capacity failure.
         */
        [[nodiscard]] Result<void> CollectOwner(const SaveManifestParticipant &owner, bool known, std::uint64_t maximumPreservedBytes,
                                                std::uint64_t &retained, SaveUnknownDataReport &report) const;
        ValidatedSaveArchive archive_;
        std::vector<SaveSceneCanonicalLayoutEntry> layout_;
    };

    /** @brief Encodes actual scene-only logical state using the existing bounded canonical value codec.
     * @param header Actual project/world/base-scene identities; publication, timestamps and storage fields are excluded.
     * @param participants Present participant/schema tuples in canonical identity order, with unique ordered record IDs.
     * @param maximumBytes Finite aggregate logical encoding budget, admitted before serialization.
     * @return Owned canonical stream or typed identity/order/duplicate/capacity failure.
     * @details This pure codec does not prove dataset absence, admit a world or publish a save. RuntimeScene's private aggregate
     * receipt and actual registered scopes must establish absence before its content-owned capture/re-save path calls it.
     * Schema2 encodes project/world/base-scene UUIDs, schema UInt32, empty dataset sequence, then participant UTF8/schema UInt32/
     * record UUID/payload bytes sequences. Unknown optional canonical payload bytes retain their exact tuple identity and bytes.
     */
    [[nodiscard]] Result<CanonicalEncodedValue> EncodeSaveSceneCanonicalState(const SaveArchiveHeader &header,
                                                                              std::span<const SaveSceneCanonicalParticipant> participants,
                                                                              std::uint64_t maximumBytes = 64ULL << 20U);
}  // namespace Horo::Runtime
