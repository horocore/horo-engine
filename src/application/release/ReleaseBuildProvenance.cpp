#include "Horo/Release/ReleaseBuildProvenance.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <ranges>
#include <span>
#include <utility>

namespace Horo::Release {
    namespace {
        using Json = nlohmann::ordered_json;
        constexpr std::size_t MaximumBytes = 16U * 1024U * 1024U;
        constexpr std::size_t MaximumFiles = 65'536U;
        constexpr std::size_t MaximumNames = 1'024U;

        [[nodiscard]] Error InvalidProvenance() {
            return MakeError(ReleaseErrors::PipelineOutputInvalid);
        }

        [[nodiscard]] char LowerAscii(const char value) noexcept {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        }

        [[nodiscard]] bool SafeName(const std::string_view value) {
            return !value.empty() && value.size() <= 128U && std::ranges::all_of(value, [](const unsigned char character) {
                return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
            });
        }

        [[nodiscard]] bool SafePath(const std::string_view path) {
            if (path.empty() || path.size() > 512U || path.front() == '/' || path.back() == '/' || path.find('\\') != path.npos)
                return false;
            std::size_t start = 0;
            while (start < path.size()) {
                const auto end = path.find('/', start);
                const auto part = path.substr(start, end == path.npos ? path.npos : end - start);
                if (!SafeName(part) || part.back() == '.' || part == "." || part == "..")
                    return false;
                std::string stem{part.substr(0, part.find('.'))};
                std::ranges::transform(stem, stem.begin(), LowerAscii);
                if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
                    (stem.size() == 4U && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9'))
                    return false;
                if (end == path.npos)
                    break;
                start = end + 1U;
            }
            return path != "manifest.json";
        }

        [[nodiscard]] bool ValidData(const ReleaseBuildProvenanceData &data) {
            if (data.files.empty() || data.files.size() > MaximumFiles || data.features.size() > MaximumNames ||
                data.environment.size() > MaximumNames)
                return false;
            for (const auto &feature : data.features)
                if (!SafeName(feature))
                    return false;
            for (const auto &entry : data.environment)
                if (!SafeName(entry.name))
                    return false;
            for (const auto &file : data.files)
                if (!SafePath(file.path))
                    return false;
            return true;
        }

        [[nodiscard]] bool UniquePaths(const std::vector<ReleaseUnsignedFile> &files) {
            std::vector<std::string> names;
            names.reserve(files.size());
            for (const auto &file : files) {
                std::string folded = file.path;
                std::ranges::transform(folded, folded.begin(), LowerAscii);
                names.push_back(std::move(folded));
            }
            std::ranges::sort(names);
            return std::ranges::adjacent_find(names) == names.end();
        }

        [[nodiscard]] Json Write(const ReleaseBuildProvenanceData &data) {
            Json document;
            document["schemaVersion"] = 1;
            document["frozen"] = {{"sourceTree", FormatSha256(data.frozen.sourceTree)},
                                  {"dependencyLock", FormatSha256(data.frozen.dependencyLock)},
                                  {"profile", FormatSha256(data.frozen.profile)},
                                  {"toolchain", FormatSha256(data.frozen.toolchain)},
                                  {"policy", FormatSha256(data.frozen.policy)},
                                  {"notes", FormatSha256(data.frozen.notes)}};
            document["buildScript"] = FormatSha256(data.buildScript);
            document["normalization"] = {{"locale", "C"}, {"timezone", "UTC"}, {"sourceEpochSeconds", data.sourceEpochSeconds}};
            document["features"] = data.features;
            document["environment"] = Json::array();
            for (const auto &entry : data.environment)
                document["environment"].push_back({{"name", entry.name}, {"sha256", FormatSha256(entry.digest)}});
            document["files"] = Json::array();
            for (const auto &file : data.files)
                document["files"].push_back({{"path", file.path}, {"size", file.size}, {"sha256", FormatSha256(file.digest)}});
            return document;
        }

        [[nodiscard]] bool ReadDigest(const Json &document, const char *name, Sha256Digest &digest) {
            if (!document.contains(name) || !document[name].is_string())
                return false;
            auto parsed = ParseSha256(document[name].get<std::string>());
            if (parsed.HasError())
                return false;
            digest = parsed.Value();
            return true;
        }

        [[nodiscard]] bool Read(const Json &document, ReleaseBuildProvenanceData &data) {
            if (!document.is_object() || document.size() != 7U || document.value("schemaVersion", 0) != 1 || !document.contains("frozen") ||
                !document["frozen"].is_object() || document["frozen"].size() != 6U || !document.contains("normalization") ||
                !document["normalization"].is_object() || document["normalization"].size() != 3U ||
                document["normalization"].value("locale", "") != "C" || document["normalization"].value("timezone", "") != "UTC" ||
                !document["normalization"].contains("sourceEpochSeconds") ||
                !document["normalization"]["sourceEpochSeconds"].is_number_unsigned() || !document.contains("features") ||
                !document["features"].is_array() || !document.contains("environment") || !document["environment"].is_array() ||
                !document.contains("files") || !document["files"].is_array())
                return false;
            const Json &frozen = document["frozen"];
            if (!ReadDigest(frozen, "sourceTree", data.frozen.sourceTree) ||
                !ReadDigest(frozen, "dependencyLock", data.frozen.dependencyLock) || !ReadDigest(frozen, "profile", data.frozen.profile) ||
                !ReadDigest(frozen, "toolchain", data.frozen.toolchain) || !ReadDigest(frozen, "policy", data.frozen.policy) ||
                !ReadDigest(frozen, "notes", data.frozen.notes) || !ReadDigest(document, "buildScript", data.buildScript))
                return false;
            data.sourceEpochSeconds = document["normalization"]["sourceEpochSeconds"].get<std::uint64_t>();
            if (document["features"].size() > MaximumNames || document["environment"].size() > MaximumNames ||
                document["files"].size() > MaximumFiles)
                return false;
            data.features = document["features"].get<std::vector<std::string>>();
            for (const Json &entry : document["environment"]) {
                if (!entry.is_object() || entry.size() != 2U || !entry.contains("name") || !entry["name"].is_string())
                    return false;
                ReleaseEnvironmentIdentity value;
                value.name = entry["name"].get<std::string>();
                if (!ReadDigest(entry, "sha256", value.digest))
                    return false;
                data.environment.push_back(std::move(value));
            }
            for (const Json &entry : document["files"]) {
                if (!entry.is_object() || entry.size() != 3U || !entry.contains("path") || !entry["path"].is_string() ||
                    !entry.contains("size") || !entry["size"].is_number_unsigned())
                    return false;
                ReleaseUnsignedFile value;
                value.path = entry["path"].get<std::string>();
                value.size = entry["size"].get<std::uint64_t>();
                if (!ReadDigest(entry, "sha256", value.digest))
                    return false;
                data.files.push_back(std::move(value));
            }
            return true;
        }
    }  // namespace

