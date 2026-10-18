#include "Horo/Foundation/Sha256.h"
#include "Horo/Foundation/Utf8.h"
#include "ProjectCodeFilesTesting.h"
#include "ProjectReadNative.h"

#include <algorithm>
#include <array>
#include <new>

namespace Horo::Platform {
    namespace {
        namespace Native = ProjectReadNative;
        using CodeFilesTesting::Boundary;

        /** @brief Validates hard native budgets independently of any application query configuration. */
        bool ValidLimits(const ProjectReadLimits &limits, const ProjectReadLimits &hard = {}) {
            return limits.maximumEntries > 0 && limits.maximumEntries <= hard.maximumEntries && limits.maximumFileBytes > 0 &&
                   limits.maximumFileBytes <= hard.maximumFileBytes && limits.maximumManifestBytes > 0 &&
                   limits.maximumManifestBytes <= hard.maximumManifestBytes && limits.maximumDuration.count() > 0 &&
                   limits.maximumDuration <= hard.maximumDuration;
        }

        /** @brief Admits exact contained inventory spellings without importing the code-query device/name profile. */
        bool SafeInventoryPath(const std::string_view path) {
            if (path.empty() || path.size() > 4096 || path.front() == '/' || path.back() == '/' ||
                path.find('\0') != std::string_view::npos)
                return false;
            std::size_t start{};
            while (start < path.size()) {
                const auto end = path.find('/', start);
                if (const auto part = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
                    part.empty() || part == "." || part == "..")
                    return false;
                if (end == std::string_view::npos)
                    break;
                start = end + 1;
            }
            return true;
        }

        /** @brief Applies one native call budget while preserving borrowed cancellation and authority. */
        ProjectReadContext BoundedContext(const ProjectReadContext &input, const ProjectReadLimits &limits) {
            return {input.projectIdentity, input.projectGeneration, input.cancellation,
                    std::min(input.deadline, std::chrono::steady_clock::now() + limits.maximumDuration), input.authorityStopped};
        }

        /** @brief Retains one root identity; shared portable logic never opens a descendant by absolute pathname. */
        class NativeFiles final : public IProjectReadFiles {
        public:
            NativeFiles(Native::Handle root, std::string identity, const std::uint64_t generation, CodeFilesTesting::Observer observer)
                : root_(std::move(root)), identity_(std::move(identity)), generation_(generation), observer_(std::move(observer)) {}

            /** @copydoc IProjectReadFiles::Text */
            Result<ProjectReadTextSnapshot> Text(const std::string_view path, const ProjectReadContext &input,
                                                 const ProjectReadLimits &limits) const override {
                return CaptureText(path, input, limits, false);
            }

            /** @copydoc IProjectReadFiles::SourceText */
            Result<ProjectReadTextSnapshot> SourceText(const std::string_view path, const ProjectReadContext &input,
                                                       const ProjectSourceReadLimits &limits) const override {
                const ProjectSourceReadLimits sourceHard;
                const ProjectReadLimits hard{.maximumFileBytes = sourceHard.maximumFileBytes,
                                             .maximumDuration = sourceHard.maximumDuration};
                const ProjectReadLimits requested{.maximumFileBytes = limits.maximumFileBytes, .maximumDuration = limits.maximumDuration};
                return CaptureText(path, input, requested, false, hard);
            }

            /** @copydoc IProjectReadFiles::InventoryText */
            Result<ProjectReadTextSnapshot> InventoryText(std::string_view path, const ProjectReadContext &input) const override {
                return CaptureText(path, input, {}, true);
            }

