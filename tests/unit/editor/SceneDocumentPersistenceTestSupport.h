#pragma once

/**
 * @file SceneDocumentPersistenceTestSupport.h
 * @brief Shared fixtures for scene persistence contract tests.
 */

#include "editor/document/SceneDocumentPersistence.h"
#include "editor/document/SceneFileWatchService.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace Horo::Editor::PersistenceTestSupport {
    class TemporaryProject final {
    public:
        TemporaryProject();
        ~TemporaryProject();

        TemporaryProject(const TemporaryProject &) = delete;
        TemporaryProject &operator=(const TemporaryProject &) = delete;

        [[nodiscard]] const std::filesystem::path &Root() const noexcept;
        [[nodiscard]] std::filesystem::path ScenePath() const;
        [[nodiscard]] std::filesystem::path RecoveryPath() const;
        void PrepareEmptyScene(std::string_view contents = "{\"schemaVersion\":1,\"objects\":[]}\n") const;
        void WriteMetadata(const std::string &defaultScene = "assets/scenes/main.horo") const;
        void WriteScene(const std::string &contents) const;

    private:
        std::filesystem::path root_;
    };

    class NativeBackedFileSystem : public DurableFileSystem {
    public:
        [[nodiscard]] Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path,
                                                                    std::string_view ownerMetadata) override;
        [[nodiscard]] Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override;
        [[nodiscard]] Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override;
        [[nodiscard]] Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override;
        [[nodiscard]] Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override;
        [[nodiscard]] Result<void> RemoveDurable(const std::filesystem::path &path) override;
        [[nodiscard]] Result<void> SyncDirectory(const std::filesystem::path &path) override;

    protected:
        NativeDurableFileSystem native_;
    };

    class ReplaceFailingFileSystem final : public NativeBackedFileSystem {
    public:
        [[nodiscard]] Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override;
    };

    class InterferingFileSystem final : public NativeBackedFileSystem {
    public:
        explicit InterferingFileSystem(std::filesystem::path canonicalPath);

        [[nodiscard]] Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override;

    private:
        std::filesystem::path canonicalPath_;
    };

    [[nodiscard]] SceneDocumentSnapshot AuthoredScene();
    [[nodiscard]] SceneFileWatchUpdate WaitForWatchUpdate(SceneFileWatchService &watcher);
    void RequireSameSceneObject(const SceneObjectSnapshot &actual, const SceneObjectSnapshot &expected);
}  // namespace Horo::Editor::PersistenceTestSupport
