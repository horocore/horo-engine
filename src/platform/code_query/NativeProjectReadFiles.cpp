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
        bool ValidLimits(const ProjectReadLimits &limits) {
            const ProjectReadLimits hard;
            return limits.maximumEntries > 0 && limits.maximumEntries <= hard.maximumEntries && limits.maximumFileBytes > 0 &&
                   limits.maximumFileBytes <= hard.maximumFileBytes && limits.maximumManifestBytes > 0 &&
                   limits.maximumManifestBytes <= hard.maximumManifestBytes && limits.maximumDuration.count() > 0 &&
                   limits.maximumDuration <= hard.maximumDuration;
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
                if (auto admitted = Admit(path, false, input, limits); admitted.HasError())
                    return Result<ProjectReadTextSnapshot>::Failure(admitted.ErrorValue());
                try {
                    const auto context = BoundedContext(input, limits);
                    auto file = Open(path, false, context);
                    if (file.HasError())
                        return Result<ProjectReadTextSnapshot>::Failure(file.ErrorValue());
                    auto text = Read(file.Value(), path, context, limits);
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

        private:
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
            Result<Native::Handle> Open(const std::string_view path, const bool directory, const ProjectReadContext &context) const {
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
                    auto child = Native::OpenChild(current.Value(), name, !last || directory);
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
                                                            const ProjectReadContext &context, const ProjectReadLimits &limits) const {
                auto before = Native::Info(file);
                if (before.HasError())
                    return Result<std::shared_ptr<const std::string>>::Failure(before.ErrorValue());
                if (!before.Value().regular || before.Value().links != 1)
                    return Result<std::shared_ptr<const std::string>>::Failure(MakeError(ProjectReadErrors::UnsafePath));
                if (before.Value().length > limits.maximumFileBytes)
                    return Result<std::shared_ptr<const std::string>>::Failure(MakeError(ProjectReadErrors::Capacity));
                Observe(Boundary::AfterFileOpen, path);
                auto text = std::make_shared<std::string>();
                text->reserve(static_cast<std::size_t>(before.Value().length));
                std::array<char, 4096> bytes{};
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
                return Native::Visit(directory, context, [&](const std::string_view name) -> Result<void> {
                    if (++entries > limits.maximumEntries)
                        return Result<void>::Failure(MakeError(ProjectReadErrors::Capacity));
                    const std::string path = prefix.empty() ? std::string{name} : std::string{prefix} + '/' + std::string{name};
                    if (!IsSafeProjectReadPath(path))
                        return Result<void>::Failure(MakeError(ProjectReadErrors::UnsafePath));
                    Observe(Boundary::BeforeDirectoryEntryOpen, path);
                    if (auto stop = CheckProjectReadContext(context); stop.HasError())
                        return stop;
                    auto child = Native::OpenChild(directory, name, false);
                    if (child.HasError())
                        return Result<void>::Failure(child.ErrorValue());
                    auto info = Native::Info(child.Value());
                    if (info.HasError())
                        return Result<void>::Failure(info.ErrorValue());
                    if (info.Value().directory)
                        return Capture(child.Value(), path, context, limits, entries, bytes, records);
                    if (!info.Value().regular || info.Value().links != 1)
                        return Result<void>::Failure(MakeError(ProjectReadErrors::UnsafePath));
                    if (path.size() + 1 > limits.maximumManifestBytes - bytes)
                        return Result<void>::Failure(MakeError(ProjectReadErrors::Capacity));
                    bytes += path.size() + 1;
                    records.push_back({path});
                    return Result<void>::Success();
                });
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
