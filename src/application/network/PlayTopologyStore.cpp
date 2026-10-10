#include "PlayTopologyInternal.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <span>
#include <utility>

namespace Horo::Application {
    namespace {
        /** @brief Read a regular bounded document; missing files retain empty revision-one defaults. */
        Result<std::optional<std::string>> Read(const std::filesystem::path &path) {
            using ReadResult = Result<std::optional<std::string>>;
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found)
                return ReadResult::Success(std::nullopt);
            if (error || !std::filesystem::is_regular_file(status))
                return ReadResult::Failure(MakeError(PlayTopologyErrors::Storage));
            std::ifstream input{path, std::ios::binary};
            std::array<char, 65537> bytes{};
            input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            const auto count = input.gcount();
            if (input.bad() || (!input && !input.eof()))
                return ReadResult::Failure(MakeError(PlayTopologyErrors::Storage));
            if (count <= 0 || count > 65536)
                return ReadResult::Failure(MakeError(PlayTopologyErrors::Invalid));
            return ReadResult::Success(std::string{bytes.data(), static_cast<std::size_t>(count)});
        }

        /** @brief Load one independent typed document without changing committed state on parse failure. */
        template <typename Value, typename Parser> Result<Value> Load(const std::filesystem::path &path, Parser parse) {
            auto document = Read(path);
            if (document.HasError())
                return Result<Value>::Failure(document.ErrorValue());
            return document.Value() ? parse(*document.Value()) : Result<Value>::Success(Value{});
        }

        /** @brief Re-read exact publication under the caller's writer lease to reject external changes. */
        template <typename Value, typename Parser>
        Result<void> CheckCurrent(const std::filesystem::path &path, const Value &expected, Parser parse) {
            const auto current = Load<Value>(path, parse);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (current.Value() != expected)
                return Result<void>::Failure(MakeError(PlayTopologyErrors::Stale));
            return Result<void>::Success();
        }

        /** @brief A zero port may clear an obsolete standalone override, but cannot create one. */
        bool ValidOverrideTarget(const PlayTopologyCatalog &project, const PlayTopologyOverride value) {
            const auto profile = std::ranges::find(project.profiles, value.profile, &PlayTopologyProfile::id);
            if (profile == project.profiles.end())
                return false;
            return value.port == 0 || profile->kind != PlayTopologyKind::Standalone;
        }

