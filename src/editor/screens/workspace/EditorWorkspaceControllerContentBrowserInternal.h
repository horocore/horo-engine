#pragma once

#include "Horo/Assets/AssetReimport.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/PathUtils.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

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
}  // namespace Horo::Editor
