#include "Horo/Editor/WorkspaceLayoutPersistence.h"
#include "Horo/Foundation/Platform.h"
#include "WorkspaceLayoutPersistenceInternal.h"

#include <array>
#include <fstream>
#include <span>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        /** @brief Releases only the private stage created by this invocation, never a pre-existing file or the destination. */
        struct WorkspaceStage {
            DurableFileSystem &files;
            const std::filesystem::path &path;
            const AtomicFileReplacementReceipt &receipt;
            bool created{};

            /** @brief Borrows invocation-owned publication state through the final cleanup decision. */
            WorkspaceStage(DurableFileSystem &files, const std::filesystem::path &path,
                           const AtomicFileReplacementReceipt &receipt) noexcept
                : files(files), path(path), receipt(receipt) {}

            WorkspaceStage(const WorkspaceStage &) = delete;
            WorkspaceStage &operator=(const WorkspaceStage &) = delete;
            WorkspaceStage(WorkspaceStage &&) = delete;
            WorkspaceStage &operator=(WorkspaceStage &&) = delete;

            ~WorkspaceStage() noexcept {
                try {
                    if (created && !receipt.WasCommitted())
                        static_cast<void>(files.RemoveDurable(path));
                } catch (...) {  // NOSONAR(cpp:S2486) A destructor must preserve the primary publication failure.
                    // Retain an orphan private stage rather than mask the publication failure or terminate teardown.
                }
            }
        };

        static_assert(!std::is_copy_constructible_v<WorkspaceStage> && !std::is_move_constructible_v<WorkspaceStage>);

    }  // namespace

    /** @copydoc WorkspaceLayoutPersistence::Save */
    bool WorkspaceLayoutPersistence::Save(DurableFileSystem &files, AtomicFileReplacementReceipt &receipt,
                                          const std::filesystem::path &path, const WorkspaceLayout &layout, std::string *error) {
        const auto fail = [error](const std::string_view message) {
            if (error)
                *error = message;
            return false;
        };
        const auto encoded = Serialize(layout);
        if (!layout.Validate().empty() || !path.is_absolute() || receipt.WasCommitted() || encoded.empty() || !Deserialize(encoded))
            return fail("workspace candidate or publication path is invalid");
        auto lockPath = path;
        lockPath += ".lock";
        auto lockResult = files.TryAcquireExclusive(lockPath, "horo.workspace");
        if (lockResult.HasError())
            return fail(lockResult.ErrorValue().message);
        // The admitted writer lease must survive the stage write, replacement and cleanup.
        const auto writerLease = std::move(lockResult).Value();
        auto prepared = path;
        prepared += ".pending";
        WorkspaceStage stage{files, prepared, receipt};
        const auto bytes = std::as_bytes(std::span{encoded.data(), encoded.size()});
        if (const auto written = files.WritePrivateDurable(prepared, bytes, stage.created); written.HasError())
            return fail(written.ErrorValue().message);
        if (const auto replaced = files.AtomicReplaceTracked(prepared, path, receipt); replaced.HasError())
            return fail(receipt.WasCommitted()
                            ? "workspace replacement committed but durability is uncertain: " + replaced.ErrorValue().message
                            : replaced.ErrorValue().message);
        return true;
    }

    std::optional<WorkspaceLayout> WorkspaceLayoutPersistence::Load(const std::filesystem::path &path, std::string *error) {
        std::error_code sizeError;
        if (const auto size = std::filesystem::file_size(path, sizeError); sizeError || size > MaximumWorkspaceDocumentBytes) {
            if (error)
                *error = "workspace document unavailable or exceeds limit";
            return std::nullopt;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            if (error)
                *error = "workspace file not found";
            return std::nullopt;
        }
        std::string document;
        std::array<char, 8192> buffer{};
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = static_cast<std::size_t>(in.gcount());
            if (count > MaximumWorkspaceDocumentBytes - document.size()) {
                if (error)
                    *error = "workspace document exceeds limit while reading";
                return std::nullopt;
            }
            document.append(buffer.data(), count);
        }
        if (in.bad()) {
            if (error)
                *error = "workspace read failed";
            return std::nullopt;
        }
        return Deserialize(document, error);
    }
}  // namespace Horo::Editor
