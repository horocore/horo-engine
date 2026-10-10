#pragma once

#include "Horo/Platform/NativeProjectReadFiles.h"

#include <array>
#include <span>

namespace Horo::Platform::ProjectReadNative {
#if defined(_WIN32)
    using HandleValue = void *;
    inline constexpr HandleValue InvalidHandle = nullptr;
#else
    using HandleValue = int;
    inline constexpr HandleValue InvalidHandle = -1;
#endif
    /** @brief Unique private native identity; never published in a Horo public contract. */
    class Handle final {
    public:
        explicit Handle(HandleValue value = InvalidHandle) noexcept;
        ~Handle();
        Handle(const Handle &) = delete;
        Handle &operator=(const Handle &) = delete;
        Handle(Handle &&other) noexcept;
        Handle &operator=(Handle &&other) noexcept;
        [[nodiscard]] HandleValue Get() const noexcept;
        [[nodiscard]] HandleValue Release() noexcept;
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        HandleValue value_;
    };

    /** @brief Native identity/mutation metadata captured without following a pathname after admission. */
    struct FileInfo final {
        bool regular{};
        bool directory{};
        std::uint64_t length{};
        std::uint64_t links{};
        std::array<std::uint64_t, 6> mutation{};
        [[nodiscard]] bool operator==(const FileInfo &) const noexcept = default;
    };

    /** @brief Normalizes the concrete invalid value without exposing native handle types. */
    [[nodiscard]] HandleValue NormalizeHandle(HandleValue value) noexcept;
    /** @brief Closes one valid unique native handle; called only by its RAII owner. */
    void CloseHandleValue(HandleValue value) noexcept;
    /** @brief Opens the platform root anchor; descendant components remain handle-relative. */
    [[nodiscard]] Result<Handle> OpenAnchor(const std::filesystem::path &path);
    /** @brief Encodes an admitted native root component without lossy replacement. */
    [[nodiscard]] Result<std::string> ComponentName(const std::filesystem::path &part);
    /** @brief Applies the same type/link admission to every opened child and directory cursor. */
    [[nodiscard]] Result<Handle> AdmitChild(Handle child, bool directory);

    /** @brief Opens every root component without following links/reparse points.
     * @param path Absolute authorized root. @return Owned directory identity or typed failure.
     */
    [[nodiscard]] Result<Handle> OpenRoot(const std::filesystem::path &path);
    /** @brief Opens an independent directory enumeration cursor on the same owned identity.
     * @param directory Retained parent identity. @return Owned independent directory handle or typed failure.
     */
    [[nodiscard]] Result<Handle> OpenDirectory(const Handle &directory);
    /** @brief Opens one exact child relative to a retained parent identity, without following links.
     * @param parent Retained parent. @param name Single validated segment. @param directory Require a directory.
     * @return Owned child identity or typed failure; special files cannot cause a blocking open.
     */
    [[nodiscard]] Result<Handle> OpenChild(const Handle &parent, std::string_view name, bool directory);
    /** @brief Captures metadata from an opened object.
     * @param handle Owned object. @return Typed identity/mutation metadata or failure.
     */
    [[nodiscard]] Result<FileInfo> Info(const Handle &handle);
    /** @brief Reads one bounded chunk from an admitted regular handle.
     * @param handle Independent read cursor. @param bytes Writable bounded chunk.
     * @return Bytes read, zero at EOF, or typed failure.
     */
    [[nodiscard]] Result<std::size_t> Read(const Handle &handle, std::span<char> bytes);
    /** @brief Visits finite native directory entries through the same retained identity.
     * @param handle Independent directory cursor. @param context Stop view checked between native calls.
     * @param visitor Bounded caller-owned admission/recursion callback; no callback is retained.
     * @return Success after complete enumeration or exact visitor/native failure.
     */
    [[nodiscard]] Result<void> Visit(const Handle &handle, const ProjectReadContext &context,
                                     const std::function<Result<void>(std::string_view)> &visitor);
}  // namespace Horo::Platform::ProjectReadNative