        /** @brief Publish a complete prepared document while the caller retains exclusive writer authority. */
        Result<void> Publish(DurableFileSystem &files, const std::filesystem::path &path, const std::string &document) {
            auto prepared = path;
            prepared += ".next";
            if (auto written = files.WriteDurable(prepared, std::as_bytes(std::span{document})); written.HasError())
                return written;
            return files.AtomicReplace(prepared, path);
        }
    }  // namespace

    /** @copydoc PlayTopologyStore::PlayTopologyStore */
    PlayTopologyStore::PlayTopologyStore(DurableFileSystem &files, std::filesystem::path project, std::filesystem::path user)
        : files_(files), projectPath_(std::move(project)), userPath_(std::move(user)) {}

    /** @copydoc PlayTopologyStore::Reload */
    Result<void> PlayTopologyStore::Reload() {
        if (projectPath_.empty() || userPath_.empty() || projectPath_.lexically_normal() == userPath_.lexically_normal())
            return Result<void>::Failure(MakeError(PlayTopologyErrors::Storage));
        auto project = Load<PlayTopologyCatalog>(projectPath_, ParsePlayTopologies);
        auto user = Load<PlayTopologyUserSettings>(userPath_, ParsePlayTopologyOverrides);
        if (project.HasError())
            return Result<void>::Failure(project.ErrorValue());
        if (user.HasError())
            return Result<void>::Failure(user.ErrorValue());
        project_ = std::move(project).Value();
        user_ = std::move(user).Value();
        loaded_ = true;
        return Result<void>::Success();
    }

    /** @copydoc PlayTopologyStore::Project */
    PlayTopologyCatalog PlayTopologyStore::Project() const {
        return project_;
    }

    /** @copydoc PlayTopologyStore::User */
    PlayTopologyUserSettings PlayTopologyStore::User() const {
        return user_;
    }

    /** @copydoc PlayTopologyStore::SaveProfile */
    Result<PlayTopologyCatalog> PlayTopologyStore::SaveProfile(const std::uint64_t revision, PlayTopologyProfile profile) {
        using Saved = Result<PlayTopologyCatalog>;
        if (!loaded_)
            return Saved::Failure(MakeError(PlayTopologyErrors::Storage));
        if (revision != project_.revision || revision == std::numeric_limits<std::uint64_t>::max())
            return Saved::Failure(MakeError(PlayTopologyErrors::Stale));
        if (auto valid = ValidatePlayTopology(profile); valid.HasError())
            return Saved::Failure(valid.ErrorValue());
        auto lockPath = projectPath_;
        lockPath += ".lock";
        auto lockResult = files_.TryAcquireExclusive(lockPath, "play-topology");
        if (lockResult.HasError())
            return Saved::Failure(lockResult.ErrorValue());
        // Keep exclusive ownership through the disk comparison and atomic publication.
        [[maybe_unused]] const auto lock = std::move(lockResult).Value();
        if (const auto current = CheckCurrent(projectPath_, project_, ParsePlayTopologies); current.HasError())
            return Saved::Failure(current.ErrorValue());
        auto candidate = project_;
        if (auto found = std::ranges::find(candidate.profiles, profile.id, &PlayTopologyProfile::id); found != candidate.profiles.end())
            *found = std::move(profile);
        else
            candidate.profiles.push_back(std::move(profile));
        ++candidate.revision;
        auto encoded = SerializePlayTopologies(candidate);
        if (encoded.HasError())
            return Saved::Failure(encoded.ErrorValue());
        if (auto written = Publish(files_, projectPath_, encoded.Value()); written.HasError())
            return Saved::Failure(written.ErrorValue());
        std::ranges::sort(candidate.profiles, {}, &PlayTopologyProfile::id);
        project_ = std::move(candidate);
        return Saved::Success(project_);
    }

    /** @copydoc PlayTopologyStore::SaveOverride */
    Result<PlayTopologyUserSettings> PlayTopologyStore::SaveOverride(const std::uint64_t revision, const PlayTopologyOverride value) {
        using Saved = Result<PlayTopologyUserSettings>;
        if (!loaded_)
            return Saved::Failure(MakeError(PlayTopologyErrors::Storage));
        if (revision != user_.revision || revision == std::numeric_limits<std::uint64_t>::max())
            return Saved::Failure(MakeError(PlayTopologyErrors::Stale));
        if (!ValidOverrideTarget(project_, value))
            return Saved::Failure(MakeError(PlayTopologyErrors::Invalid));
        auto lockPath = userPath_;
        lockPath += ".lock";
        auto lockResult = files_.TryAcquireExclusive(lockPath, "play-topology-user");
        if (lockResult.HasError())
            return Saved::Failure(lockResult.ErrorValue());
        // Keep exclusive ownership through the disk comparison and atomic publication.
        [[maybe_unused]] const auto lock = std::move(lockResult).Value();
        if (const auto current = CheckCurrent(userPath_, user_, ParsePlayTopologyOverrides); current.HasError())
            return Saved::Failure(current.ErrorValue());
        auto candidate = user_;
        std::erase_if(candidate.overrides, [value](const auto &entry) {
            return entry.profile == value.profile;
        });
        if (value.port != 0)
            candidate.overrides.push_back(value);
        ++candidate.revision;
        auto encoded = SerializePlayTopologyOverrides(candidate);
        if (encoded.HasError())
            return Saved::Failure(encoded.ErrorValue());
        if (auto written = Publish(files_, userPath_, encoded.Value()); written.HasError())
            return Saved::Failure(written.ErrorValue());
        std::ranges::sort(candidate.overrides, {}, &PlayTopologyOverride::profile);
        user_ = std::move(candidate);
        return Saved::Success(user_);
    }
}  // namespace Horo::Application
