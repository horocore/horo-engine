#pragma once

#include "Horo/Runtime/Save/SaveSceneCanonicalState.h"
#include "SaveContentWorldTestHelpers.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Runtime::SceneContentRetentionTest {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace SceneContentWorldTest;

    struct RetainedFixture final {
        SceneContentTest::Fixture source;
        ImmutableSaveArchive archive;
        SaveContentProjectPolicy policy;
        SaveParticipantId owner = SaveParticipantId::Parse("project.optional").Value();
        SaveRecordId record = SceneTest::Id<SaveRecordId>(31);
        std::uint64_t decodedBytes{};
        std::vector<std::byte> stored;

        RetainedFixture(SaveSceneCanonicalRepresentation representation, SaveChunkCodec codec,
                        std::optional<SaveContentNecessity> declaredDlc = {}) {
            auto original = SaveArchiveReader{}.Read(source.archive.bytes);
            REQUIRE(original.HasValue());
            const auto header = original.Value().Header();
            auto declarationBytes = ReadDeclaration(original.Value(), declaredDlc);
            PrepareStoredRecord(codec);
            WriteArchive(header, declarationBytes, representation, codec);
        }

        std::vector<std::byte> decoded;
        Sha256Digest digest;

        /** @brief Retains actual source declarations and optionally appends an explicit missing-DLC requirement. */
        std::vector<std::byte> ReadDeclaration(const ValidatedSaveArchive &original,
                                               const std::optional<SaveContentNecessity> declaredDlc) {
            auto declaration = original.SelectChunk(SaveContentRequirementsRecord());
            REQUIRE(declaration.HasValue());
            REQUIRE(declaration.Value());
            auto declarationBytes = std::move(*declaration.Value());
            if (declaredDlc) {
                auto decodedRequirements = DecodeSaveContentRequirements(declarationBytes);
                REQUIRE(decodedRequirements.HasValue());
                auto requirements = std::move(decodedRequirements).Value();
                requirements.push_back(
                    {owner, *declaredDlc,
                     SaveChunkContentRequirement{Assets::AssetChunkId::Parse("missing_dlc").Value(), Assets::AssetChunkKind::Dlc}});
                auto encodedRequirements = EncodeSaveContentRequirements(requirements);
                REQUIRE(encodedRequirements.HasValue());
                declarationBytes.assign(encodedRequirements.Value().Bytes().begin(), encodedRequirements.Value().Bytes().end());
            }
            return declarationBytes;
        }

        /** @brief Constructs real deterministic known payload bytes and applies the actual required codec. */
        void PrepareStoredRecord(const SaveChunkCodec codec) {
            decoded.resize(65'536);
            std::uint32_t state = 17;
            for (std::size_t index = 0; index < 4096; ++index) {
                state = state * 1664525U + 1013904223U;
                decoded[index] = static_cast<std::byte>(state >> 24U);
            }
            for (std::size_t index = 4096; index < decoded.size(); ++index)
                decoded[index] = decoded[index % 4096];
            CanonicalValueWriter payloadWriter;
            REQUIRE(payloadWriter.WriteBytes(decoded).HasValue());
            auto payload = std::move(payloadWriter).Finalize();
            REQUIRE(payload.HasValue());
            decoded.assign(payload.Value().Bytes().begin(), payload.Value().Bytes().end());
            decodedBytes = decoded.size();
            digest = ComputeSha256(decoded);
            if (codec == SaveChunkCodec::Deflate) {
                auto compressed = EncodeSaveChunk(decoded, {.preferred = codec, .required = true}, {});
                REQUIRE(compressed.HasValue());
                stored = std::move(compressed).Value().stored;
                REQUIRE(stored.size() < decoded.size());
            } else {
                stored = decoded;
            }
        }

        /** @brief Authenticates the complete record classifications, including the required self-entry. */
        std::vector<std::byte> LayoutBytes(const SaveSceneCanonicalRepresentation representation) const {
            std::vector tags{SaveSceneCanonicalLayoutEntry{SaveContentRequirementsRecord(), SaveSceneCanonicalRepresentation::Known},
                             SaveSceneCanonicalLayoutEntry{SaveSceneCanonicalLayoutRecord(), SaveSceneCanonicalRepresentation::Known},
                             SaveSceneCanonicalLayoutEntry{record, representation}};
            std::ranges::sort(tags, {}, &SaveSceneCanonicalLayoutEntry::record);
            auto layout = EncodeSaveSceneCanonicalLayout(tags);
            REQUIRE(layout.HasValue());
            std::vector<std::byte> layoutBytes(layout.Value().Bytes().begin(), layout.Value().Bytes().end());
            return layoutBytes;
        }

        /** @brief Derives the fixture root from actual known payloads or closed stored-frame tuples. */
        CanonicalStateHash CanonicalRoot(const SaveArchiveHeader &header, const std::vector<std::byte> &declarationBytes,
                                         const std::vector<std::byte> &layoutBytes, const SaveSceneCanonicalRepresentation representation,
                                         const SaveChunkCodec codec) const {
            const auto version = SceneContentTest::V<ParticipantSchemaVersion>();
            const std::array declarationRecords{
                SaveSceneCanonicalRecord{SaveContentRequirementsRecord(), std::span<const std::byte>{declarationBytes}}};
            const std::array layoutRecords{
                SaveSceneCanonicalRecord{SaveSceneCanonicalLayoutRecord(), std::span<const std::byte>{layoutBytes}}};
            const SaveSceneCanonicalOpaque opaque{codec, stored.size(), decoded.size(), 1, digest, stored};
            const std::array dataRecords{
                SaveSceneCanonicalRecord{record, representation == SaveSceneCanonicalRepresentation::Known
                                                     ? decltype(SaveSceneCanonicalRecord::payload){std::span<const std::byte>{decoded}}
                                                     : decltype(SaveSceneCanonicalRecord::payload){opaque}}};
            const std::array owners{SaveSceneCanonicalParticipant{SaveContentRequirementsParticipant(), version, declarationRecords},
                                    SaveSceneCanonicalParticipant{SaveSceneCanonicalLayoutParticipant(), version, layoutRecords},
                                    SaveSceneCanonicalParticipant{owner, version, dataRecords}};
            auto canonical = EncodeSaveSceneCanonicalState(header, owners);
            REQUIRE(canonical.HasValue());
            return ComputeCanonicalStateHash(canonical.Value().Bytes());
        }

        /** @brief Produces the exact ordered stored directory through the production writer without reinterpreting opaque bytes. */
        void WriteArchive(const SaveArchiveHeader &header, const std::vector<std::byte> &declarationBytes,
                          const SaveSceneCanonicalRepresentation representation, const SaveChunkCodec codec) {
            const auto layoutBytes = LayoutBytes(representation);
            const auto hash = CanonicalRoot(header, declarationBytes, layoutBytes, representation, codec);
            const auto version = SceneContentTest::V<ParticipantSchemaVersion>();
            const SaveGameManifest manifest{SceneContentTest::V<SaveSchemaVersion>(2),
                                            hash,
                                            {{SaveContentRequirementsParticipant(), version, true, {SaveContentRequirementsRecord()}},
                                             {SaveSceneCanonicalLayoutParticipant(), version, true, {SaveSceneCanonicalLayoutRecord()}},
                                             {owner, version, false, {record}}}};
            std::array chunks{PreservedSaveChunk{{SaveContentRequirementsRecord(), SaveContentRequirementsParticipant(), 0,
                                                  declarationBytes.size(), declarationBytes.size(), 1, SaveChunkCodec::Raw,
                                                  ComputeSha256(declarationBytes)},
                                                 declarationBytes},
                              PreservedSaveChunk{{SaveSceneCanonicalLayoutRecord(), SaveSceneCanonicalLayoutParticipant(), 0,
                                                  layoutBytes.size(), layoutBytes.size(), 1, SaveChunkCodec::Raw,
                                                  ComputeSha256(layoutBytes)},
                                                 layoutBytes},
                              PreservedSaveChunk{{record, owner, 0, stored.size(), decoded.size(), 1, codec, digest}, stored}};
            std::ranges::sort(chunks, [](const PreservedSaveChunk &left, const PreservedSaveChunk &right) {
                return left.entry.record < right.entry.record;
            });
            auto written = SaveArchiveContainerWriter::Write(header, manifest, chunks, SceneContentTest::V<ArchiveFormatVersion>(2));
            if (written.HasError()) {
                INFO("retained fixture codec=" << static_cast<unsigned>(codec) << " stored=" << stored.size()
                                               << " decoded=" << decoded.size() << " error=" << written.ErrorValue().code.Value());
                std::string diagnostics;
                for (const auto &diagnostic : written.ErrorValue().diagnostics)
                    diagnostics += diagnostic.path + ": " + diagnostic.message + "; ";
                INFO("writer/reader diagnostics: " << diagnostics);
                REQUIRE(written.HasValue());
            }
            REQUIRE(written.HasValue());
            archive = written.Value().Archive();
            policy = source.policy;
            policy.compatibility.archiveVersions.direct.maximum = SceneContentTest::V<ArchiveFormatVersion>(2);
            policy.compatibility.saveSchemaVersions.direct = {SceneContentTest::V<SaveSchemaVersion>(2),
                                                              SceneContentTest::V<SaveSchemaVersion>(2)};
        }
    };
}  // namespace Horo::Runtime::SceneContentRetentionTest
