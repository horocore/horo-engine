#pragma once

/** @file OfflineAudioExport.h @brief Host-owned deterministic PCM rendering and transactional WAVE publication. */

#include "Horo/Audio/Internal/OfflineAudioRender.h"
#include "Horo/Foundation/Platform.h"

namespace Horo::Application::Internal {
    /** @brief Reserved private staging child below the host-authorized destination directory. */
    inline constexpr std::string_view OfflineAudioWaveStagingDirectoryName = ".horo-audio-export-staging";
    /** @brief Nonblocking export writer key below the staging directory, disjoint from direct prepared-file leaves. */
    inline constexpr std::string_view OfflineAudioWaveWriterLock = ".leases/wave.lock";

    /** @brief Explicit offline core plan and two host-authorized publication paths; no device or timeline discovery. */
    struct OfflineAudioWaveExportRequest final {
        Audio::Backend::OfflineAudioRenderRequest render;
        std::filesystem::path preparedFile; /**< Fresh leaf under destination.parent_path()/OfflineAudioWaveStagingDirectoryName. */
        std::filesystem::path destination;
        std::uint64_t maximumEncodedBytes{64 * 1'024 * 1'024}; /**< Complete WAVE bytes, bounded by MaximumOfflineAudioOutputBytes. */
    };

    /** @brief Caller-owned irreversible evidence, retained through errors and exceptions; never reused or reset. */
    struct OfflineAudioWaveExportReceipt final {
        bool admitted{}; /**< One-shot operation admission before entering the prepared core; retained even without file creation. */
        bool preparedCreated{};
        bool preparedRemoved{}; /**< True only after successful pre-commit cleanup; commit consumes the prepared file separately. */
        AtomicFileReplacementReceipt publication;
    };

    /** @brief Complete published float32 WAVE shape; canonical speaker roles are reordered to WAVE mask order without mixing. */
    struct OfflineAudioWaveExportSummary final {
        std::uint64_t contentFrames{};
        std::uint64_t tailFrames{};
        std::uint64_t encodedBytes{};
        std::uint32_t sampleRate{};
        std::uint32_t speakerMask{};
    };

    /**
     * @brief Render an exclusive prepared core, encode complete float32 speaker PCM and durably publish it through the host authority.
     * @param request Bounded render/tail plan and absolute paths within the supplied authority.
     * @param render Fresh exclusive offline core, retained with all media and DSP state through this synchronous operation.
     * @param files Host-owned no-follow filesystem authority; no native fallback or direct filesystem writes occur here.
     * @param receipt Fresh caller-owned evidence, retained even if error construction or a filesystem operation throws.
     * @return Published shape only after durable commit; otherwise typed render/format/cancellation/I/O failure with its native cause.
     * @pre Caller exclusively owns the offline core and protected staging/destination namespaces through cleanup.
     * Both directories already exist within a no-follow authority;
     * staging is the reserved private child of the destination directory. No active device core may be borrowed.
     * @details Speaker layouts use WAVEFORMATEXTENSIBLE with IEEE float32 and an explicit channel permutation. Discrete/Ambisonic layouts
     * reject before rendering rather than losing their semantics. Content plus declared mixer tail is retained, including finite headroom.
     * PCM and encoded storage are each bounded by MaximumOfflineAudioOutputBytes; prepared core storage is caller-owned separately.
     * A dedicated nonblocking export writer lease is acquired before rendering and retained through cleanup, including exceptions.
     * Cancellation is checked between render blocks, bounded encoding batches and before publication; synchronous filesystem calls
     * cannot be interrupted. Commit wins cancellation. A post-commit durability failure leaves publication.WasCommitted() true and must
     * never be reported as rollback. Pre-commit failure attempts cleanup only when this invocation created the private prepared file.
     * Failed/throwing cleanup leaves its receipt caller-owned; no pre-existing prepared file is removed or truncated.
     * @throws std::bad_alloc If an allocation failure cannot be represented as a Result; receipt still records any irreversible work.
     */
    [[nodiscard]] Result<OfflineAudioWaveExportSummary> ExportOfflineAudioWave(const OfflineAudioWaveExportRequest &request,
                                                                               Audio::Backend::RenderPort render, DurableFileSystem &files,
                                                                               OfflineAudioWaveExportReceipt &receipt);
}  // namespace Horo::Application::Internal
