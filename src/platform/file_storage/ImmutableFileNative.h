#pragma once

#include "Horo/Foundation/ImmutableFileStorage.h"
#include "ProjectReadNative.h"

#include <string>

namespace Horo::Platform::ImmutableFileNative {
    using ProjectReadNative::Handle;

    /** @brief Opens one directory by retained parent identity; optional absence is distinct from an unsafe object. */
    [[nodiscard]] Result<std::optional<Handle>> Directory(const Handle &parent, std::string_view name, bool create);
    /** @brief Opens one regular single-link file without following a redirect; absence remains a cache miss. */
    [[nodiscard]] Result<std::optional<Handle>> Regular(const Handle &parent, std::string_view name, bool flushable = false);

    /** @brief Owns a private preparation directory and its fixed payload name; no user-supplied path is retained. */
    struct Preparation final {
        Handle directory;
        Handle file;
        std::string name;
        bool published{};
    };

    /** @brief Exclusively creates a private operation directory and payload through a retained parent. */
    [[nodiscard]] Result<std::optional<Preparation>> Prepare(const Handle &parent, std::string name);
    /** @brief Writes and flushes the complete owned payload in bounded cancellation-aware transfers. */
    [[nodiscard]] Result<void> Write(const Handle &file, std::span<const std::byte> bytes, const CancellationToken &cancellation);
    /** @brief Atomically moves the operation-owned payload to an absent destination; false means destination already exists. */
    [[nodiscard]] Result<bool> Publish(const Handle &parent, Preparation &preparation, std::string_view destination);
    /** @brief Synchronizes an opened file or directory according to native durability policy. */
    [[nodiscard]] Result<void> Sync(const Handle &directory);
    /** @brief Removes only operation-owned private preparation content; never traverses another directory. */
    void Discard(const Handle &parent, Preparation &preparation) noexcept;
}  // namespace Horo::Platform::ImmutableFileNative
