#pragma once

#include "Horo/Assets/AssetSourcePublication.h"

#include <string_view>

namespace Horo::Editor {
    /** @brief Exact pair paths and identity of its registered semantic asset. */
    struct DecisionSaveJournalKey final {
        Assets::AssetId asset;
        std::string_view sourceKey;
        std::filesystem::path sourcePath;
    };

    /** @brief Inspects the exact bounded local journal without changing disk. */
    [[nodiscard]] Result<std::optional<Assets::AssetSourceIdentity>> ReadDecisionSaveJournalIdentity(const std::filesystem::path &root,
                                                                                                     Assets::AssetId asset);

    /** @brief Retires an interrupted save only for the exact external pair explicitly reviewed by the user.
     * @pre Caller holds the project mutation lease and has validated the registered source pair.
     */
    [[nodiscard]] Result<void> AcceptDecisionSaveJournal(const std::filesystem::path &root, const DecisionSaveJournalKey &key,
                                                         const Assets::AssetSourceIdentity &journal,
                                                         const Assets::AssetSourceIdentity &graph,
                                                         const Assets::AssetSourceIdentity &layout, DurableFileSystem &files);

    /** @brief Rejects a new write while an interrupted paired publication is unresolved. */
    [[nodiscard]] Result<void> AdmitDecisionSaveJournal(const std::filesystem::path &root, Assets::AssetId asset);

    /** @brief Records the undo bytes and exact pair identities before either canonical file is changed.
     * @pre Caller holds the project mutation lease and the destination leases for both files.
     */
    [[nodiscard]] Result<Assets::AssetSourceIdentity> PrepareDecisionSaveJournal(
        const std::filesystem::path &root, const DecisionSaveJournalKey &key, std::span<const std::byte> previousGraph,
        std::span<const std::byte> nextGraph, Assets::AssetSourceIdentity previousLayout, std::span<const std::byte> nextLayout,
        DurableFileSystem &files);

    /** @brief Clears only the exact owned journal after the pair is durable or restored. */
    [[nodiscard]] Result<void> ClearDecisionSaveJournal(const std::filesystem::path &root, Assets::AssetId asset,
                                                        const Assets::AssetSourceIdentity &identity, DurableFileSystem &files);

    /** @brief Rolls back only a proven half-commit and keeps fully published pairs.
     * @pre Caller holds the project mutation lease. An external revision is never overwritten.
     */
    [[nodiscard]] Result<void> RecoverDecisionSaveJournal(const std::filesystem::path &root, const DecisionSaveJournalKey &key,
                                                          DurableFileSystem &files);
}  // namespace Horo::Editor
