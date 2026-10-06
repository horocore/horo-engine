#include "Horo/Packages/PackageRequest.h"

#include "Horo/Packages/PackagePath.h"
#include "PackageJsonGuard.h"
#include "PackageValidation.h"

#include <functional>
#include <nlohmann/json.hpp>

namespace Horo::Packages {
    namespace {
        using Json = nlohmann::json;
        const ErrorCodeDescriptor Invalid{ErrorDomainId{"packages.request"}, ErrorCode{"packages.request.invalid"}, ErrorSeverity::Error,
                                          "Portable package intent is invalid or unsupported.",
                                          "Validate named sources and bounded explicit dependencies."};
        const ErrorCodeDescriptor Stale{ErrorDomainId{"packages.request"}, ErrorCode{"packages.request.stale"}, ErrorSeverity::Error,
                                        "Package lock does not match current portable intent.",
                                        "Resolve and review the current request before cooking."};

        /** @brief Requires an exact shape with explicitly supported optional fields only. */
        bool Fields(const Json &value, const std::initializer_list<std::string_view> required,
                    const std::initializer_list<std::string_view> optional = {}) {
            if (!value.is_object())
                return false;
            for (const auto key : required)
                if (!value.contains(key))
                    return false;
            return std::ranges::all_of(value.items(), [required, optional](const auto &entry) {
                return std::ranges::find(required, entry.key()) != required.end() ||
                       std::ranges::find(optional, entry.key()) != optional.end();
            });
        }

        /** @brief Accepts safe credential-free HTTPS locators only; no fetching or authority selection occurs. */
        bool Locator(const Json &value) {
            if (!value.is_string())
                return false;
            const auto &url = value.get_ref<const std::string &>();
            if (!url.starts_with("https://") || url.size() <= 8 || url.find_first_of("@?#\\") != std::string::npos)
                return false;
            const auto authority =
                std::string_view{url}.substr(8, url.find('/', 8) == std::string::npos ? url.size() - 8 : url.find('/', 8) - 8);
            return !authority.empty() && std::ranges::all_of(url, [](const unsigned char c) {
                return c > 32 && c < 127;
            });
        }

        /** @brief Validates complete named-source semantics without weakening transport or signature policy. */
        bool Source(const Json &value) {
            if (!value.is_object() || !value.contains("kind") || !value["kind"].is_string())
                return false;
            if (value.contains("priority") &&
                (!value["priority"].is_number_unsigned() || value["priority"].get<std::uint64_t>() > 1'000'000))
                return false;
            const auto &kind = value["kind"].get_ref<const std::string &>();
            if (kind == "public-registry" || kind == "private-registry")
                return Fields(value, {"kind", "registry"}, {"priority"}) && value["registry"].is_string() &&
                       HoroPackageSourceId::Parse(value["registry"].get_ref<const std::string &>()).HasValue();
            if (kind == "static-index" || kind == "direct-artifact") {
                const auto hash = kind == "static-index" ? "indexSha256" : "sha256";
                return Fields(value, {"kind", "url", hash}, {"priority"}) && Locator(value["url"]) && value[hash].is_string() &&
                       ParseSha256(value[hash].get_ref<const std::string &>()).HasValue();
            }
            if (kind == "vendored-artifact" || kind == "vendored") {
                if (!Fields(value, {"kind", "root"}, {"priority"}) || !value["root"].is_string())
                    return false;
                auto path = value["root"].get<std::string>();
                if (path.ends_with('/'))
                    path.pop_back();
                return PackagePath::Parse(path).HasValue();
            }
            return false;
        }

        /** @brief Parses the resolver's existing closed any/exact/caret version model. */
        Result<PackageVersionRange> Range(std::string_view value) {
            if (value == "*")
                return Result<PackageVersionRange>::Success({});
            const bool caret = value.starts_with('^');
            auto version = PackageVersion::Parse(caret ? value.substr(1) : value);
            if (version.HasError())
                return Result<PackageVersionRange>::Failure(version.ErrorValue());
            return Result<PackageVersionRange>::Success(
                {caret ? PackageVersionRange::Kind::Caret : PackageVersionRange::Kind::Exact, std::move(version).Value()});
        }

