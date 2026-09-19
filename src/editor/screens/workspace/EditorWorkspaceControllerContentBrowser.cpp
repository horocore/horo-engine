#include "Horo/Assets/AssetReimport.h"
#include "Horo/Editor/ProjectIntegrityValidatorService.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/PathUtils.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "editor/menu/EditorMenuPlatform.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/GameplayBehaviorRequestValidation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] bool HasPathPrefix(const std::filesystem::path &root, const std::filesystem::path &candidate) {
            return Horo::Foundation::Paths::HasPathPrefix(root, candidate);
        }

        [[nodiscard]] std::filesystem::path NormalizeAbsolute(const std::filesystem::path &path) {
            std::error_code error;
            const std::filesystem::path absolute = std::filesystem::absolute(path, error).lexically_normal();
            if (error)
                return {};
            const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
            return error ? absolute : canonical;
        }

        [[nodiscard]] Error NativeGenerationError(const ProjectGameplayRegistry &generation, const std::string_view fallback) {
            if (!generation.Diagnostics().empty())
                return generation.Diagnostics().front().error;
            return MakeError(Gameplay::GameplayErrors::GameplayReloadRestoreFailed, std::string{fallback});
        }

        void LogNativeReloadFailure(const std::string_view phase, const Error &error) {
            LOG_ERROR("editor.gameplay", "Native gameplay reload %.*s: %s", static_cast<int>(phase.size()), phase.data(),
                      error.message.c_str());
        }

        [[nodiscard]] bool IsDirectContentBrowserEntry(const ContentBrowserDirectory &directory, const std::filesystem::path &candidate) {
            if (!candidate.is_absolute())
                return false;
            const std::filesystem::path normalized = NormalizeAbsolute(candidate);
            const std::filesystem::path root = NormalizeAbsolute(directory.absoluteRootPath);
            if (const std::filesystem::path current = NormalizeAbsolute(directory.absoluteCurrentPath);
                normalized.empty() || root.empty() || current.empty() || normalized.parent_path() != current ||
                !HasPathPrefix(root, normalized)) {
                return false;
            }
            std::error_code error;
            const auto status = std::filesystem::symlink_status(normalized, error);
            return !error && !std::filesystem::is_symlink(status) &&
                   (std::filesystem::is_directory(status) || std::filesystem::is_regular_file(status));
        }

        [[nodiscard]] std::optional<std::vector<std::filesystem::path>> ValidatedAssetCompanions(const std::filesystem::path &source,
                                                                                                 const bool requireIdentitySidecar) {
            std::error_code error;
            if (const std::filesystem::file_status sourceStatus = std::filesystem::symlink_status(source, error);
                error || std::filesystem::is_symlink(sourceStatus) || !std::filesystem::is_regular_file(sourceStatus)) {
                return std::nullopt;
            }

            std::vector<std::filesystem::path> paths{source};
            for (const char *suffix : {".horo", ".meta"}) {
                std::filesystem::path sidecar = source;
                sidecar += suffix;
                error.clear();
                const std::filesystem::file_status status = std::filesystem::symlink_status(sidecar, error);
                if (error) {
                    if (error != std::errc::no_such_file_or_directory) {
                        return std::nullopt;
                    }
                    error.clear();
                    if (requireIdentitySidecar && std::string_view{suffix} == ".horo") {
                        return std::nullopt;
                    }
                    continue;
                }
                if (!std::filesystem::exists(status)) {
                    if (requireIdentitySidecar && std::string_view{suffix} == ".horo") {
                        return std::nullopt;
                    }
                    continue;
                }
                if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
                    return std::nullopt;
                }
                paths.push_back(std::move(sidecar));
            }
            return paths;
        }

        [[nodiscard]] std::string PortableFold(const std::string_view value) {
            std::string folded{value};
            std::ranges::transform(folded, folded.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return folded;
        }

        [[nodiscard]] bool DirectoryContainsPortableName(const std::filesystem::path &directory, const std::string_view name,
                                                         const std::filesystem::path &ignoredEntry = {}) {
            const std::string foldedName = PortableFold(name);
            const std::filesystem::path normalizedIgnored =
                ignoredEntry.empty() ? std::filesystem::path{} : ignoredEntry.lexically_normal();
            std::error_code error;
            std::filesystem::directory_iterator iterator{directory, std::filesystem::directory_options::skip_permission_denied, error};
            const std::filesystem::directory_iterator end;
            while (!error && iterator != end) {
                if (iterator->path().lexically_normal() != normalizedIgnored &&
                    PortableFold(iterator->path().filename().string()) == foldedName) {
                    return true;
                }
                iterator.increment(error);
            }
            return error || iterator != end;
        }

        [[nodiscard]] bool IsPortableEntryName(const std::string_view name) {
            if (name.empty() || name == "." || name == ".." || name.ends_with(' ') || name.ends_with('.')) {
                return false;
            }
            for (const unsigned char character : name) {
                if (character < 32U || std::string_view{R"(<>:"/\|?*)"}.find(static_cast<char>(character)) != std::string_view::npos) {
                    return false;
                }
            }

            const std::size_t dot = name.find('.');
            const std::string stem = PortableFold(name.substr(0, dot));
            if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") {
                return false;
            }
            if (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9') {
                return false;
            }
            return true;
        }

        [[nodiscard]] bool RollbackPathMoves(const std::vector<std::pair<std::filesystem::path, std::filesystem::path>> &moved) {
            bool complete = true;
            for (auto item = moved.rbegin(); item != moved.rend(); ++item) {
                std::error_code error;
                std::filesystem::rename(item->second, item->first, error);
                if (error) {
                    complete = false;
                    LOG_ERROR("editor.content_browser", "Rollback rename failed: %s -> %s (%s)", item->second.string().c_str(),
                              item->first.string().c_str(), error.message().c_str());
                }
            }
            return complete;
        }

        [[nodiscard]] bool RemoveCreatedPaths(const std::vector<std::filesystem::path> &created) {
            bool complete = true;
            for (auto item = created.rbegin(); item != created.rend(); ++item) {
                std::error_code error;
                if (!std::filesystem::remove(*item, error) || error) {
                    complete = false;
                    LOG_ERROR("editor.content_browser", "Copy rollback removal failed: %s (%s)", item->string().c_str(),
                              error.message().c_str());
                }
            }
            return complete;
        }

        [[nodiscard]] std::filesystem::path CompanionDestination(const std::filesystem::path &item, const std::filesystem::path &source,
                                                                 const std::filesystem::path &destination) {
            if (item == source)
                return destination;
            std::filesystem::path target = destination;
            target += item.extension().string();
            return target;
        }

        [[nodiscard]] bool AssetDestinationAvailable(const std::filesystem::path &source, const std::filesystem::path &destination,
                                                     const std::vector<std::filesystem::path> &companions) {
            return std::ranges::all_of(companions, [&source, &destination](const std::filesystem::path &item) {
                const std::filesystem::path target = CompanionDestination(item, source, destination);
                return !DirectoryContainsPortableName(target.parent_path(), target.filename().string());
            });
        }

        [[nodiscard]] bool PathDoesNotExist(const std::filesystem::path &path) {
            std::error_code error;
            const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
            if (error == std::errc::no_such_file_or_directory) {
                return true;
            }
            return !error && !std::filesystem::exists(status);
        }

        [[nodiscard]] std::filesystem::path ResolveDuplicateDestination(const std::filesystem::path &source,
                                                                        const std::filesystem::path &destinationDirectory,
                                                                        const std::vector<std::filesystem::path> &companions) {
            const std::string extension = source.extension().string();
            const std::string stem = source.stem().string();
            for (std::uint32_t index = 1; index < 10000; ++index) {
                const std::filesystem::path candidate = destinationDirectory / std::format("{} ({}){}", stem, index, extension);
                if (AssetDestinationAvailable(source, candidate, companions)) {
                    return candidate;
                }
            }
            return {};
        }

        [[nodiscard]] Assets::AssetId GenerateRandomAssetId(const Assets::AssetRegistrySnapshot &snapshot) {
            std::random_device random;
            for (std::uint32_t attempt = 0; attempt < 32; ++attempt) {
                std::array<std::uint8_t, 16> bytes{};
                for (std::uint8_t &byte : bytes)
                    byte = static_cast<std::uint8_t>(random());
                const auto version = static_cast<std::byte>(bytes[6]);
                const auto variant = static_cast<std::byte>(bytes[8]);
                bytes[6] = std::to_integer<std::uint8_t>((version & std::byte{0x0fU}) | std::byte{0x40U});
                bytes[8] = std::to_integer<std::uint8_t>((variant & std::byte{0x3fU}) | std::byte{0x80U});
                if (const Assets::AssetId candidate = Assets::AssetId::FromBytes(bytes);
                    candidate.IsValid() && snapshot.Find(candidate) == nullptr)
                    return candidate;
            }
            return {};
        }

        [[nodiscard]] std::optional<nlohmann::json> ReadSidecarJson(const std::filesystem::path &path) {
            std::error_code error;
            if (const std::uintmax_t size = std::filesystem::file_size(path, error); error || size == 0 || size > 1024U * 1024U)
                return std::nullopt;
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return std::nullopt;
            const nlohmann::json parsed = nlohmann::json::parse(input, nullptr, false, true);
            return parsed.is_object() ? std::optional<nlohmann::json>{parsed} : std::nullopt;
        }

        [[nodiscard]] std::vector<std::byte> JsonBytes(const nlohmann::json &value) {
            const std::string serialized = value.dump(2) + '\n';
            const auto *begin = reinterpret_cast<const std::byte *>(serialized.data());
            return {begin, begin + serialized.size()};
        }

        [[nodiscard]] std::string BehaviorNamespace(const std::filesystem::path &projectRoot) {
            std::string result = projectRoot.filename().string();
            std::ranges::transform(result, result.begin(), [](const unsigned char value) {
                return std::isalnum(value) ? static_cast<char>(std::tolower(value)) : '_';
            });
            return result.empty() ? "project" : result;
        }

        [[nodiscard]] std::string BehaviorSlug(std::string value) {
            std::ranges::transform(value, value.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return value;
        }

        [[nodiscard]] std::string NativeBehaviorContents(const std::string_view behaviorName, const std::string_view typeId) {
            return std::
                format("#include <Horo/Gameplay/NativeBehavior.h>\n\n"
                       "class {} final : public Horo::Gameplay::IBehaviorInstance {{\n"
                       "public:\n"
                       "    static Horo::Gameplay::BehaviorDescriptor DescribeBehavior() {{\n"
                       "        Horo::Gameplay::BehaviorDescriptor descriptor;\n"
                       "        descriptor.displayName = \"{}\";\n"
                       "        return descriptor;\n"
                       "    }}\n\n"
                       "    void OnFixedUpdate(Horo::Gameplay::BehaviorContext& ctx, Horo::Gameplay::FixedDeltaTime dt) override {{\n"
                       "        (void)ctx;\n"
                       "        (void)dt;\n"
                       "    }}\n"
                       "}};\n\nHORO_BEHAVIOR({}, \"{}\")\n",
                       behaviorName, behaviorName, behaviorName, typeId);
        }

        [[nodiscard]] std::string LuaBehaviorContents(const std::string_view behaviorName, const std::string_view typeId) {
            return std::format("return horo.behavior {{\n"
                               "    type_id = \"{}\",\n"
                               "    display_name = \"{}\",\n"
                               "    category = \"Gameplay\",\n"
                               "    schema_version = 1,\n"
                               "    fields = {{}},\n"
                               "    on_fixed_update = function(ctx, dt)\n"
                               "    end\n"
                               "}}\n",
                               typeId, behaviorName);
        }

        [[nodiscard]] const std::string &GameModuleContents() {
            static const std::string contents =
                "#include <Horo/Gameplay/GameModule.h>\n\n"
                "namespace {\n"
                "class ProjectGameModule final : public Horo::Gameplay::IGameModule {\n"
                "public:\n"
                "    Horo::Result<void> Register(Horo::Gameplay::GameRegistrationContext&) override {\n"
                "        return Horo::Result<void>::Success();\n"
                "    }\n"
                "    Horo::Result<void> Start(Horo::Gameplay::GameRuntimeContext&) override {\n"
                "        return Horo::Result<void>::Success();\n"
                "    }\n"
                "    void Stop(Horo::Gameplay::GameRuntimeContext&) noexcept override {}\n"
                "};\n"
                "}\n\n"
                "extern \"C\" HORO_GAME_EXPORT Horo::Gameplay::IGameModule* CreateGameModule() noexcept {\n"
                "    return new ProjectGameModule{};\n"
                "}\n\n"
                "extern \"C\" HORO_GAME_EXPORT void DestroyGameModule(Horo::Gameplay::IGameModule* module) noexcept {\n"
                "    delete module;\n"
                "}\n";
            return contents;
        }

        [[nodiscard]] Result<void> WriteBehaviorFiles(DurableFileSystem &files, const std::filesystem::path &source,
                                                      const std::string_view contents, const bool nativeBehavior,
                                                      const std::string_view typeId) {
            if (Result<void> written = files.WriteDurable(source, std::as_bytes(std::span{contents})); written.HasError())
                return written;

            if (nativeBehavior) {
                const std::filesystem::path moduleSource = source.parent_path() / "GameModule.cpp";
                if (std::error_code error; std::filesystem::exists(moduleSource, error) && !error)
                    return Result<void>::Success();
                const std::string &moduleContents = GameModuleContents();
                if (Result<void> written = files.WriteDurable(moduleSource, std::as_bytes(std::span{moduleContents})); written.HasError()) {
                    static_cast<void>(files.RemoveDurable(source));
                    return written;
                }
                return Result<void>::Success();
            }

            const std::string metadata =
                std::format("{{\n  \"schemaVersion\": 1,\n  \"runtime\": \"lua\",\n  \"behaviorTypeId\": \"{}\"\n}}\n", typeId);
            std::filesystem::path sidecar = source;
            sidecar += ".meta";
            if (Result<void> written = files.WriteDurable(sidecar, std::as_bytes(std::span{metadata})); written.HasError()) {
                static_cast<void>(files.RemoveDurable(source));
                return written;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] std::optional<std::filesystem::path> CreateUniqueTrashDirectory(const std::filesystem::path &trashRoot,
                                                                                      const std::int64_t stamp) {
            std::error_code error;
            for (std::uint32_t attempt = 0; attempt < 1000; ++attempt) {
                if (const std::filesystem::path candidate = trashRoot / std::format("asset-{}-{}", stamp, attempt);
                    std::filesystem::create_directory(candidate, error))
                    return candidate;
                if (error)
                    return std::nullopt;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::filesystem::path> SelectTrashManifestPath(const std::filesystem::path &trashDirectory,
                                                                                   const std::vector<std::filesystem::path> &sources) {
            for (std::uint32_t attempt = 0; attempt < 1000; ++attempt) {
                const std::string fileName = attempt == 0 ? "trash.json" : std::format("trash-{}.json", attempt);
                const bool collides = std::ranges::any_of(sources, [&fileName](const std::filesystem::path &item) {
                    return PortableFold(item.filename().string()) == PortableFold(fileName);
                });
                if (!collides)
                    return trashDirectory / fileName;
            }
            return std::nullopt;
        }

        [[nodiscard]] const Assets::AssetImporterContribution *ResolvePreviewContribution(
            const ContentBrowserEntry &entry, const Assets::AssetImporterCatalogSnapshot &catalog) {
            if (!entry.importerContributionId.empty()) {
                if (const auto *contribution = catalog.FindById(entry.importerContributionId); contribution != nullptr)
                    return contribution;
            }
            const auto assetType = Assets::AssetTypeId::Parse(entry.assetType);
            return assetType.HasValue() ? catalog.FindPreviewContribution(assetType.Value()) : nullptr;
        }

        [[nodiscard]] std::optional<Assets::AssetPreviewRequest> BuildPreviewRequest(const ContentBrowserEntry &entry,
                                                                                     const Assets::AssetImporterCatalogSnapshot &catalog) {
            if (entry.kind != ContentBrowserEntryKind::Asset || entry.assetType.empty())
                return std::nullopt;
            const Assets::AssetImporterContribution *contribution = ResolvePreviewContribution(entry, catalog);
            if (contribution == nullptr || contribution->previewProvider == nullptr)
                return std::nullopt;
            auto assetType = Assets::AssetTypeId::Parse(entry.assetType);
            if (assetType.HasError())
                return std::nullopt;
            return Assets::AssetPreviewRequest{
                .contributionId = contribution->contributionId,
                .moduleId = contribution->moduleId,
                .moduleVersion = contribution->moduleVersion,
                .providerVersion = contribution->version,
                .absoluteAssetPath = entry.absolutePath,
                .assetType = std::move(assetType).Value(),
                .width = 128,
                .height = 128,
                .provider = contribution->previewProvider,
            };
        }
    }  // namespace

    void EditorWorkspaceController::RebuildContentBrowserProjection(const std::filesystem::path &projectRoot,
                                                                    const std::filesystem::path &requestedDirectory) {
        m_viewModel.contentBrowser = BuildContentBrowserDirectory(projectRoot, requestedDirectory, m_assetRegistry, m_importerCatalog);
        ScheduleContentBrowserPreviews();
    }

    void EditorWorkspaceController::ScheduleContentBrowserPreviews() {
        for (PendingContentBrowserPreview &pending : m_pendingContentBrowserPreviews)
            static_cast<void>(pending.handle.RequestCancel());
        m_pendingContentBrowserPreviews.clear();
        if (m_assetPreviews == nullptr || m_importerCatalog == nullptr)
            return;

        for (const ContentBrowserEntry &entry : m_viewModel.contentBrowser.entries) {
            auto request = BuildPreviewRequest(entry, *m_importerCatalog);
            if (!request)
                continue;
            const std::string contributionId = request->contributionId;
            const std::string providerVersion = request->providerVersion;
            auto submitted = m_assetPreviews->Submit(std::move(*request));
            if (submitted.HasError())
                continue;
            m_pendingContentBrowserPreviews.push_back(PendingContentBrowserPreview{
                .absolutePath = entry.absolutePath,
                .contributionId = contributionId,
                .providerVersion = providerVersion,
                .handle = std::move(submitted).Value(),
            });
        }
    }

    void EditorWorkspaceController::PollContentBrowserPreviews() {
        std::erase_if(m_pendingContentBrowserPreviews, [this](PendingContentBrowserPreview &pending) {
            if (const Assets::AssetPreviewState state = pending.handle.State();
                state == Assets::AssetPreviewState::Queued || state == Assets::AssetPreviewState::Running)
                return false;
            if (auto completed = pending.handle.TakeResult(); completed.HasValue()) {
                const auto entry =
                    std::ranges::find(m_viewModel.contentBrowser.entries, pending.absolutePath, &ContentBrowserEntry::absolutePath);
                if (entry != m_viewModel.contentBrowser.entries.end() && entry->importerContributionId == pending.contributionId &&
                    entry->activeImporterVersion == pending.providerVersion)
                    entry->previewImage = std::move(completed).Value().image;
            }
            return true;
        });
    }

    void EditorWorkspaceController::RefreshAssets(const Assets::AssetRegistrySnapshot &assetRegistry) {
        if (assetRegistry.Revision() == m_viewModel.assetRegistryRevision)
            return;
        m_assetRegistry = assetRegistry;
        m_viewModel.assetRegistryRevision = assetRegistry.Revision();
        m_contentBrowserRefreshPending = false;
        m_contentBrowserLoadingPresented = false;
        RebuildContentBrowserProjection(m_viewModel.projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
        ReconcileContentBrowserNavigation();
    }

    void EditorWorkspaceController::UpdateContentBrowser() {
        PollContentBrowserPreviews();
        if (!m_contentBrowserRefreshPending)
            return;
        if (!m_contentBrowserLoadingPresented) {
            m_contentBrowserLoadingPresented = true;
            return;
        }

        m_contentBrowserRefreshPending = false;
        m_contentBrowserLoadingPresented = false;
        RefreshContentBrowserAfterMutation();
    }

    void EditorWorkspaceController::RefreshContentBrowserAfterMutation() {
        m_contentBrowserRefreshPending = false;
        m_contentBrowserLoadingPresented = false;
        if (m_mutableAssetRegistry != nullptr) {
            if (auto rebuilt =
                    Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, m_viewModel.projectRoot, Assets::AssetRegistryOpenMode::Edit);
                rebuilt.HasError() || rebuilt.Value().status == Assets::AssetRegistryBuildStatus::Failed) {
                m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.registry_failed";
                RebuildContentBrowserProjection(m_viewModel.projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
                ReconcileContentBrowserNavigation();
                return;
            }
            m_assetRegistry = m_mutableAssetRegistry->Snapshot();
            m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
        }
        RebuildContentBrowserProjection(m_viewModel.projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
        ReconcileContentBrowserNavigation();
    }

    void EditorWorkspaceController::RequestContentBrowserRefresh() {
        if (m_contentBrowserRefreshPending)
            return;
        m_contentBrowserRefreshPending = true;
        m_contentBrowserLoadingPresented = false;
        m_viewModel.contentBrowser.loadState = ContentBrowserLoadState::Loading;
        m_viewModel.contentBrowserOperationError.clear();
    }

    void EditorWorkspaceController::ReconcileContentBrowserNavigation() {
        const std::filesystem::path root = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteRootPath);
        const std::filesystem::path current = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteCurrentPath);
        const auto isValidHistoryEntry = [&root, &current](const std::filesystem::path &entry) {
            const std::filesystem::path normalized = NormalizeAbsolute(entry);
            return !normalized.empty() && normalized != current && IsContentBrowserDirectoryTargetAllowed(root, normalized);
        };
        std::erase_if(m_contentBrowserBackHistory, [&isValidHistoryEntry](const std::filesystem::path &entry) {
            return !isValidHistoryEntry(entry);
        });
        std::erase_if(m_contentBrowserForwardHistory, [&isValidHistoryEntry](const std::filesystem::path &entry) {
            return !isValidHistoryEntry(entry);
        });
        m_viewModel.contentBrowserCanNavigateBack = !m_contentBrowserBackHistory.empty();
        m_viewModel.contentBrowserCanNavigateForward = !m_contentBrowserForwardHistory.empty();
    }

    void EditorWorkspaceController::NavigateContentBrowser(const std::filesystem::path &absoluteDirectory, const bool recordHistory) {
        const std::filesystem::path root = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteRootPath);
        const std::filesystem::path destination = NormalizeAbsolute(absoluteDirectory);
        if (!IsContentBrowserDirectoryTargetAllowed(root, destination)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }

        const std::filesystem::path current = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteCurrentPath);
        if (destination == current)
            return;

        m_contentBrowserRefreshPending = false;
        m_contentBrowserLoadingPresented = false;
        if (recordHistory && !current.empty()) {
            m_contentBrowserBackHistory.push_back(current);
            m_contentBrowserForwardHistory.clear();
        }
        RebuildContentBrowserProjection(m_viewModel.projectRoot, destination);
        m_viewModel.contentBrowserOperationError.clear();
        m_viewModel.contentBrowserCanNavigateBack = !m_contentBrowserBackHistory.empty();
        m_viewModel.contentBrowserCanNavigateForward = !m_contentBrowserForwardHistory.empty();
    }

    void EditorWorkspaceController::NavigateContentBrowserBack() {
        if (m_contentBrowserBackHistory.empty())
            return;
        const std::filesystem::path current = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteCurrentPath);
        while (!m_contentBrowserBackHistory.empty()) {
            const std::filesystem::path destination = m_contentBrowserBackHistory.back();
            m_contentBrowserBackHistory.pop_back();
            if (!IsContentBrowserDirectoryTargetAllowed(m_viewModel.contentBrowser.absoluteRootPath, destination)) {
                continue;
            }
            if (!current.empty())
                m_contentBrowserForwardHistory.push_back(current);
            NavigateContentBrowser(destination, false);
            break;
        }
        m_viewModel.contentBrowserCanNavigateBack = !m_contentBrowserBackHistory.empty();
        m_viewModel.contentBrowserCanNavigateForward = !m_contentBrowserForwardHistory.empty();
    }

    void EditorWorkspaceController::NavigateContentBrowserForward() {
        if (m_contentBrowserForwardHistory.empty())
            return;
        const std::filesystem::path current = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteCurrentPath);
        while (!m_contentBrowserForwardHistory.empty()) {
            const std::filesystem::path destination = m_contentBrowserForwardHistory.back();
            m_contentBrowserForwardHistory.pop_back();
            if (!IsContentBrowserDirectoryTargetAllowed(m_viewModel.contentBrowser.absoluteRootPath, destination)) {
                continue;
            }
            if (!current.empty())
                m_contentBrowserBackHistory.push_back(current);
            NavigateContentBrowser(destination, false);
            break;
        }
        m_viewModel.contentBrowserCanNavigateBack = !m_contentBrowserBackHistory.empty();
        m_viewModel.contentBrowserCanNavigateForward = !m_contentBrowserForwardHistory.empty();
    }

    void EditorWorkspaceController::NavigateContentBrowserUp() {
        const std::filesystem::path root = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteRootPath);
        const std::filesystem::path current = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteCurrentPath);
        if (!root.empty() && current != root)
            NavigateContentBrowser(current.parent_path(), true);
    }

    void EditorWorkspaceController::DuplicateContentBrowserAsset(const std::filesystem::path &absolutePath) {
        m_viewModel.contentBrowserOperationError.clear();
        static_cast<void>(CopyContentBrowserAssetTo(NormalizeAbsolute(absolutePath), NormalizeAbsolute(absolutePath).parent_path()));
    }

    void EditorWorkspaceController::SetContentBrowserClipboard(const std::filesystem::path &absolutePath,
                                                               const ContentBrowserClipboardMode mode) {
        m_viewModel.contentBrowserOperationError.clear();
        const std::filesystem::path source = NormalizeAbsolute(absolutePath);
        std::error_code error;
        if (const auto status = std::filesystem::symlink_status(source, error);
            !IsDirectContentBrowserEntry(m_viewModel.contentBrowser, source) || error || !std::filesystem::is_regular_file(status)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }
        m_viewModel.contentBrowserClipboard = {
            .mode = mode,
            .absoluteSourcePath = source.string(),
        };
    }

    void EditorWorkspaceController::PasteContentBrowserAsset(const std::filesystem::path &absoluteDirectory) {
        using enum ContentBrowserClipboardMode;
        m_viewModel.contentBrowserOperationError.clear();
        const ContentBrowserClipboardState clipboard = m_viewModel.contentBrowserClipboard;
        if (clipboard.mode == None || clipboard.absoluteSourcePath.empty()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.clipboard_empty";
            return;
        }

        const std::filesystem::path source = NormalizeAbsolute(clipboard.absoluteSourcePath);
        const std::filesystem::path destination = NormalizeAbsolute(absoluteDirectory);
        bool succeeded{};
        if (clipboard.mode == Copy)
            succeeded = CopyContentBrowserAssetTo(source, destination);
        else
            succeeded = MoveContentBrowserAssetTo(source, destination);
        if (succeeded && clipboard.mode == Move)
            ClearContentBrowserClipboard();
    }

    void EditorWorkspaceController::TransferContentBrowserAsset(const ContentBrowserAssetTransferRequest &request) {
        m_viewModel.contentBrowserOperationError.clear();
        if (!std::filesystem::path{request.absoluteSourcePath}.is_absolute() ||
            !std::filesystem::path{request.absoluteDestinationDirectory}.is_absolute()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }
        const std::filesystem::path source = NormalizeAbsolute(request.absoluteSourcePath);
        const std::filesystem::path destination = NormalizeAbsolute(request.absoluteDestinationDirectory);
        if (request.mode == ContentBrowserTransferMode::Copy)
            static_cast<void>(CopyContentBrowserAssetTo(source, destination));
        else
            static_cast<void>(MoveContentBrowserAssetTo(source, destination));
    }

    void EditorWorkspaceController::CreateContentBrowserFolder(const std::filesystem::path &absoluteDirectory,
                                                               const std::string_view name) {
        LOG_INFO("editor.asset_actions", "Create folder requested: directory='%s' name='%s'", absoluteDirectory.string().c_str(),
                 std::string{name}.c_str());
        m_viewModel.contentBrowserOperationError.clear();
        if (m_mutations == nullptr || m_durableFiles == nullptr) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.unavailable";
            return;
        }
        const std::filesystem::path directory = NormalizeAbsolute(absoluteDirectory);
        const std::filesystem::path requestedName{name};
        if (!IsContentBrowserDirectoryTargetAllowed(m_viewModel.contentBrowser.absoluteRootPath, directory) ||
            requestedName != requestedName.filename() || !IsPortableEntryName(name)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_name";
            return;
        }
        if (DirectoryContainsPortableName(directory, name)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.name_exists";
            return;
        }

        if (auto lease = m_mutations->TryAcquire(ProjectMutationRequest{
                .projectRoot = m_viewModel.projectRoot,
                .owner = ProjectMutationOwner::Asset,
                .operationId = "content-browser-create-folder",
            });
            lease.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.busy";
            return;
        } else {
            if (std::error_code error; !std::filesystem::create_directory(directory / requestedName, error) || error) {
                LOG_ERROR("editor.asset_actions", "Create folder failed: path='%s' error='%s'",
                          (directory / requestedName).string().c_str(), error.message().c_str());
                m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.create_folder_failed";
                return;
            }
            static_cast<void>(m_durableFiles->SyncDirectory(directory));
            LOG_INFO("editor.asset_actions", "Create folder completed: path='%s'", (directory / requestedName).string().c_str());
            RefreshContentBrowserAfterMutation();
        }
    }

    void EditorWorkspaceController::CreateGameplayBehavior(const CreateGameplayBehaviorRequest &request) {
        m_viewModel.contentBrowserOperationError.clear();
        if (const Result<void> validation = ValidateCreateGameplayBehaviorRequest(request); validation.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_name";
            return;
        }
        const bool nativeBehavior = request.kind == GameplayBehaviorKind::Native;
        LOG_INFO("editor.asset_actions", "Create %s behavior requested: directory='%s' base='%s'", nativeBehavior ? "native" : "lua",
                 request.destination.c_str(), request.baseName.c_str());
        if (m_mutations == nullptr || m_durableFiles == nullptr) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.unavailable";
            return;
        }

        const std::filesystem::path projectRoot = NormalizeAbsolute(m_viewModel.projectRoot);
        const std::filesystem::path assetsRoot = projectRoot / "assets";
        const std::filesystem::path scriptsRoot = assetsRoot / "scripts";
        const std::filesystem::path requestedDirectory = NormalizeAbsolute(request.destination);
        std::filesystem::path directory;
        if (nativeBehavior)
            directory = projectRoot / "source" / "gameplay";
        else
            directory = HasPathPrefix(scriptsRoot, requestedDirectory) ? requestedDirectory : scriptsRoot;
        if ((!nativeBehavior && !HasPathPrefix(assetsRoot, directory)) || (nativeBehavior && !HasPathPrefix(projectRoot, directory))) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }

        CreateGameplayBehaviorFiles(request, nativeBehavior, projectRoot, directory);
    }

    void EditorWorkspaceController::CreateGameplayBehaviorFiles(const CreateGameplayBehaviorRequest &request, const bool nativeBehavior,
                                                                const std::filesystem::path &projectRoot,
                                                                const std::filesystem::path &directory) {
        if (auto lease = m_mutations->TryAcquire(ProjectMutationRequest{
                .projectRoot = projectRoot,
                .owner = ProjectMutationOwner::Asset,
                .operationId = nativeBehavior ? "create-native-behavior" : "create-lua-behavior",
            });
            lease.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.busy";
            return;
        } else {
            WriteGameplayBehaviorSource(request, nativeBehavior, projectRoot, directory);
        }
    }

    void EditorWorkspaceController::WriteGameplayBehaviorSource(const CreateGameplayBehaviorRequest &request, const bool nativeBehavior,
                                                                const std::filesystem::path &projectRoot,
                                                                const std::filesystem::path &directory) {
        std::error_code filesystemError;
        std::filesystem::create_directories(directory, filesystemError);
        if (filesystemError) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.create_behavior_failed";
            return;
        }

        const std::filesystem::path source = directory / (request.baseName + (nativeBehavior ? ".cpp" : ".horo_script"));
        if (std::filesystem::exists(source, filesystemError) || filesystemError) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.name_exists";
            return;
        }

        const std::string behaviorName = source.stem().string();
        const std::string typeId =
            std::format("game.{}.{}.{}", BehaviorNamespace(projectRoot), nativeBehavior ? "cpp" : "lua", BehaviorSlug(behaviorName));
        const std::string contents =
            nativeBehavior ? NativeBehaviorContents(behaviorName, typeId) : LuaBehaviorContents(behaviorName, typeId);
        if (const Result<void> written = WriteBehaviorFiles(*m_durableFiles, source, contents, nativeBehavior, typeId);
            written.HasError()) {
            LOG_ERROR("editor.asset_actions", "Create %s behavior files failed for '%s': %s", nativeBehavior ? "native" : "lua",
                      source.string().c_str(), written.ErrorValue().message.c_str());
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.create_behavior_failed";
            return;
        }
        if (nativeBehavior) {
            ProjectIntegrityValidatorService validator{*m_durableFiles};
            static_cast<void>(validator.Repair(projectRoot));
        }

        static_cast<void>(m_durableFiles->SyncDirectory(directory));
        RefreshContentBrowserAfterMutation();
        if (nativeBehavior)
            m_nativeBuildDebounceSeconds = 0.25F;
        else
            RefreshGameplayRegistry();
        LOG_INFO("editor.asset_actions", "Create %s behavior completed: source='%s'", nativeBehavior ? "native" : "lua",
                 source.string().c_str());
    }

    void EditorWorkspaceController::RefreshGameplayRegistry() {
        if (m_nativeGameplayReloadQuarantined)
            return;
        if (m_playSession.IsActive() && m_gameplayRegistry && m_gameplayRegistry->HasNativeModule()) {
            m_nativeGameplayReloadPending = true;
            return;
        }
        std::unique_ptr<ProjectGameplayRegistry> candidate = ProjectGameplayRegistry::Discover(m_viewModel.projectRoot);
        if (m_playSession.IsActive()) {
            m_pendingGameplayRegistry = std::move(candidate);
            return;
        }
        m_gameplayRegistry = std::move(candidate);
        RefreshAvailableBehaviorProjection();
    }

    void EditorWorkspaceController::RefreshAvailableBehaviorProjection() {
        m_viewModel.availableBehaviors.clear();
        for (const Gameplay::BehaviorRegistration &registration : m_gameplayRegistry->Registry().Registrations())
            m_viewModel.availableBehaviors.push_back(registration.descriptor);
        for (const ProjectGameplayDiagnostic &diagnostic : m_gameplayRegistry->Diagnostics())
            LOG_ERROR("editor.gameplay", "Gameplay source '%s' is invalid: %s", diagnostic.source.string().c_str(),
                      diagnostic.error.message.c_str());
    }

    void EditorWorkspaceController::ApplyPendingGameplayRegistry() {
        if (!m_pendingGameplayRegistry)
            return;
        if (m_pendingGameplayRegistry->HasBlockingDiagnostics()) {
            for (const ProjectGameplayDiagnostic &diagnostic : m_pendingGameplayRegistry->Diagnostics())
                LOG_ERROR("editor.gameplay", "Gameplay registry candidate was rejected for '%s': %s", diagnostic.source.string().c_str(),
                          diagnostic.error.message.c_str());
            m_pendingGameplayRegistry.reset();
            return;
        }
        if (const Result<void> reloaded =
                m_playSession.ReloadBehaviors(m_pendingGameplayRegistry->Registry(), m_gameplayRegistry->Registry());
            reloaded.HasError()) {
            LOG_ERROR("editor.gameplay", "Gameplay reload rolled back to the previous module: %s", reloaded.ErrorValue().message.c_str());
            m_pendingGameplayRegistry.reset();
            RefreshPlayStateProjection();
            return;
        }
        m_gameplayRegistry = std::move(m_pendingGameplayRegistry);
        RefreshAvailableBehaviorProjection();
        LOG_INFO("editor.gameplay", "Gameplay module reloaded at a fixed-tick safe point.");
    }

    void EditorWorkspaceController::ApplyNativeGameplayReload() {
        if (m_nativeGameplayReloadQuarantined || !m_nativeGameplayReloadPending)
            return;
        m_nativeGameplayReloadPending = false;
        m_pendingGameplayRegistry.reset();
        if (!m_gameplayRegistry || !m_gameplayRegistry->HasNativeModule() || !m_playSession.IsActive())
            return;

        const std::filesystem::path projectRoot{m_viewModel.projectRoot};
        auto begun = BeginNativeGameplayReload(projectRoot);
        if (begun.HasError()) {
            LogNativeReloadFailure("could not begin safely", begun.ErrorValue());
            return;
        }
        NativeGameplayReloadTransaction transaction = std::move(begun).Value();
        if (Result<void> retired = RetireNativeGameplayGeneration(transaction); retired.HasError()) {
            Error error = retired.ErrorValue();
            DegradeNativeGameplayReload(transaction, error);
            LogNativeReloadFailure("could not retire safely", error);
            return;
        }
        m_gameplayRegistry.reset();
        auto candidate = TryActivateNativeGameplayGeneration(projectRoot, transaction);
        if (candidate.HasValue()) {
            CommitNativeGameplayReload(std::move(candidate).Value(), transaction);
            return;
        }
        RollbackNativeGameplayReload(projectRoot, std::move(transaction), candidate.ErrorValue());
    }

    Result<EditorWorkspaceController::NativeGameplayReloadTransaction> EditorWorkspaceController::BeginNativeGameplayReload(
        const std::filesystem::path &projectRoot) const {
        auto preserved =
            m_gameplayRegistry->PreserveNativeArtifactForRollback(projectRoot / ".horo" / "local" / "gameplay_module_rollback");
        if (preserved.HasError())
            return Result<NativeGameplayReloadTransaction>::Failure(preserved.ErrorValue());
        auto luaGeneration = m_gameplayRegistry->CaptureLuaGeneration();
        if (luaGeneration.HasError())
            return Result<NativeGameplayReloadTransaction>::Failure(luaGeneration.ErrorValue());
        NativeGameplayReloadTransaction transaction;
        transaction.rollbackArtifact = std::move(preserved).Value();
        transaction.luaGeneration = std::move(luaGeneration).Value();
        return Result<NativeGameplayReloadTransaction>::Success(std::move(transaction));
    }

    Result<void> EditorWorkspaceController::RetireNativeGameplayGeneration(NativeGameplayReloadTransaction &transaction) {
        auto playSnapshot = m_playSession.QuiesceForReload();
        if (playSnapshot.HasError())
            return Result<void>::Failure(playSnapshot.ErrorValue());
        transaction.playSnapshot = std::move(playSnapshot).Value();
        auto moduleSnapshot = m_gameplayRegistry->PrepareNativeReload();
        if (moduleSnapshot.HasError())
            return Result<void>::Failure(moduleSnapshot.ErrorValue());
        transaction.moduleSnapshot = std::move(moduleSnapshot).Value();
        transaction.phase = NativeGameplayReloadTransaction::Phase::Retired;
        return Result<void>::Success();
    }

    Result<std::unique_ptr<ProjectGameplayRegistry>> EditorWorkspaceController::TryActivateNativeGameplayGeneration(
        const std::filesystem::path &projectRoot, NativeGameplayReloadTransaction &transaction) {
        if (transaction.phase != NativeGameplayReloadTransaction::Phase::Retired)
            return Result<std::unique_ptr<ProjectGameplayRegistry>>::Failure(
                MakeError(Gameplay::GameplayErrors::GameplayReloadRestoreFailed, "Native reload generation was not retired."));
        std::unique_ptr<ProjectGameplayRegistry> candidate =
            ProjectGameplayRegistry::DiscoverNativeGeneration(projectRoot, transaction.luaGeneration);
        if (!candidate->HasNativeModule() || candidate->HasBlockingDiagnostics())
            return Result<std::unique_ptr<ProjectGameplayRegistry>>::Failure(
                NativeGenerationError(*candidate, "The replacement native gameplay generation could not be loaded."));
        if (Result<void> restored = candidate->RestoreNativeReload(transaction.moduleSnapshot); restored.HasError())
            return Result<std::unique_ptr<ProjectGameplayRegistry>>::Failure(restored.ErrorValue());
        if (Result<void> restored = m_playSession.RestoreAfterReload(candidate->Registry(), transaction.playSnapshot); restored.HasError())
            return Result<std::unique_ptr<ProjectGameplayRegistry>>::Failure(restored.ErrorValue());
        transaction.phase = NativeGameplayReloadTransaction::Phase::Activated;
        return Result<std::unique_ptr<ProjectGameplayRegistry>>::Success(std::move(candidate));
    }

    void EditorWorkspaceController::CommitNativeGameplayReload(std::unique_ptr<ProjectGameplayRegistry> generation,
                                                               NativeGameplayReloadTransaction &transaction) {
        m_gameplayRegistry = std::move(generation);
        transaction.phase = NativeGameplayReloadTransaction::Phase::Committed;
        RefreshAvailableBehaviorProjection();
        LOG_INFO("editor.gameplay", "Native gameplay generation reloaded transactionally at a fixed-tick safe point.");
    }

    void EditorWorkspaceController::RollbackNativeGameplayReload(const std::filesystem::path &projectRoot,
                                                                 NativeGameplayReloadTransaction transaction, const Error &candidateError) {
        std::unique_ptr<ProjectGameplayRegistry> rollback =
            ProjectGameplayRegistry::DiscoverRollback(projectRoot, transaction.rollbackArtifact, transaction.luaGeneration);
        Error rollbackError = NativeGenerationError(*rollback, "The previous native gameplay generation could not be loaded.");
        const bool ready = rollback->HasNativeModule() && !rollback->HasBlockingDiagnostics();
        Result<void> activated =
            ready ? rollback->RestoreNativeReload(transaction.moduleSnapshot) : Result<void>::Failure(std::move(rollbackError));
        if (activated.HasValue())
            activated = m_playSession.RestoreAfterReload(rollback->Registry(), transaction.playSnapshot);
        if (activated.HasValue()) {
            m_gameplayRegistry = std::move(rollback);
            transaction.phase = NativeGameplayReloadTransaction::Phase::RolledBack;
            RefreshAvailableBehaviorProjection();
            LogNativeReloadFailure("rejected the candidate and restored the previous generation", candidateError);
            return;
        }
        rollback.reset();
        rollbackError = activated.ErrorValue();
        DegradeNativeGameplayReload(transaction, rollbackError);
        LogNativeReloadFailure("could not restore the candidate", candidateError);
        LogNativeReloadFailure("could not restore the previous generation; Play Mode was stopped", rollbackError);
    }

    void EditorWorkspaceController::DegradeNativeGameplayReload(NativeGameplayReloadTransaction &transaction, Error error) {
        transaction.phase = NativeGameplayReloadTransaction::Phase::Degraded;
        m_nativeGameplayReloadQuarantined = true;
        m_nativeGameplayReloadPending = false;
        m_pendingGameplayRegistry.reset();
        if (m_playSession.State() == EditorPlaySessionState::Reloading)
            m_playSession.DegradeAfterReload(std::move(error));
    }

    bool EditorWorkspaceController::CopyContentBrowserAssetTo(const std::filesystem::path &absoluteSource,
                                                              const std::filesystem::path &absoluteDestinationDirectory) {
        if (m_mutations == nullptr || m_durableFiles == nullptr || m_mutableAssetRegistry == nullptr) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.unavailable";
            return false;
        }

        const std::filesystem::path root = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteRootPath);
        const std::filesystem::path source = NormalizeAbsolute(absoluteSource);
        const std::filesystem::path destinationDirectory = NormalizeAbsolute(absoluteDestinationDirectory);
        std::error_code error;
        if (const auto sourceStatus = std::filesystem::symlink_status(source, error);
            error || std::filesystem::is_symlink(sourceStatus) || !std::filesystem::is_regular_file(sourceStatus) ||
            !HasPathPrefix(root, source) || !IsContentBrowserDirectoryTargetAllowed(root, destinationDirectory)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return false;
        }

        const std::filesystem::path projectRoot = NormalizeAbsolute(m_viewModel.projectRoot);
        const std::string projectPath = source.lexically_relative(projectRoot).generic_string();
        const Assets::AssetRecord *sourceRecord = m_assetRegistry.FindByPath(projectPath);
        std::filesystem::path sourceSidecar = source;
        sourceSidecar += ".horo";
        const auto companions = ValidatedAssetCompanions(source, true);
        if (sourceRecord == nullptr || !companions.has_value()) {
            m_viewModel.contentBrowserOperationError = sourceRecord == nullptr ? "workspace.content_browser.operation.asset_required"
                                                                               : "workspace.content_browser.operation.companion_invalid";
            return false;
        }

        std::filesystem::path destination = destinationDirectory / source.filename();
        if (destinationDirectory == source.parent_path() || !AssetDestinationAvailable(source, destination, *companions)) {
            destination = ResolveDuplicateDestination(source, destinationDirectory, *companions);
        }
        if (destination.empty()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.name_exists";
            return false;
        }

        auto sidecar = ReadSidecarJson(sourceSidecar);
        const Assets::AssetId newId = GenerateRandomAssetId(m_assetRegistry);
        if (!sidecar.has_value() || !newId.IsValid()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.copy_failed";
            return false;
        }
        (*sidecar)["assetId"] = newId.ToString();

        if (auto lease = m_mutations->TryAcquire(ProjectMutationRequest{
                .projectRoot = m_viewModel.projectRoot,
                .owner = ProjectMutationOwner::Asset,
                .operationId = "content-browser-copy",
            });
            lease.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.busy";
            return false;
        } else {
            const std::vector<std::byte> sidecarBytes = JsonBytes(*sidecar);
            const std::optional<std::vector<std::filesystem::path>> created =
                CopyContentBrowserCompanions(source, sourceSidecar, destination, *companions, sidecarBytes);
            return created.has_value() && PublishCopiedContentBrowserAsset(projectRoot, destination, newId, *created);
        }
    }

    std::optional<std::vector<std::filesystem::path>> EditorWorkspaceController::CopyContentBrowserCompanions(
        const std::filesystem::path &source, const std::filesystem::path &sourceSidecar, const std::filesystem::path &destination,
        const std::vector<std::filesystem::path> &companions, const std::span<const std::byte> sidecarBytes) {
        std::vector<std::filesystem::path> created;
        for (const std::filesystem::path &item : companions) {
            const std::filesystem::path target = CompanionDestination(item, source, destination);
            if (!PathDoesNotExist(target)) {
                const bool rollbackComplete = RemoveCreatedPaths(created);
                SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.name_exists");
                return std::nullopt;
            }
            if (const Result<void> copied =
                    item == sourceSidecar ? m_durableFiles->WriteDurable(target, sidecarBytes) : m_durableFiles->CopyDurable(item, target);
                copied.HasError()) {
                std::error_code cleanupError;
                std::filesystem::remove(target, cleanupError);
                const bool rollbackComplete = RemoveCreatedPaths(created);
                SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.copy_failed");
                return std::nullopt;
            }
            created.emplace_back(target);
        }
        if (m_durableFiles->SyncDirectory(destination.parent_path()).HasError()) {
            const bool rollbackComplete = RemoveCreatedPaths(created);
            SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.copy_failed");
            return std::nullopt;
        }
        return created;
    }

    bool EditorWorkspaceController::PublishCopiedContentBrowserAsset(const std::filesystem::path &projectRoot,
                                                                     const std::filesystem::path &destination,
                                                                     const Assets::AssetId copiedId,
                                                                     const std::vector<std::filesystem::path> &created) {
        const auto rollback = [this, &created, &projectRoot](const std::string_view failureKey) {
            const bool rollbackComplete = RemoveCreatedPaths(created);
            static_cast<void>(Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, projectRoot, Assets::AssetRegistryOpenMode::Edit));
            SetContentBrowserRollbackError(rollbackComplete, failureKey);
            return false;
        };
        if (auto rebuilt = Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, projectRoot, Assets::AssetRegistryOpenMode::Edit);
            rebuilt.HasError() || rebuilt.Value().status != Assets::AssetRegistryBuildStatus::Complete) {
            return rollback("workspace.content_browser.operation.registry_failed");
        }
        const Assets::AssetRegistrySnapshot rebuiltSnapshot = m_mutableAssetRegistry->Snapshot();
        const std::string destinationProjectPath = destination.lexically_relative(projectRoot).generic_string();
        if (const Assets::AssetRecord *copiedRecord = rebuiltSnapshot.Find(copiedId);
            copiedRecord == nullptr || copiedRecord->sourcePath.String() != destinationProjectPath) {
            return rollback("workspace.content_browser.operation.registry_failed");
        }
        m_assetRegistry = rebuiltSnapshot;
        m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
        RebuildContentBrowserProjection(projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
        return true;
    }

    bool EditorWorkspaceController::MoveContentBrowserAssetTo(const std::filesystem::path &absoluteSource,
                                                              const std::filesystem::path &absoluteDestinationDirectory) {
        if (m_mutations == nullptr || m_durableFiles == nullptr || m_mutableAssetRegistry == nullptr) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.unavailable";
            return false;
        }
        const std::filesystem::path root = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteRootPath);
        const std::filesystem::path source = NormalizeAbsolute(absoluteSource);
        const std::filesystem::path destinationDirectory = NormalizeAbsolute(absoluteDestinationDirectory);
        std::error_code error;
        if (const auto sourceStatus = std::filesystem::symlink_status(source, error);
            error || std::filesystem::is_symlink(sourceStatus) || !std::filesystem::is_regular_file(sourceStatus) ||
            !HasPathPrefix(root, source) || !IsContentBrowserDirectoryTargetAllowed(root, destinationDirectory)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return false;
        }
        if (source.parent_path() == destinationDirectory) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.same_folder";
            return false;
        }
        const std::filesystem::path projectRoot = NormalizeAbsolute(m_viewModel.projectRoot);
        const std::string oldProjectPath = source.lexically_relative(projectRoot).generic_string();
        const Assets::AssetRecord *record = m_assetRegistry.FindByPath(oldProjectPath);
        if (record == nullptr) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.asset_required";
            return false;
        }
        const auto companions = ValidatedAssetCompanions(source, true);
        if (!companions.has_value()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.companion_invalid";
            return false;
        }
        const Assets::AssetId originalId = record->id;
        const std::filesystem::path destination = destinationDirectory / source.filename();
        if (!AssetDestinationAvailable(source, destination, *companions)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.name_exists";
            return false;
        }

        if (auto lease = m_mutations->TryAcquire(ProjectMutationRequest{
                .projectRoot = m_viewModel.projectRoot,
                .owner = ProjectMutationOwner::Asset,
                .operationId = "content-browser-move",
            });
            lease.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.busy";
            return false;
        } else {
            const std::optional<ContentBrowserPathMoves> moved =
                MoveContentBrowserCompanions(source, destination, *companions, "workspace.content_browser.operation.move_failed");
            if (!moved.has_value())
                return false;
            return PublishMovedContentBrowserAsset(projectRoot, destination, originalId, *moved);
        }
    }

    std::optional<EditorWorkspaceController::ContentBrowserPathMoves> EditorWorkspaceController::MoveContentBrowserCompanions(
        const std::filesystem::path &source, const std::filesystem::path &destination, const std::vector<std::filesystem::path> &companions,
        const std::string_view failureKey) {
        std::error_code error;
        ContentBrowserPathMoves moved;
        moved.reserve(companions.size());
        for (const std::filesystem::path &item : companions) {
            const std::filesystem::path target = CompanionDestination(item, source, destination);
            if (!PathDoesNotExist(target)) {
                const bool rollbackComplete = RollbackPathMoves(moved);
                SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.name_exists");
                return std::nullopt;
            }
            std::filesystem::rename(item, target, error);
            if (error) {
                const bool rollbackComplete = RollbackPathMoves(moved);
                SetContentBrowserRollbackError(rollbackComplete, failureKey);
                return std::nullopt;
            }
            moved.emplace_back(item, target);
        }
        const bool sourceSynced = m_durableFiles->SyncDirectory(source.parent_path()).HasValue();
        if (const bool destinationSynced =
                source.parent_path() == destination.parent_path() || m_durableFiles->SyncDirectory(destination.parent_path()).HasValue();
            !sourceSynced || !destinationSynced) {
            const bool rollbackComplete = RollbackPathMoves(moved);
            SetContentBrowserRollbackError(rollbackComplete, failureKey);
            return std::nullopt;
        }
        return moved;
    }

    bool EditorWorkspaceController::PublishMovedContentBrowserAsset(const std::filesystem::path &projectRoot,
                                                                    const std::filesystem::path &destination,
                                                                    const Assets::AssetId originalId,
                                                                    const ContentBrowserPathMoves &moved) {
        const auto rollback = [this, &moved, &projectRoot](const std::string_view failureKey) {
            const bool rollbackComplete = RollbackPathMoves(moved);
            static_cast<void>(Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, projectRoot, Assets::AssetRegistryOpenMode::Edit));
            SetContentBrowserRollbackError(rollbackComplete, failureKey);
            return false;
        };
        if (auto rebuilt = Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, projectRoot, Assets::AssetRegistryOpenMode::Edit);
            rebuilt.HasError() || rebuilt.Value().status != Assets::AssetRegistryBuildStatus::Complete) {
            return rollback("workspace.content_browser.operation.registry_failed");
        }
        const Assets::AssetRegistrySnapshot rebuiltSnapshot = m_mutableAssetRegistry->Snapshot();
        const std::string newProjectPath = destination.lexically_relative(projectRoot).generic_string();
        if (const Assets::AssetRecord *movedRecord = rebuiltSnapshot.Find(originalId);
            movedRecord == nullptr || movedRecord->sourcePath.String() != newProjectPath) {
            return rollback("workspace.content_browser.operation.registry_failed");
        }
        m_assetRegistry = rebuiltSnapshot;
        m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
        RebuildContentBrowserProjection(projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
        return true;
    }

    void EditorWorkspaceController::ClearContentBrowserClipboard() noexcept {
        m_viewModel.contentBrowserClipboard = {};
    }

    void EditorWorkspaceController::SetContentBrowserRollbackError(const bool rollbackComplete, const std::string_view failureKey) {
        m_viewModel.contentBrowserOperationError = rollbackComplete ? failureKey : "workspace.content_browser.operation.rollback_failed";
    }

    void EditorWorkspaceController::ReimportContentBrowserAsset(const std::filesystem::path &absolutePath) {
        m_viewModel.contentBrowserOperationError.clear();
        if (m_mutations == nullptr || m_durableFiles == nullptr || m_mutableAssetRegistry == nullptr || m_importerCatalog == nullptr) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.unavailable";
            return;
        }
        const std::filesystem::path target = NormalizeAbsolute(absolutePath);
        if (!IsDirectContentBrowserEntry(m_viewModel.contentBrowser, target)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }

        if (auto lease = m_mutations->TryAcquire(ProjectMutationRequest{
                .projectRoot = m_viewModel.projectRoot,
                .owner = ProjectMutationOwner::Asset,
                .operationId = "content-browser-reimport",
            });
            lease.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.busy";
            return;
        } else {
            if (auto reimported = Assets::ReimportProjectAsset(
                    Assets::AssetReimportRequest{
                        .absoluteProjectRoot = NormalizeAbsolute(m_viewModel.projectRoot),
                        .absoluteAssetPath = target,
                        .importerCatalog = m_importerCatalog,
                        .registry = m_mutableAssetRegistry,
                        .files = m_durableFiles,
                    },
                    CancellationToken{});
                reimported.HasError()) {
                LOG_ERROR("editor.content_browser", "Reimport failed for %s: %s", target.string().c_str(),
                          reimported.ErrorValue().message.c_str());
                if (reimported.ErrorValue().code.Value() == "asset.import.no_importer")
                    m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.reimport_importer_missing";
                else if (reimported.ErrorValue().code.Value() == "asset.registry.source_missing")
                    m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.reimport_unavailable";
                else
                    m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.reimport_failed";
                return;
            }
            m_assetRegistry = m_mutableAssetRegistry->Snapshot();
            m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
            RebuildContentBrowserProjection(m_viewModel.projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
        }
    }

    void EditorWorkspaceController::RevealContentBrowserEntry(const std::filesystem::path &absolutePath) {
        m_viewModel.contentBrowserOperationError.clear();
        const std::filesystem::path target = NormalizeAbsolute(absolutePath);
        const std::filesystem::path root = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteRootPath);
        std::error_code error;
        if (const auto status = std::filesystem::symlink_status(target, error);
            error || std::filesystem::is_symlink(status) ||
            (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status)) || !HasPathPrefix(root, target)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }
        if (!RevealInNativeFileManager(target)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.reveal_unavailable";
        }
    }

    void EditorWorkspaceController::OpenDiagnosticSource(const DiagnosticSourceRequest &source) {
        m_viewModel.contentBrowserOperationError.clear();
        if (!std::filesystem::path{source.absolutePath}.is_absolute()) {
            m_viewModel.contentBrowserOperationError = "workspace.global_dock.build_output.source.invalid";
            return;
        }

        const std::filesystem::path target = NormalizeAbsolute(source.absolutePath);
        const std::filesystem::path projectRoot = NormalizeAbsolute(m_viewModel.projectRoot);
        std::error_code error;
        if (const auto status = std::filesystem::symlink_status(target, error); error || std::filesystem::is_symlink(status) ||
                                                                                !std::filesystem::is_regular_file(status) ||
                                                                                !HasPathPrefix(projectRoot, target)) {
            m_viewModel.contentBrowserOperationError = "workspace.global_dock.build_output.source.invalid";
            return;
        }
        DiagnosticSourceRequest validatedSource = source;
        validatedSource.absolutePath = target.string();
        if (!m_diagnosticSourceNavigator(validatedSource))
            m_viewModel.contentBrowserOperationError = "workspace.global_dock.build_output.source.unavailable";
    }

    void EditorWorkspaceController::RenameContentBrowserEntry(const std::filesystem::path &absolutePath, const std::string_view newName) {
        m_viewModel.contentBrowserOperationError.clear();
        if (m_mutations == nullptr || m_durableFiles == nullptr || m_mutableAssetRegistry == nullptr) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.unavailable";
            return;
        }

        const std::optional<ContentBrowserRenamePlan> plan = PrepareContentBrowserRename(absolutePath, newName);
        if (!plan.has_value())
            return;

        if (auto lease = m_mutations->TryAcquire(ProjectMutationRequest{
                .projectRoot = m_viewModel.projectRoot,
                .owner = ProjectMutationOwner::Asset,
                .operationId = "content-browser-rename",
            });
            lease.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.busy";
            return;
        } else {
            const std::optional<ContentBrowserPathMoves> moved =
                MoveContentBrowserCompanions(plan->source, plan->destination, plan->sources,
                                             "workspace.content_browser.operation.rename_failed");
            if (moved.has_value())
                static_cast<void>(PublishRenamedContentBrowserEntries(*moved));
        }
    }

    std::optional<EditorWorkspaceController::ContentBrowserRenamePlan> EditorWorkspaceController::PrepareContentBrowserRename(
        const std::filesystem::path &absolutePath, const std::string_view newName) {
        if (!absolutePath.is_absolute()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return std::nullopt;
        }
        ContentBrowserRenamePlan plan{.source = NormalizeAbsolute(absolutePath)};
        std::filesystem::path requestedName{newName};
        if (!IsDirectContentBrowserEntry(m_viewModel.contentBrowser, plan.source) || requestedName != requestedName.filename() ||
            !IsPortableEntryName(newName)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_name";
            return std::nullopt;
        }

        std::error_code error;
        const bool regularFile = std::filesystem::is_regular_file(plan.source, error);
        if (regularFile && requestedName.extension().empty())
            requestedName += plan.source.extension().string();
        if (regularFile && requestedName.extension() != plan.source.extension()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_name";
            return std::nullopt;
        }
        plan.destination = plan.source.parent_path() / requestedName;
        if (plan.destination == plan.source)
            return std::nullopt;
        if (DirectoryContainsPortableName(plan.source.parent_path(), requestedName.string(), plan.source)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.name_exists";
            return std::nullopt;
        }

        const bool directory = std::filesystem::is_directory(plan.source, error);
        if (error) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return std::nullopt;
        }
        if (directory) {
            plan.sources.push_back(plan.source);
            return plan;
        }

        const std::filesystem::path projectRoot = NormalizeAbsolute(m_viewModel.projectRoot);
        const Assets::AssetRecord *record = m_assetRegistry.FindByPath(plan.source.lexically_relative(projectRoot).generic_string());
        const auto companions = ValidatedAssetCompanions(plan.source, record != nullptr);
        if (!companions.has_value()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.companion_invalid";
            return std::nullopt;
        }
        plan.sources = *companions;
        for (const std::filesystem::path &item : plan.sources) {
            const std::filesystem::path target = CompanionDestination(item, plan.source, plan.destination);
            if (DirectoryContainsPortableName(target.parent_path(), target.filename().string(), item)) {
                m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.name_exists";
                return std::nullopt;
            }
        }
        return plan;
    }

    bool EditorWorkspaceController::PublishRenamedContentBrowserEntries(const ContentBrowserPathMoves &moved) {
        const std::filesystem::path projectRoot = NormalizeAbsolute(m_viewModel.projectRoot);
        const auto rollback = [this, &moved, &projectRoot]() {
            const bool rollbackComplete = RollbackPathMoves(moved);
            static_cast<void>(Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, projectRoot, Assets::AssetRegistryOpenMode::Edit));
            SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.registry_failed");
            return false;
        };
        if (auto rebuilt = Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, projectRoot, Assets::AssetRegistryOpenMode::Edit);
            rebuilt.HasError() || rebuilt.Value().status != Assets::AssetRegistryBuildStatus::Complete) {
            return rollback();
        }
        const Assets::AssetRegistrySnapshot rebuiltSnapshot = m_mutableAssetRegistry->Snapshot();
        if (const bool registryPreserved = rebuiltSnapshot.Records().size() == m_assetRegistry.Records().size() &&
                                           std::ranges::all_of(m_assetRegistry.Records(),
                                                               [&rebuiltSnapshot](const Assets::AssetRecord &record) {
            return rebuiltSnapshot.Find(record.id) != nullptr;
        });
            !registryPreserved) {
            return rollback();
        }
        m_assetRegistry = rebuiltSnapshot;
        m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
        RebuildContentBrowserProjection(projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
        return true;
    }

    void EditorWorkspaceController::DeleteContentBrowserEntry(const std::filesystem::path &absolutePath) {
        m_viewModel.contentBrowserOperationError.clear();
        if (m_mutations == nullptr || m_durableFiles == nullptr || m_mutableAssetRegistry == nullptr) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.unavailable";
            return;
        }

        if (!std::filesystem::path{absolutePath}.is_absolute()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }
        const std::filesystem::path source = NormalizeAbsolute(absolutePath);
        if (!IsDirectContentBrowserEntry(m_viewModel.contentBrowser, source)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }

        if (auto lease = m_mutations->TryAcquire(ProjectMutationRequest{
                .projectRoot = m_viewModel.projectRoot,
                .owner = ProjectMutationOwner::Asset,
                .operationId = "content-browser-delete",
            });
            lease.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.busy";
            return;
        } else {
            const std::optional<ContentBrowserDeletePlan> plan = PrepareContentBrowserDelete(source);
            if (!plan.has_value())
                return;
            const std::optional<std::filesystem::path> trashDirectory = CreateContentBrowserTrash(*plan);
            if (!trashDirectory.has_value())
                return;
            const std::optional<ContentBrowserPathMoves> moved = MoveContentBrowserEntriesToTrash(*plan, *trashDirectory);
            if (moved.has_value())
                static_cast<void>(PublishContentBrowserDeletion(*plan, *trashDirectory, *moved));
        }
    }

    std::optional<EditorWorkspaceController::ContentBrowserDeletePlan> EditorWorkspaceController::PrepareContentBrowserDelete(
        const std::filesystem::path &source) {
        std::error_code error;
        const bool directory = std::filesystem::is_directory(source, error);
        if (error) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.delete_failed";
            return std::nullopt;
        }
        ContentBrowserDeletePlan plan{.projectRoot = NormalizeAbsolute(m_viewModel.projectRoot), .source = source};
        if (directory) {
            plan.sources.push_back(source);
            return plan;
        }

        plan.deletedAssetProjectPath = source.lexically_relative(plan.projectRoot).generic_string();
        const Assets::AssetRecord *record = m_assetRegistry.FindByPath(plan.deletedAssetProjectPath);
        if (record != nullptr)
            plan.deletedAssetId = record->id;
        const auto companions = ValidatedAssetCompanions(source, record != nullptr);
        if (!companions.has_value()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.companion_invalid";
            return std::nullopt;
        }
        plan.sources = *companions;
        return plan;
    }

    std::optional<std::filesystem::path> EditorWorkspaceController::CreateContentBrowserTrash(const ContentBrowserDeletePlan &plan) {
        std::error_code error;
        const auto stamp =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const std::filesystem::path trashRoot = plan.projectRoot / ".horo" / "local" / "trash";
        std::filesystem::create_directories(trashRoot, error);
        if (error) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.delete_failed";
            return std::nullopt;
        }
        const std::optional<std::filesystem::path> trashDirectory = CreateUniqueTrashDirectory(trashRoot, stamp);
        if (!trashDirectory.has_value()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.delete_failed";
            return std::nullopt;
        }

        nlohmann::json manifest{{"schemaVersion", 1},
                                {"originalAbsolutePath", plan.source.string()},
                                {"deletedAtUnixMicroseconds", stamp},
                                {"entries", nlohmann::json::array()}};
        for (const std::filesystem::path &item : plan.sources) {
            manifest["entries"].push_back({{"originalAbsolutePath", item.string()}, {"trashFileName", item.filename().string()}});
        }
        if (const std::optional<std::filesystem::path> manifestPath = SelectTrashManifestPath(*trashDirectory, plan.sources);
            !manifestPath.has_value() || m_durableFiles->WriteDurable(*manifestPath, JsonBytes(manifest)).HasError()) {
            std::filesystem::remove_all(*trashDirectory, error);
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.delete_failed";
            return std::nullopt;
        }
        return trashDirectory;
    }

    std::optional<EditorWorkspaceController::ContentBrowserPathMoves> EditorWorkspaceController::MoveContentBrowserEntriesToTrash(
        const ContentBrowserDeletePlan &plan, const std::filesystem::path &trashDirectory) {
        std::error_code error;
        ContentBrowserPathMoves moved;
        moved.reserve(plan.sources.size());
        for (const std::filesystem::path &item : plan.sources) {
            const std::filesystem::path target = trashDirectory / item.filename();
            if (!PathDoesNotExist(target)) {
                const bool rollbackComplete = RollbackPathMoves(moved);
                if (rollbackComplete)
                    std::filesystem::remove_all(trashDirectory, error);
                SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.delete_failed");
                return std::nullopt;
            }
            std::filesystem::rename(item, target, error);
            if (error) {
                const bool rollbackComplete = RollbackPathMoves(moved);
                if (rollbackComplete)
                    std::filesystem::remove_all(trashDirectory, error);
                SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.delete_failed");
                return std::nullopt;
            }
            moved.emplace_back(item, target);
        }
        if (m_durableFiles->SyncDirectory(plan.source.parent_path()).HasError() ||
            m_durableFiles->SyncDirectory(trashDirectory).HasError()) {
            const bool rollbackComplete = RollbackPathMoves(moved);
            if (rollbackComplete)
                std::filesystem::remove_all(trashDirectory, error);
            SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.delete_failed");
            return std::nullopt;
        }
        return moved;
    }

    bool EditorWorkspaceController::PublishContentBrowserDeletion(const ContentBrowserDeletePlan &plan,
                                                                  const std::filesystem::path &trashDirectory,
                                                                  const ContentBrowserPathMoves &moved) {
        const auto rollback = [this, &moved, &trashDirectory, &plan]() {
            const bool rollbackComplete = RollbackPathMoves(moved);
            if (rollbackComplete) {
                std::error_code error;
                std::filesystem::remove_all(trashDirectory, error);
            }
            static_cast<void>(Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, plan.projectRoot, Assets::AssetRegistryOpenMode::Edit));
            SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.registry_failed");
            return false;
        };
        if (auto rebuilt = Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, plan.projectRoot, Assets::AssetRegistryOpenMode::Edit);
            rebuilt.HasError() || rebuilt.Value().status == Assets::AssetRegistryBuildStatus::Failed) {
            return rollback();
        }
        const Assets::AssetRegistrySnapshot rebuiltSnapshot = m_mutableAssetRegistry->Snapshot();
        if (plan.deletedAssetId.has_value() && (rebuiltSnapshot.Find(*plan.deletedAssetId) != nullptr ||
                                                rebuiltSnapshot.FindByPath(plan.deletedAssetProjectPath) != nullptr)) {
            return rollback();
        }
        LOG_INFO("editor.content_browser", "Moved asset entry to recoverable project trash: %s", trashDirectory.string().c_str());
        m_assetRegistry = rebuiltSnapshot;
        m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
        RebuildContentBrowserProjection(plan.projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
        return true;
    }
}  // namespace Horo::Editor