        private:
            /** @brief Shares one native byte reader while preserving distinct query and inventory hardlink admission. */
            Result<ProjectReadTextSnapshot> CaptureText(std::string_view path, const ProjectReadContext &input,
                                                        const ProjectReadLimits &limits, bool allowRegularHardlinks,
                                                        const ProjectReadLimits &hard = {}) const {
                try {
                    if (input.projectIdentity != identity_ || input.projectGeneration != generation_)
                        return Result<ProjectReadTextSnapshot>::Failure(MakeError(ProjectReadErrors::Stale));
                    if (!(allowRegularHardlinks ? SafeInventoryPath(path) : IsSafeProjectReadPath(path)))
                        return Result<ProjectReadTextSnapshot>::Failure(MakeError(ProjectReadErrors::UnsafePath));
                    if (!ValidLimits(limits, hard))
                        return Result<ProjectReadTextSnapshot>::Failure(MakeError(ProjectReadErrors::Invalid));
                    const auto context = allowRegularHardlinks ? input : BoundedContext(input, limits);
                    if (auto stop = CheckProjectReadContext(context); stop.HasError())
                        return Result<ProjectReadTextSnapshot>::Failure(stop.ErrorValue());
                    auto file = Open(path, false, context, allowRegularHardlinks);
                    if (file.HasError())
                        return Result<ProjectReadTextSnapshot>::Failure(file.ErrorValue());
                    auto text = Read(file.Value(), path, context, limits, allowRegularHardlinks);
                    if (text.HasError())
                        return Result<ProjectReadTextSnapshot>::Failure(text.ErrorValue());
                    const auto &bytes = *text.Value();
                    const auto revision = FormatSha256(ComputeSha256(std::as_bytes(std::span{bytes.data(), bytes.size()})));
                    if (auto stop = CheckProjectReadContext(context); stop.HasError())
                        return Result<ProjectReadTextSnapshot>::Failure(stop.ErrorValue());
                    return Result<ProjectReadTextSnapshot>::Success(
                        {identity_, generation_, std::string{path}, revision, std::move(text).Value()});
                } catch (const std::bad_alloc &) {
                    return Result<ProjectReadTextSnapshot>::Failure(MakeError(ProjectReadErrors::Capacity));
                }
            }

        public:
            /** @copydoc IProjectReadFiles::Files */
            Result<ProjectReadManifest> Files(const std::string_view prefix, const ProjectReadContext &input,
                                              const ProjectReadLimits &limits) const override {
                if (auto admitted = Admit(prefix, true, input, limits); admitted.HasError())
                    return Result<ProjectReadManifest>::Failure(admitted.ErrorValue());
                try {
                    const auto context = BoundedContext(input, limits);
                    auto directory = Open(prefix, true, context);
                    if (directory.HasError())
                        return Result<ProjectReadManifest>::Failure(directory.ErrorValue());
                    ProjectReadManifest manifest{identity_, generation_, {}, {}};
                    std::size_t entries{};
                    std::size_t bytes{};
                    if (auto captured = Capture(directory.Value(), prefix, context, limits, entries, bytes, manifest.entries);
                        captured.HasError())
                        return Result<ProjectReadManifest>::Failure(captured.ErrorValue());
                    std::ranges::sort(manifest.entries, {}, &ProjectReadEntry::path);
                    std::string names;
                    names.reserve(bytes);
                    for (const auto &entry : manifest.entries)
                        names.append(entry.path).push_back('\n');
                    manifest.revision = FormatSha256(ComputeSha256(std::as_bytes(std::span{names.data(), names.size()})));
                    if (auto stop = CheckProjectReadContext(context); stop.HasError())
                        return Result<ProjectReadManifest>::Failure(stop.ErrorValue());
                    return Result<ProjectReadManifest>::Success(std::move(manifest));
                } catch (const std::bad_alloc &) {
                    return Result<ProjectReadManifest>::Failure(MakeError(ProjectReadErrors::Capacity));
                }
            }

            /** @copydoc IProjectReadFiles::Inventory */
            Result<ProjectInventory> Inventory(const std::string_view prefix, const ProjectInventoryDepth depth,
                                               const ProjectReadContext &input, const ProjectInventoryLimits &limits) const override {
                const ProjectInventoryLimits hard;
                if (input.projectIdentity != identity_ || input.projectGeneration != generation_)
                    return Result<ProjectInventory>::Failure(MakeError(ProjectReadErrors::Stale));
                if (!prefix.empty() && !SafeInventoryPath(prefix))
                    return Result<ProjectInventory>::Failure(MakeError(ProjectReadErrors::UnsafePath));
                if (limits.maximumEntries > hard.maximumEntries || limits.maximumManifestBytes > hard.maximumManifestBytes ||
                    limits.maximumDuration.count() <= 0 || limits.maximumDuration > hard.maximumDuration ||
                    (depth != ProjectInventoryDepth::Children && depth != ProjectInventoryDepth::Recursive))
                    return Result<ProjectInventory>::Failure(MakeError(ProjectReadErrors::Invalid));
                try {
                    const ProjectReadContext context{input.projectIdentity, input.projectGeneration, input.cancellation,
                                                     std::min(input.deadline, std::chrono::steady_clock::now() + limits.maximumDuration),
                                                     input.authorityStopped};
                    if (auto stopped = CheckProjectReadContext(context); stopped.HasError())
                        return Result<ProjectInventory>::Failure(stopped.ErrorValue());
                    auto directory = Open(prefix, true, context);
                    if (directory.HasError())
                        return Result<ProjectInventory>::Failure(directory.ErrorValue());
                    ProjectInventory result;
                    std::size_t bytes{};
                    InventoryState state{context, limits, depth, bytes, result.entries, {}};
                    if (auto captured = CaptureInventory(directory.Value(), prefix, state); captured.HasError())
                        return Result<ProjectInventory>::Failure(captured.ErrorValue());
                    std::ranges::sort(result.entries, {}, &ProjectInventoryEntry::path);
                    if (auto stopped = CheckProjectReadContext(context); stopped.HasError())
                        return Result<ProjectInventory>::Failure(stopped.ErrorValue());
                    return Result<ProjectInventory>::Success(std::move(result));
                } catch (const std::bad_alloc &) {
                    return Result<ProjectInventory>::Failure(MakeError(ProjectReadErrors::Capacity));
                }
            }

