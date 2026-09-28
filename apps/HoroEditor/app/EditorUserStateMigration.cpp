#include "EditorUserStateMigration.h"

#include "Horo/Release/UserStateMigrationErrors.h"

#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <utility>

namespace Horo::Editor {
    namespace {
        using Json = nlohmann::json;

        /** @brief Bounds legacy input before deriving exact content-addressed steps. */
        [[nodiscard]] Result<std::string> ReadLegacy(const std::filesystem::path &path) {
            std::error_code error;
            if (const auto status = std::filesystem::symlink_status(path, error); error || !std::filesystem::is_regular_file(status))
                return Result<std::string>::Failure(MakeError(Release::UserStateMigrationErrors::UnsafePath));
            if (const auto links = std::filesystem::hard_link_count(path, error); error || links != 1U)
                return Result<std::string>::Failure(MakeError(Release::UserStateMigrationErrors::UnsafePath));
            constexpr std::uintmax_t MaximumBytes = 8U * 1024U * 1024U;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size == 0U || size > MaximumBytes)
                return Result<std::string>::Failure(MakeError(Release::UserStateMigrationErrors::UnsafePath));
            std::ifstream input(path, std::ios::binary);
            std::string bytes(static_cast<std::size_t>(size), '\0');
            input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            return input ? Result<std::string>::Success(std::move(bytes))
                         : Result<std::string>::Failure(MakeError(Release::UserStateMigrationErrors::UnsafePath));
        }

        /** @brief Supplies only the precomputed canonical target bytes; secret material never enters this adapter. */
        class LegacyTransformer final : public Release::IUserStateMigrationTransformer {
        public:
            explicit LegacyTransformer(const std::map<std::string, std::string, std::less<>> &targets) : targets_(targets) {}

            [[nodiscard]] Result<std::vector<std::byte>> Transform(const Release::UserStateMigrationStep &step,
                                                                   std::span<const std::byte>) override {
                const auto found = targets_.find(step.relativePath.generic_string());
                if (found == targets_.end())
                    return Result<std::vector<std::byte>>::Failure(MakeError(Release::UserStateMigrationErrors::TransformFailed));
                const auto bytes = std::as_bytes(std::span{found->second});
                return Result<std::vector<std::byte>>::Success(std::vector<std::byte>{bytes.begin(), bytes.end()});
            }

            [[nodiscard]] Result<void> ReauthorizeCredentialReference(std::string_view) override {
                return Result<void>::Failure(MakeError(Release::UserStateMigrationErrors::TransformFailed));
            }

        private:
            const std::map<std::string, std::string, std::less<>> &targets_;
        };

        /** @brief Prepares one old document without reserializing or dropping unknown fields. */
        [[nodiscard]] Result<void> AppendStep(const std::filesystem::path &root, const std::string_view name,
                                              const Release::UserStateFamily family, std::vector<Release::UserStateMigrationStep> &steps,
                                              std::map<std::string, std::string, std::less<>> &targets) {
            const auto path = root / name;
            std::error_code error;
            if (const auto status = std::filesystem::symlink_status(path, error);
                status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory))
                return Result<void>::Success();
            auto source = ReadLegacy(path);
            if (source.HasError())
                return Result<void>::Failure(source.ErrorValue());
            const Json document = Json::parse(source.Value(), nullptr, false);
            if (document.is_discarded())
                return Result<void>::Failure(MakeError(Release::UserStateMigrationErrors::InvalidPlan));
            std::string target;
            if (family == Release::UserStateFamily::Preferences && document.is_object()) {
                if (document.contains("schemaVersion"))
                    return document["schemaVersion"] == 1
                               ? Result<void>::Success()
                               : Result<void>::Failure(MakeError(Release::UserStateMigrationErrors::InvalidPlan));
                target = source.Value();
                const auto opening = target.find('{');
                target.insert(opening + 1U, document.empty() ? R"("schemaVersion":1)" : R"("schemaVersion":1,)");
            } else if (family == Release::UserStateFamily::RecentProjects && document.is_array()) {
                target = R"({"schemaVersion":1,"entries":)" + source.Value() + "}\n";
            } else if (family == Release::UserStateFamily::RecentProjects && document.is_object() && document.contains("schemaVersion") &&
                       document["schemaVersion"] == 1 && document.contains("entries") && document["entries"].is_array()) {
                return Result<void>::Success();
            } else {
                return Result<void>::Failure(MakeError(Release::UserStateMigrationErrors::InvalidPlan));
            }
            steps.push_back({family,
                             Release::UserStateMigrationAction::Transform,
                             std::filesystem::path{name},
                             0U,
                             1U,
                             ComputeSha256(std::as_bytes(std::span{source.Value()})),
                             ComputeSha256(std::as_bytes(std::span{target})),
                             {}});
            targets.try_emplace(std::string{name}, std::move(target));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc MigrateLegacyEditorUserState */
    Result<Release::UserStateMigrationReport> MigrateLegacyEditorUserState(const std::filesystem::path &userStateRoot,
                                                                           const std::filesystem::path &cacheRoot,
                                                                           NativeDurableFileSystem &files) {
        std::vector<Release::UserStateMigrationStep> steps;
        std::map<std::string, std::string, std::less<>> targets;
        if (auto settings = AppendStep(userStateRoot, "editor_settings.json", Release::UserStateFamily::Preferences, steps, targets);
            settings.HasError())
            return Result<Release::UserStateMigrationReport>::Failure(settings.ErrorValue());
        if (auto recent = AppendStep(userStateRoot, "recent_projects.json", Release::UserStateFamily::RecentProjects, steps, targets);
            recent.HasError())
            return Result<Release::UserStateMigrationReport>::Failure(recent.ErrorValue());
        if (steps.empty())
            return Result<Release::UserStateMigrationReport>::Success({});
        LegacyTransformer transformer{targets};
        return Release::RunUserStateMigration({userStateRoot, cacheRoot, steps}, files, transformer);
    }
}  // namespace Horo::Editor
