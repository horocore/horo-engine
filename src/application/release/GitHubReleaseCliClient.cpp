#include "Horo/Release/GitHubReleaseCliClient.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <random>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <sddl.h>
#include <windows.h>
#else
#include <stdlib.h>
#include <unistd.h>
#endif

namespace Horo::Release {
    namespace {
        using Json = nlohmann::json;

        [[nodiscard]] bool SafeIdentity(const std::string_view text, const bool repository) {
            if (text.empty() || text.size() > 200U)
                return false;
            std::size_t slashes = 0;
            for (const char character : text) {
                if (character == '/') {
                    ++slashes;
                    continue;
                }
                if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-' ||
                    (!repository && character == '+'))
                    continue;
                return false;
            }
            if (repository) {
                const auto slash = text.find('/');
                return slashes == 1U && slash != 0U && slash + 1U != text.size();
            }
            return slashes == 0U && text.front() == 'v';
        }

        [[nodiscard]] bool SafeAssetName(const std::string_view name) {
            if (name.empty() || name.size() > 1024U || name.front() == '-')
                return false;
            return std::ranges::all_of(name, [](const char character) {
                return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-' ||
                       character == '%';
            });
        }

        [[nodiscard]] bool SafeGitObjectId(const std::string_view objectId) {
            if (objectId.size() != 40U)
                return false;
            return std::ranges::all_of(objectId, [](const char character) {
                return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
            });
        }

        [[nodiscard]] Result<Json> ParseResponse(const std::string &response) {
            try {
                return Result<Json>::Success(Json::parse(response));
            } catch (const Json::exception &) {
                return Result<Json>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            }
        }

        [[nodiscard]] Result<GitHubReleaseAssetEvidence> MeasureFile(const std::filesystem::path &file, const std::string_view name) {
            std::ifstream input(file, std::ios::binary);
            if (!input)
                return Result<GitHubReleaseAssetEvidence>::Failure(MakeError(ReleaseErrors::PipelineStagingIoFailed));
            Sha256Builder hash;
            std::array<char, 64U * 1024U> block{};
            std::uint64_t size = 0;
            while (input) {
                input.read(block.data(), static_cast<std::streamsize>(block.size()));
                const auto count = input.gcount();
                if (count > 0) {
                    size += static_cast<std::uint64_t>(count);
                    if (!hash.Update(std::as_bytes(std::span{block.data(), static_cast<std::size_t>(count)})))
                        return Result<GitHubReleaseAssetEvidence>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
                }
            }
            if (!input.eof())
                return Result<GitHubReleaseAssetEvidence>::Failure(MakeError(ReleaseErrors::PipelineStagingIoFailed));
            return Result<GitHubReleaseAssetEvidence>::Success({std::string{name}, size, hash.Finalize()});
        }

        class TemporaryAsset final {
        public:
            TemporaryAsset(const TemporaryAsset &) = delete;
            TemporaryAsset &operator=(const TemporaryAsset &) = delete;
            TemporaryAsset(TemporaryAsset &&) = delete;
            TemporaryAsset &operator=(TemporaryAsset &&) = delete;

            TemporaryAsset() {
                std::error_code error;
                // Only an atomically created owner-only child of this shared directory is used for files.
                const auto temporary = std::filesystem::temp_directory_path(error);  // NOSONAR(cpp:S5443)
                if (error)
                    return;
#if !defined(_WIN32)
                std::string pattern = (temporary / "horo-release-XXXXXX").string();
                std::vector<char> buffer(pattern.begin(), pattern.end());
                buffer.push_back('\0');
                if (const char *created = ::mkdtemp(buffer.data()))
                    root_ = created;
#else
                PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
                if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)(A;;FA;;;SY)", SDDL_REVISION_1, &rawDescriptor,
                                                                          nullptr))
                    return;
                const std::unique_ptr<void, decltype(&LocalFree)> descriptor{rawDescriptor, &LocalFree};
                SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
                std::random_device random;
                for (int attempt = 0; attempt < 8; ++attempt) {
                    root_ = temporary / ("horo-release-" + std::to_string(random()) + "-" + std::to_string(random()));
                    if (CreateDirectoryW(root_.c_str(), &attributes))
                        return;
                    const auto createError = GetLastError();
                    root_.clear();
                    if (createError != ERROR_ALREADY_EXISTS)
                        return;
                }