        private:
            /** @brief Borrows the caller's independent inventory budget throughout synchronous traversal. */
            struct InventoryState final {
                const ProjectReadContext &context;
                const ProjectInventoryLimits &limits;
                ProjectInventoryDepth depth;
                std::size_t &bytes;
                std::vector<ProjectInventoryEntry> &entries;
                std::vector<std::size_t> pendingDirectories;
            };

            /** @brief Reports links/special files without opening them and queues only directory spellings. */
            struct InventoryVisit final : Native::EntryVisitor {
                /** @brief Borrows caller-owned traversal state only during one synchronous native Visit. */
                InventoryVisit(InventoryState &state, std::string_view prefix) noexcept : prefix(prefix), state(state) {}

                /** @copydoc Native::EntryVisitor::operator() */
                Result<void> operator()(std::string_view) const override {
                    return Result<void>::Failure(MakeError(ProjectReadErrors::Unavailable));
                }

                /** @copydoc Native::EntryVisitor::Classified */
                Result<void> Classified(const std::string_view name, const ProjectInventoryKind kind) const override {
                    const std::string path = prefix.empty() ? std::string{name} : std::string{prefix} + '/' + std::string{name};
                    if (state.entries.size() >= state.limits.maximumEntries ||
                        path.size() + 1 > state.limits.maximumManifestBytes - state.bytes)
                        return Result<void>::Failure(MakeError(ProjectReadErrors::Capacity));
                    state.bytes += path.size() + 1;
                    state.entries.emplace_back(path, kind);
                    if (kind != ProjectInventoryKind::Directory || state.depth != ProjectInventoryDepth::Recursive)
                        return Result<void>::Success();
                    state.pendingDirectories.push_back(state.entries.size() - 1);
                    return Result<void>::Success();
                }

                std::string_view prefix;
                InventoryState &state;
            };

            /** @brief Traverses admitted directories iteratively; depth never consumes unbounded C++ stack or native handles. */
            Result<void> CaptureInventory(const Native::Handle &directory, const std::string_view prefix, InventoryState &state) const {
                InventoryVisit first{state, prefix};
                if (auto visited = Native::Visit(directory, state.context, first); visited.HasError())
                    return visited;
                while (!state.pendingDirectories.empty()) {
                    const std::string path = state.entries[state.pendingDirectories.back()].path;
                    state.pendingDirectories.pop_back();
                    if (auto stopped = CheckProjectReadContext(state.context); stopped.HasError())
                        return stopped;
                    auto child = Open(path, true, state.context);
                    if (child.HasError())
                        return Result<void>::Failure(child.ErrorValue());
                    InventoryVisit next{state, path};
                    if (auto visited = Native::Visit(child.Value(), state.context, next); visited.HasError())
                        return visited;
                }
                return Result<void>::Success();
            }

            /** @brief Shares one finite enumeration budget across synchronous handle-relative recursion. */
            struct CaptureState final {
                const ProjectReadContext &context;
                const ProjectReadLimits &limits;
                std::size_t &entries;
                std::size_t &bytes;
                std::vector<ProjectReadEntry> &records;
            };

            /** @brief Borrows one opened directory and its caller-owned state only for the current Visit call. */
            struct CaptureVisit final : Native::EntryVisitor {
                CaptureVisit(const NativeFiles &owner, const Native::Handle &openedDirectory, const std::string_view pathPrefix,
                             CaptureState &captureState) noexcept
                    : files(owner), directory(openedDirectory), prefix(pathPrefix), state(captureState) {}