    ReleaseBuildProvenance::ReleaseBuildProvenance(ReleaseBuildProvenanceData data, std::string json, const Sha256Digest digest)
        : data_(std::move(data)), json_(std::move(json)), digest_(digest) {}

    /** @copydoc ReleaseBuildProvenance::Create */
    Result<ReleaseBuildProvenance> ReleaseBuildProvenance::Create(ReleaseBuildProvenanceData data) {
        if (!ValidData(data))
            return Result<ReleaseBuildProvenance>::Failure(InvalidProvenance());
        std::ranges::sort(data.features);
        std::ranges::sort(data.environment, {}, &ReleaseEnvironmentIdentity::name);
        std::ranges::sort(data.files, {}, &ReleaseUnsignedFile::path);
        if (std::ranges::adjacent_find(data.features) != data.features.end() ||
            std::ranges::adjacent_find(data.environment, {}, &ReleaseEnvironmentIdentity::name) != data.environment.end() ||
            !UniquePaths(data.files))
            return Result<ReleaseBuildProvenance>::Failure(InvalidProvenance());
        std::string json = Write(data).dump();
        if (json.size() > MaximumBytes)
            return Result<ReleaseBuildProvenance>::Failure(InvalidProvenance());
        const Sha256Digest digest = ComputeSha256(std::as_bytes(std::span{json}));
        return Result<ReleaseBuildProvenance>::Success(ReleaseBuildProvenance{std::move(data), std::move(json), digest});
    }