        /** @brief Canonicalizes a bounded unique token set, rejecting duplicates rather than silently dropping intent. */
        Result<std::vector<std::string>> Tokens(const Json &value) {
            if (!value.is_array() || value.size() > 128)
                return Result<std::vector<std::string>>::Failure(MakeError(Invalid));
            std::vector<std::string> result;
            for (const auto &token : value) {
                if (!token.is_string() || !Detail::IsCanonicalPackageToken(token.get_ref<const std::string &>(), 128))
                    return Result<std::vector<std::string>>::Failure(MakeError(Invalid));
                result.push_back(token.get<std::string>());
            }
            std::ranges::sort(result);
            if (std::adjacent_find(result.begin(), result.end()) != result.end())
                return Result<std::vector<std::string>>::Failure(MakeError(Invalid));
            return Result<std::vector<std::string>>::Success(std::move(result));
        }

        /** @brief Decodes one explicitly source-assigned root and normalizes supported semantic defaults. */
        Result<PortablePackageDependency> Dependency(const std::string &id, Json &value, const Json &sources) {
            if (!Fields(value, {"source", "version"}, {"features", "contributions", "optional", "artifact", "sha256"}) ||
                !value["source"].is_string() || !value["version"].is_string() ||
                (value.contains("optional") && !value["optional"].is_boolean()))
                return Result<PortablePackageDependency>::Failure(MakeError(Invalid));
            auto package = HoroPackageId::Parse(id);
            auto source = HoroPackageSourceId::Parse(value["source"].get_ref<const std::string &>());
            auto range = Range(value["version"].get_ref<const std::string &>());
            if (package.HasError() || source.HasError() || range.HasError() || !sources.contains(source.Value().Value()))
                return Result<PortablePackageDependency>::Failure(MakeError(Invalid));
            auto features = Tokens(value.value("features", Json::array()));
            auto contributions = Tokens(value.value("contributions", Json::array()));
            if (features.HasError() || contributions.HasError())
                return Result<PortablePackageDependency>::Failure(MakeError(Invalid));
            std::optional<Sha256Digest> digest;
            if (value.contains("artifact") || value.contains("sha256")) {
                if (!value.contains("artifact") || !value["artifact"].is_string() ||
                    !PackagePath::Parse(value["artifact"].get<std::string>()).HasValue() || !value.contains("sha256") ||
                    !value["sha256"].is_string())
                    return Result<PortablePackageDependency>::Failure(MakeError(Invalid));
                auto parsed = ParseSha256(value["sha256"].get_ref<const std::string &>());
                if (parsed.HasError())
                    return Result<PortablePackageDependency>::Failure(parsed.ErrorValue());
                digest = parsed.Value();
                value["sha256"] = FormatSha256(*digest);
            }
            const auto kind = sources.at(source.Value().Value()).at("kind").get<std::string>();
            if (kind == "direct-artifact") {
                const auto sourcePin = ParseSha256(sources.at(source.Value().Value()).at("sha256").get_ref<const std::string &>());
                if (sourcePin.HasError() || (digest && *digest != sourcePin.Value()))
                    return Result<PortablePackageDependency>::Failure(MakeError(Invalid));
                digest = sourcePin.Value();
            }
            if ((kind == "vendored" || kind == "vendored-artifact") && !digest)
                return Result<PortablePackageDependency>::Failure(MakeError(Invalid));
            value["features"] = features.Value();
            value["contributions"] = contributions.Value();
            value["optional"] = value.value("optional", false);
            value["version"] = range.Value().ToString();
            return Result<PortablePackageDependency>::Success(
                {{std::move(package).Value(), range.Value(),
                  value["optional"].get<bool>() ? PackageDependencyRequirement::Optional : PackageDependencyRequirement::Required,
                  std::move(features).Value()},
                 std::move(source).Value(),
                 digest,
                 std::move(contributions).Value()});
        }
    }  // namespace