                /** @copydoc Native::EntryVisitor::operator() */
                [[nodiscard]] Result<void> operator()(const std::string_view name) const override {
                    return files.CaptureEntry(directory, prefix, name, state);
                }

                const NativeFiles &files;
                const Native::Handle &directory;
                std::string_view prefix;
                CaptureState &state;
            };

            /** @brief Rejects invalid authority or budgets before any native descendant access. */
            Result<void> Admit(const std::string_view path, const bool allowRoot, const ProjectReadContext &context,
                               const ProjectReadLimits &limits) const {
                if (context.projectIdentity != identity_ || context.projectGeneration != generation_)
                    return Result<void>::Failure(MakeError(ProjectReadErrors::Stale));
                if (!IsSafeProjectReadPath(path, allowRoot))
                    return Result<void>::Failure(MakeError(ProjectReadErrors::UnsafePath));
                if (!ValidLimits(limits))
                    return Result<void>::Failure(MakeError(ProjectReadErrors::Invalid));
                return CheckProjectReadContext(context);
            }

            /** @brief Provides an instance-owned deterministic test boundary; production observers are absent. */
            void Observe(const Boundary boundary, const std::string_view path) const {
                if (observer_)
                    observer_(boundary, path);
            }

            /** @brief Resolves each segment only against the previous opened directory identity. */
            Result<Native::Handle> Open(const std::string_view path, const bool directory, const ProjectReadContext &context,
                                        bool allowRegularHardlinks = false) const {
                auto current = Native::OpenDirectory(root_);
                if (current.HasError())
                    return current;
                std::size_t start{};
                while (start < path.size()) {
                    const auto end = path.find('/', start);
                    const bool last = end == std::string_view::npos;
                    const auto name = path.substr(start, last ? path.size() - start : end - start);
                    Observe(Boundary::BeforeComponentOpen, path.substr(0, last ? path.size() : end));
                    if (auto stop = CheckProjectReadContext(context); stop.HasError())
                        return Result<Native::Handle>::Failure(stop.ErrorValue());
                    auto child = Native::OpenChild(current.Value(), name, !last || directory, last && allowRegularHardlinks);
                    if (child.HasError())
                        return child;
                    current = std::move(child);
                    if (last)
                        break;
                    start = end + 1;
                }
                return current;
            }

            /** @brief Captures bounded bytes from an admitted regular object and fences mutation metadata around the read. */
            Result<std::shared_ptr<const std::string>> Read(const Native::Handle &file, const std::string_view path,
                                                            const ProjectReadContext &context, const ProjectReadLimits &limits,
                                                            bool allowRegularHardlinks) const {
                auto before = Native::Info(file);
                if (before.HasError())
                    return Result<std::shared_ptr<const std::string>>::Failure(before.ErrorValue());
                if (!before.Value().regular || (before.Value().links != 1 && !allowRegularHardlinks))
                    return Result<std::shared_ptr<const std::string>>::Failure(MakeError(ProjectReadErrors::UnsafePath));
                if (before.Value().length > limits.maximumFileBytes)
                    return Result<std::shared_ptr<const std::string>>::Failure(MakeError(ProjectReadErrors::Capacity));
                Observe(Boundary::AfterFileOpen, path);
                auto text = std::make_shared<std::string>();
                text->reserve(static_cast<std::size_t>(before.Value().length));
                std::array<char, Native::ReadChunkBytes> bytes{};
                for (;;) {
                    Observe(Boundary::BeforeRead, path);
                    if (auto stop = CheckProjectReadContext(context); stop.HasError())
                        return Result<std::shared_ptr<const std::string>>::Failure(stop.ErrorValue());
                    auto count = Native::Read(file, bytes);
                    if (count.HasError())
                        return Result<std::shared_ptr<const std::string>>::Failure(count.ErrorValue());
                    if (count.Value() == 0)
                        break;
                    if (count.Value() > bytes.size() || count.Value() > limits.maximumFileBytes - text->size())
                        return Result<std::shared_ptr<const std::string>>::Failure(MakeError(ProjectReadErrors::Capacity));
                    text->append(bytes.data(), count.Value());
                }
                auto after = Native::Info(file);
                if (after.HasError())
                    return Result<std::shared_ptr<const std::string>>::Failure(after.ErrorValue());
                if (before.Value() != after.Value() || after.Value().length != text->size())
                    return Result<std::shared_ptr<const std::string>>::Failure(MakeError(ProjectReadErrors::Stale));
                return Result<std::shared_ptr<const std::string>>::Success(std::move(text));
            }