    /** @copydoc ReleaseBuildProvenance::ParseCanonical */
    Result<ReleaseBuildProvenance> ReleaseBuildProvenance::ParseCanonical(const std::string_view json) {
        if (json.empty() || json.size() > MaximumBytes)
            return Result<ReleaseBuildProvenance>::Failure(InvalidProvenance());
        const Json document = Json::parse(json, nullptr, false);
        if (document.is_discarded())
            return Result<ReleaseBuildProvenance>::Failure(InvalidProvenance());
        try {
            ReleaseBuildProvenanceData data;
            if (!Read(document, data))
                return Result<ReleaseBuildProvenance>::Failure(InvalidProvenance());
            auto result = Create(std::move(data));
            if (result.HasError() || result.Value().CanonicalJson() != json)
                return Result<ReleaseBuildProvenance>::Failure(InvalidProvenance());
            return result;
        } catch (const Json::exception &) {
            return Result<ReleaseBuildProvenance>::Failure(InvalidProvenance());
        }
    }

    /** @copydoc ReleaseBuildProvenance::CanonicalJson */
    const std::string &ReleaseBuildProvenance::CanonicalJson() const noexcept {
        return json_;
    }

    /** @copydoc ReleaseBuildProvenance::Digest */
    const Sha256Digest &ReleaseBuildProvenance::Digest() const noexcept {
        return digest_;
    }

    /** @copydoc ReleaseBuildProvenance::Data */
    const ReleaseBuildProvenanceData &ReleaseBuildProvenance::Data() const noexcept {
        return data_;
    }

    /** @copydoc ReleaseBuildProvenance::Compare */
    std::vector<ReleaseBuildVariance> ReleaseBuildProvenance::Compare(const ReleaseBuildProvenance &other) const {
        std::vector<ReleaseBuildVariance> differences;
        const auto check = [&differences](const bool changed, const char *name) {
            if (changed)
                differences.push_back({name});
        };
        check(data_.frozen.sourceTree != other.data_.frozen.sourceTree, "frozen.sourceTree");
        check(data_.frozen.dependencyLock != other.data_.frozen.dependencyLock, "frozen.dependencyLock");
        check(data_.frozen.profile != other.data_.frozen.profile, "frozen.profile");
        check(data_.frozen.toolchain != other.data_.frozen.toolchain, "frozen.toolchain");
        check(data_.frozen.policy != other.data_.frozen.policy, "frozen.policy");
        check(data_.frozen.notes != other.data_.frozen.notes, "frozen.notes");
        check(data_.buildScript != other.data_.buildScript, "buildScript");
        check(data_.sourceEpochSeconds != other.data_.sourceEpochSeconds, "normalization.sourceEpochSeconds");
        check(data_.features != other.data_.features, "features");
        std::size_t environmentLeft = 0;
        std::size_t environmentRight = 0;
        while (environmentLeft < data_.environment.size() || environmentRight < other.data_.environment.size()) {
            if (environmentRight == other.data_.environment.size() ||
                (environmentLeft < data_.environment.size() &&
                 data_.environment[environmentLeft].name < other.data_.environment[environmentRight].name)) {
                differences.push_back({"environment." + data_.environment[environmentLeft++].name});
            } else if (environmentLeft == data_.environment.size() ||
                       other.data_.environment[environmentRight].name < data_.environment[environmentLeft].name) {
                differences.push_back({"environment." + other.data_.environment[environmentRight++].name});
            } else {
                if (data_.environment[environmentLeft] != other.data_.environment[environmentRight])
                    differences.push_back({"environment." + data_.environment[environmentLeft].name});
                ++environmentLeft;
                ++environmentRight;
            }
        }
        std::size_t left = 0;
        std::size_t right = 0;
        while (left < data_.files.size() || right < other.data_.files.size()) {
            if (right == other.data_.files.size() ||
                (left < data_.files.size() && data_.files[left].path < other.data_.files[right].path)) {
                differences.push_back({"files." + data_.files[left++].path});
            } else if (left == data_.files.size() || other.data_.files[right].path < data_.files[left].path) {
                differences.push_back({"files." + other.data_.files[right++].path});
            } else {
                if (data_.files[left] != other.data_.files[right])
                    differences.push_back({"files." + data_.files[left].path});
                ++left;
                ++right;
            }
        }
        return differences;
    }
}  // namespace Horo::Release