    /** @copydoc ValidatedPackageRequest::Parse */
    Result<ValidatedPackageRequest> ValidatedPackageRequest::Parse(const std::string_view bytes) {
        if (bytes.empty() || bytes.size() > 1024U * 1024U)
            return Result<ValidatedPackageRequest>::Failure(MakeError(Invalid));
        try {
            Detail::PackageJsonGuard guard{128U};
            auto root = Json::parse(bytes, std::ref(guard));
            if (!guard.IsValid() || !Fields(root, {"sources", "dependencies"}, {"schemaVersion"}) || !root["sources"].is_object() ||
                root["sources"].size() > 128 || !root["dependencies"].is_object() || root["dependencies"].size() > 512 ||
                (root.contains("schemaVersion") && (!root["schemaVersion"].is_number_unsigned() || root["schemaVersion"] != 1)))
                return Result<ValidatedPackageRequest>::Failure(MakeError(Invalid));
            for (auto &[id, source] : root["sources"].items()) {
                if (HoroPackageSourceId::Parse(id).HasError() || !Source(source))
                    return Result<ValidatedPackageRequest>::Failure(MakeError(Invalid));
                source["priority"] = source.value("priority", 100U);
                if (source["kind"] == "vendored")
                    source["kind"] = "vendored-artifact";
            }
            std::vector<PortablePackageDependency> dependencies;
            for (auto &[id, value] : root["dependencies"].items()) {
                auto dependency = Dependency(id, value, root["sources"]);
                if (dependency.HasError())
                    return Result<ValidatedPackageRequest>::Failure(dependency.ErrorValue());
                dependencies.push_back(std::move(dependency).Value());
            }
            root["schemaVersion"] = 1U;
            return Result<ValidatedPackageRequest>::Success(ValidatedPackageRequest{std::move(dependencies), root.dump() + '\n'});
        } catch (const Json::exception &) {
            return Result<ValidatedPackageRequest>::Failure(MakeError(Invalid));
        }
    }

    /** @copydoc ValidatedPackageRequest::ValidatedPackageRequest */
    ValidatedPackageRequest::ValidatedPackageRequest(std::vector<PortablePackageDependency> dependencies, std::string canonical)
        : dependencies_(std::move(dependencies)), canonical_(std::move(canonical)),
          digest_(ComputeSha256(std::as_bytes(std::span{canonical_}))) {}

    /** @copydoc ValidatedPackageRequest::Digest */
    const Sha256Digest &ValidatedPackageRequest::Digest() const noexcept {
        return digest_;
    }

    /** @copydoc ValidatedPackageRequest::SerializeCanonical */
    const std::string &ValidatedPackageRequest::SerializeCanonical() const noexcept {
        return canonical_;
    }

    /** @copydoc ValidatedPackageRequest::Dependencies */
    std::span<const PortablePackageDependency> ValidatedPackageRequest::Dependencies() const noexcept {
        return dependencies_;
    }

    /** @copydoc ValidatedPackageRequest::ValidateLock */
    Result<void> ValidatedPackageRequest::ValidateLock(const ValidatedPackageLockfileV1 &lock) const {
        if (lock.RequestHash() != digest_)
            return Result<void>::Failure(MakeError(Stale));
        for (const auto &dependency : dependencies_) {
            const auto found = std::ranges::find(lock.Roots(), dependency.request.package, &LockedPackageReference::package);
            if (found == lock.Roots().end()) {
                if (dependency.request.requirement == PackageDependencyRequirement::Required)
                    return Result<void>::Failure(MakeError(Stale));
                continue;
            }
            const auto package = std::ranges::find(lock.Packages(), found->package, &LockedPackage::package);
            if (package == lock.Packages().end() || !dependency.request.versions.Allows(found->version) ||
                package->source != dependency.source ||
                (dependency.artifactDigest && package->artifactDigest != *dependency.artifactDigest))
                return Result<void>::Failure(MakeError(Stale));
            for (const auto &contribution : dependency.contributions)
                if (std::ranges::find(package->contributions, contribution) == package->contributions.end())
                    return Result<void>::Failure(MakeError(Stale));
        }
        for (const auto &root : lock.Roots())
            if (std::ranges::none_of(dependencies_, [&root](const auto &dependency) {
                return dependency.request.package == root.package;
            }))
                return Result<void>::Failure(MakeError(Stale));
        return Result<void>::Success();
    }
}  // namespace Horo::Packages