            /** @brief Recurses through opened directories with one shared entry/byte budget, never pathname traversal. */
            Result<void> Capture(const Native::Handle &directory, const std::string_view prefix, const ProjectReadContext &context,
                                 const ProjectReadLimits &limits, std::size_t &entries, std::size_t &bytes,
                                 std::vector<ProjectReadEntry> &records) const {
                CaptureState state{context, limits, entries, bytes, records};
                CaptureVisit capture{*this, directory, prefix, state};
                return Native::Visit(directory, context, capture);
            }

            /** @brief Admits one entry before recursion or publication, preserving the shared byte and entry ceilings. */
            Result<void> CaptureEntry(const Native::Handle &directory, const std::string_view prefix, const std::string_view name,
                                      CaptureState &state) const {
                if (++state.entries > state.limits.maximumEntries)
                    return Result<void>::Failure(MakeError(ProjectReadErrors::Capacity));
                const std::string path = prefix.empty() ? std::string{name} : std::string{prefix} + '/' + std::string{name};
                if (!IsSafeProjectReadPath(path))
                    return Result<void>::Failure(MakeError(ProjectReadErrors::UnsafePath));
                Observe(Boundary::BeforeDirectoryEntryOpen, path);
                if (auto stop = CheckProjectReadContext(state.context); stop.HasError())
                    return stop;
                auto child = Native::OpenChild(directory, name, false);
                if (child.HasError())
                    return Result<void>::Failure(child.ErrorValue());
                auto info = Native::Info(child.Value());
                if (info.HasError())
                    return Result<void>::Failure(info.ErrorValue());
                if (info.Value().directory)
                    return Capture(child.Value(), path, state.context, state.limits, state.entries, state.bytes, state.records);
                if (!info.Value().regular || info.Value().links != 1)
                    return Result<void>::Failure(MakeError(ProjectReadErrors::UnsafePath));
                if (path.size() + 1 > state.limits.maximumManifestBytes - state.bytes)
                    return Result<void>::Failure(MakeError(ProjectReadErrors::Capacity));
                state.bytes += path.size() + 1;
                state.records.emplace_back(path);
                return Result<void>::Success();
            }

            Native::Handle root_;
            std::string identity_;
            std::uint64_t generation_;
            CodeFilesTesting::Observer observer_;
        };
    }  // namespace

    /** @copydoc CodeFilesTesting::Create */
    Result<std::shared_ptr<const IProjectReadFiles>> CodeFilesTesting::Create(const std::filesystem::path &root, std::string identity,
                                                                              const std::uint64_t generation, Observer observer) {
        if (!root.is_absolute() || root.native().size() > 4096 ||
            root.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos || identity.empty() ||
            identity.size() > 256 || !IsValidUtf8ScalarSequence(identity) || generation == 0 ||
            std::ranges::any_of(root, [](const auto &part) {
            return part == "." || part == "..";
        }))
            return Result<std::shared_ptr<const IProjectReadFiles>>::Failure(MakeError(ProjectReadErrors::Invalid));
        try {
            auto directory = Native::OpenRoot(root);
            if (directory.HasError())
                return Result<std::shared_ptr<const IProjectReadFiles>>::Failure(directory.ErrorValue());
            if (auto cursor = Native::OpenDirectory(directory.Value()); cursor.HasError())
                return Result<std::shared_ptr<const IProjectReadFiles>>::Failure(cursor.ErrorValue());
            return Result<std::shared_ptr<const IProjectReadFiles>>::Success(
                std::make_shared<NativeFiles>(std::move(directory).Value(), std::move(identity), generation, std::move(observer)));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<const IProjectReadFiles>>::Failure(MakeError(ProjectReadErrors::Capacity));
        }
    }

    /** @copydoc CreateNativeProjectReadFiles */
    Result<std::shared_ptr<const IProjectReadFiles>> CreateNativeProjectReadFiles(const std::filesystem::path &root, std::string identity,
                                                                                  const std::uint64_t generation) {
        return CodeFilesTesting::Create(root, std::move(identity), generation, {});
    }
}  // namespace Horo::Platform