#endif
            }

            ~TemporaryAsset() {
                if (!root_.empty()) {
                    std::error_code error;
                    std::filesystem::remove_all(root_, error);
                }
            }

            [[nodiscard]] bool Valid() const noexcept {
                return !root_.empty();
            }

            [[nodiscard]] std::filesystem::path Path(const std::string_view name) const {
                return root_ / name;
            }

        private:
            std::filesystem::path root_;
        };

        [[nodiscard]] bool Matches(const GitHubReleaseAssetEvidence &actual, const ReleaseArtifactRecord &expected) {
            return actual.size == expected.size && actual.digest == expected.digest;
        }

        /** @brief Finds one asset identity in a page and rejects duplicate names or invalid IDs. */
        [[nodiscard]] Result<void> FindAssetOnPage(const Json &assets, const std::string_view name, std::uint64_t &found) {
            try {
                for (const auto &asset : assets) {
                    if (asset.at("name").get<std::string>() != name)
                        continue;
                    if (found != 0U)
                        return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputCollision));
                    found = asset.at("id").get<std::uint64_t>();
                    if (found == 0U)
                        return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
                }
            } catch (const Json::exception &) {
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc GitHubReleaseCliClient::GitHubReleaseCliClient */
    GitHubReleaseCliClient::GitHubReleaseCliClient(IExternalProcessRunner &processes, CancellationToken cancellation) noexcept
        : processes_(processes), cancellation_(std::move(cancellation)) {}

    /** @brief Executes one bounded GitHub CLI request; output and errors never include credential values. */
    Result<std::string> GitHubReleaseCliClient::Run(std::vector<std::string> arguments) const {
        ExternalProcessRequest request;
        request.executable = "gh";
        request.arguments = std::move(arguments);
        request.timeout = std::chrono::minutes{5};
        request.maximumLineBytes = 1024U * 1024U;
        request.maximumOutputBytes = 4U * 1024U * 1024U;
        std::string output;
        bool truncated = false;
        request.onOutput = [&output, &request, &truncated](const ProcessOutputLine &line) {
            if (line.stream == ProcessOutputStream::StandardOutput) {
                if (line.truncated || output.size() + line.text.size() + 1U > request.maximumOutputBytes) {
                    truncated = true;
                    return;
                }
                output += line.text;
                output.push_back('\n');
            }
        };
        if (auto result = processes_.Run(request, cancellation_);
            result.HasError() || result.Value().reason != ProcessTerminationReason::Exited || result.Value().exitCode != 0 || truncated)
            return Result<std::string>::Failure(MakeError(ReleaseErrors::PipelineProcessFailed));
        return Result<std::string>::Success(std::move(output));
    }

    /** @copydoc GitHubReleaseCliClient::FindExisting */
    Result<GitHubReleaseIdentity> GitHubReleaseCliClient::FindExisting(const std::string_view repository, const std::string_view tag) {
        if (!SafeIdentity(repository, true) || !SafeIdentity(tag, false))
            return Result<GitHubReleaseIdentity>::Failure(MakeError(ReleaseErrors::PipelineInputChanged));
        auto response = Run({"api", "repos/" + std::string{repository} + "/releases/tags/" + std::string{tag}});
        if (response.HasError())
            return Result<GitHubReleaseIdentity>::Failure(response.ErrorValue());
        auto parsed = ParseResponse(response.Value());
        if (parsed.HasError())
            return Result<GitHubReleaseIdentity>::Failure(parsed.ErrorValue());
        try {
            const auto &document = parsed.Value();
            const auto id = document.at("id").get<std::uint64_t>();
            const auto remoteTag = document.at("tag_name").get<std::string>();
            const auto body = document.at("body").get<std::string>();
            if (id == 0U || remoteTag != tag || document.at("draft").get<bool>() || body.size() > MaximumReleaseNotesSnapshotBytes)
                return Result<GitHubReleaseIdentity>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            auto commit = ResolveSourceCommit(repository, tag);
            if (commit.HasError())
                return Result<GitHubReleaseIdentity>::Failure(commit.ErrorValue());
            return Result<GitHubReleaseIdentity>::Success({std::string{repository}, remoteTag, id, std::move(commit).Value(), body});
        } catch (const Json::exception &) {
            return Result<GitHubReleaseIdentity>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        }
    }

    /** @brief Peels a bounded annotated tag chain to the exact source commit. */
    Result<std::string> GitHubReleaseCliClient::ResolveSourceCommit(const std::string_view repository, const std::string_view tag) const {
        auto response = Run({"api", "repos/" + std::string{repository} + "/git/ref/tags/" + std::string{tag}});
        if (response.HasError())
            return response;
        for (std::uint32_t depth = 0U; depth < 4U; ++depth) {
            auto parsed = ParseResponse(response.Value());
            if (parsed.HasError())
                return Result<std::string>::Failure(parsed.ErrorValue());
            try {
                const auto &object = parsed.Value().at("object");
                const auto type = object.at("type").get<std::string>();
                const auto sha = object.at("sha").get<std::string>();
                if (!SafeGitObjectId(sha))
                    return Result<std::string>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
                if (type == "commit")
                    return Result<std::string>::Success(sha);
                if (type != "tag")
                    return Result<std::string>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
                response = Run({"api", "repos/" + std::string{repository} + "/git/tags/" + sha});
                if (response.HasError())
                    return response;
            } catch (const Json::exception &) {
                return Result<std::string>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            }
        }
        return Result<std::string>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
    }

    /** @brief Rejects changed or removed tag-to-release bindings between operations. */
    Result<void> GitHubReleaseCliClient::ConfirmIdentity(const GitHubReleaseIdentity &release) {
        auto current = FindExisting(release.repository, release.tag);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (current.Value().releaseId != release.releaseId || current.Value().sourceCommit != release.sourceCommit ||
            current.Value().body != release.body)
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        return Result<void>::Success();
    }

    /** @brief Resolves one exact asset without treating a failed API call as an absent asset. */
    Result<std::uint64_t> GitHubReleaseCliClient::AssetId(const GitHubReleaseIdentity &release, const std::string_view name) const {
        std::uint64_t found = 0U;
        for (std::uint32_t page = 1U; page <= 10U; ++page) {
            auto response =
                Run({"api", std::format("repos/{}/releases/{}/assets?per_page=100&page={}", release.repository, release.releaseId, page)});
            if (response.HasError())
                return Result<std::uint64_t>::Failure(response.ErrorValue());
            auto parsed = ParseResponse(response.Value());
            if (parsed.HasError() || !parsed.Value().is_array())
                return Result<std::uint64_t>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            if (auto foundOnPage = FindAssetOnPage(parsed.Value(), name, found); foundOnPage.HasError())
                return Result<std::uint64_t>::Failure(foundOnPage.ErrorValue());
            if (parsed.Value().size() < 100U)
                return Result<std::uint64_t>::Success(found);
        }
        return Result<std::uint64_t>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
    }

    /** @copydoc GitHubReleaseCliClient::ReadAsset */
    Result<GitHubReleaseAssetEvidence> GitHubReleaseCliClient::ReadAsset(const GitHubReleaseIdentity &release,
                                                                         const std::string_view name) {
        if (!SafeAssetName(name))
            return Result<GitHubReleaseAssetEvidence>::Failure(MakeError(ReleaseErrors::PipelineInputChanged));
        if (auto current = ConfirmIdentity(release); current.HasError())
            return Result<GitHubReleaseAssetEvidence>::Failure(current.ErrorValue());
        auto asset = AssetId(release, name);
        if (asset.HasError())
            return Result<GitHubReleaseAssetEvidence>::Failure(asset.ErrorValue());
        if (asset.Value() == 0U)
            return Result<GitHubReleaseAssetEvidence>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        TemporaryAsset temporary;
        if (!temporary.Valid())
            return Result<GitHubReleaseAssetEvidence>::Failure(MakeError(ReleaseErrors::PipelineStagingIoFailed));
        const auto download = temporary.Path("asset");
        if (auto result = Run({"release", "download", release.tag, "--repo", release.repository, "--pattern", std::string{name}, "--output",
                               download.string()});
            result.HasError())
            return Result<GitHubReleaseAssetEvidence>::Failure(result.ErrorValue());
        auto measured = MeasureFile(download, name);
        if (measured.HasError())
            return measured;
        if (auto current = ConfirmIdentity(release); current.HasError())
            return Result<GitHubReleaseAssetEvidence>::Failure(current.ErrorValue());
        return measured;
    }

    /** @copydoc GitHubReleaseCliClient::UploadExact */
    Result<void> GitHubReleaseCliClient::UploadExact(const GitHubReleaseIdentity &release, const std::string_view name,
                                                     const std::filesystem::path &file, const ReleaseArtifactRecord &evidence) {
        if (!SafeAssetName(name))
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineInputChanged));
        auto local = MeasureFile(file, name);
        if (local.HasError())
            return Result<void>::Failure(local.ErrorValue());
        if (!Matches(local.Value(), evidence))
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineInputChanged));
        if (auto current = ConfirmIdentity(release); current.HasError())
            return current;
        auto asset = AssetId(release, name);
        if (asset.HasError())
            return Result<void>::Failure(asset.ErrorValue());
        if (asset.Value() != 0U) {
            auto remote = ReadAsset(release, name);
            if (remote.HasError())
                return Result<void>::Failure(remote.ErrorValue());
            return Matches(remote.Value(), evidence) ? Result<void>::Success()
                                                     : Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputCollision));
        }
        TemporaryAsset temporary;
        if (!temporary.Valid())
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineStagingIoFailed));
        const auto staged = temporary.Path(name);
        std::error_code error;
        std::filesystem::create_hard_link(file, staged, error);
        if (error) {
            error.clear();
            std::filesystem::copy_file(file, staged, std::filesystem::copy_options::none, error);
            if (error)
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineStagingIoFailed));
        }
        if (auto stagedEvidence = MeasureFile(staged, name); stagedEvidence.HasError() || !Matches(stagedEvidence.Value(), evidence))
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineInputChanged));
        auto upload = Run({"release", "upload", release.tag, staged.string(), "--repo", release.repository});
        if (auto remote = ReadAsset(release, name); remote.HasValue() && Matches(remote.Value(), evidence))
            return Result<void>::Success();
        if (upload.HasError())
            return Result<void>::Failure(upload.ErrorValue());
        return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
    }

    /** @copydoc GitHubReleaseCliClient::Commit */
    Result<void> GitHubReleaseCliClient::Commit(const GitHubReleaseIdentity &release, const ReleaseChannel &channel,
                                                const Sha256Digest &manifestDigest) {
        if (channel.kind != ReleaseChannelKind::Stable || !channel.customId.empty())
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineInputChanged));
        auto manifest = ReadAsset(release, "manifest.json");
        if (manifest.HasError())
            return Result<void>::Failure(manifest.ErrorValue());
        if (manifest.Value().digest != manifestDigest)
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        if (auto changed = Run({"api", "--method", "PATCH", std::format("repos/{}/releases/{}", release.repository, release.releaseId),
                                "--field", "make_latest=true", "--silent"});
            changed.HasError())
            return Result<void>::Failure(changed.ErrorValue());
        auto latest = Run({"api", "repos/" + release.repository + "/releases/latest"});
        if (latest.HasError())
            return Result<void>::Failure(latest.ErrorValue());
        auto parsed = ParseResponse(latest.Value());
        if (parsed.HasError())
            return Result<void>::Failure(parsed.ErrorValue());
        try {
            if (parsed.Value().at("id").get<std::uint64_t>() == release.releaseId)
                return Result<void>::Success();
        } catch (const Json::exception &) {
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        }
        return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
    }
}  // namespace Horo::Release
