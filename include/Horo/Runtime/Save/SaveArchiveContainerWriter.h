#pragma once

/** @file SaveArchiveContainerWriter.h
 * @brief Bounded assembly of the existing reader-compatible save container.
 */
#include "Horo/Runtime/Save/SaveArchiveFinalization.h"
#include "Horo/Runtime/Save/SaveArchiveReader.h"

namespace Horo::Runtime {
    /** @brief Load-time assembler; no mutable world state or participant callback is retained. */
    class SaveArchiveContainerWriter final {
    public:
        /** @brief Builds an unsigned existing-format container and admits its exact bytes with the production reader.
         * @param header Canonical publication metadata.
         * @param manifest Exact semantic owners and stable record inventory.
         * @param chunks Record-sorted stored chunks; codecs, digests and alignment are preserved.
         * @param version Existing archive/container version, 1 or 2.
         * @param limits Trusted finite reader and construction bounds.
         * @return Owned finalized archive only after complete reader admission; no partial bytes escape.
         * @details Raw stored bytes must match their supplied decoded digest; raw hashing is charged before allocation.
         *          Compressed/unknown codecs remain opaque here; this assembler does not decode or reinterpret their payloads.
         *          Raw digest work and reader admission share maximumReadWorkBytes; sorted duplicate records are rejected.
         */
        [[nodiscard]] static Result<FinalizedSaveArchive> Write(const SaveArchiveHeader &header, const SaveGameManifest &manifest,
                                                                std::span<const PreservedSaveChunk> chunks, ArchiveFormatVersion version,
                                                                const SaveArchiveReaderLimits &limits = {});
    };
}  // namespace Horo::Runtime
