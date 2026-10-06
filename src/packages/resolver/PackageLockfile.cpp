#include "Horo/Packages/PackageLockfile.h"

#include "PackageJsonGuard.h"
#include "PackageValidation.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace Horo::Packages {
    namespace {
        using Json = nlohmann::json;
        using OrderedJson = nlohmann::ordered_json;

        const ErrorDomainId LockDomain{"packages.lockfile"};
        const ErrorCodeDescriptor InvalidLock{LockDomain, ErrorCode{"packages.lockfile.invalid"}, ErrorSeverity::Error,
                                              "Package lockfile is invalid.", "Regenerate the lockfile from verified package metadata."};
        const ErrorCodeDescriptor ResourceLimit{LockDomain, ErrorCode{"packages.lockfile.limit"}, ErrorSeverity::Error,
                                                "Package lockfile exceeds its resource policy.",
                                                "Reduce the resolved package graph or raise an explicit trusted limit."};
        const ErrorCodeDescriptor StaleLock{LockDomain, ErrorCode{"packages.lockfile.stale"}, ErrorSeverity::Error,
                                            "Package lockfile does not match the current dependency request.",
                                            "Resolve dependencies and review the regenerated lockfile."};
        const ErrorCodeDescriptor UnsupportedPlatform{LockDomain, ErrorCode{"packages.lockfile.unsupported_platform"}, ErrorSeverity::Error,
                                                      "A locked package does not support the restore platform.",
                                                      "Select compatible package artifacts before restoring."};
        const ErrorCodeDescriptor UnsupportedFormat{LockDomain, ErrorCode{"packages.lockfile.unsupported_format"}, ErrorSeverity::Error,
                                                    "A locked package uses an unsupported package format.",
                                                    "Restore with a compatible host or resolve a supported package artifact."};

        [[nodiscard]] auto PlatformKey(const PackagePlatform &platform) {
            return std::tie(platform.operatingSystem, platform.architecture, platform.sdkAbi);
        }

        [[nodiscard]] bool PlatformLess(const PackagePlatform &left, const PackagePlatform &right) {
            return PlatformKey(left) < PlatformKey(right);
        }

        [[nodiscard]] bool ReferenceLess(const LockedPackageReference &left, const LockedPackageReference &right) {
            return left.package.Value() < right.package.Value();
        }

        [[nodiscard]] bool PackageLess(const LockedPackage &left, const LockedPackage &right) {
            return left.package.Value() < right.package.Value();
        }

        template <typename Range, typename Less> [[nodiscard]] bool StrictlySorted(const Range &range, Less less) {
            return std::ranges::adjacent_find(range, [&](const auto &left, const auto &right) {
                return !less(left, right);
            }) == std::ranges::end(range);
        }

        [[nodiscard]] bool HasExactKeys(const Json &value, const std::initializer_list<std::string_view> keys) {
            if (!value.is_object() || value.size() != keys.size())
                return false;
            return std::ranges::all_of(keys, [&](const std::string_view key) {
                return value.contains(key);
            });
        }

        [[nodiscard]] Result<LockedPackageReference> DecodeReference(const Json &value) {
            if (!HasExactKeys(value, {"id", "version"}) || !value.at("id").is_string() || !value.at("version").is_string())
                return Result<LockedPackageReference>::Failure(MakeError(InvalidLock));
            auto package = HoroPackageId::Parse(value.at("id").get_ref<const std::string &>());
            auto version = PackageVersion::Parse(value.at("version").get_ref<const std::string &>());
            if (package.HasError() || version.HasError())
                return Result<LockedPackageReference>::Failure(MakeError(InvalidLock));
            return Result<LockedPackageReference>::Success({std::move(package).Value(), std::move(version).Value()});
        }

        [[nodiscard]] Result<PackagePlatform> DecodePlatform(const Json &value) {
            if (!HasExactKeys(value, {"operatingSystem", "architecture", "sdkAbi"}) || !value.at("operatingSystem").is_string() ||
                !value.at("architecture").is_string() || !value.at("sdkAbi").is_string())
                return Result<PackagePlatform>::Failure(MakeError(InvalidLock));
            PackagePlatform platform{value.at("operatingSystem").get<std::string>(), value.at("architecture").get<std::string>(),
                                     value.at("sdkAbi").get<std::string>()};
            if (!Detail::IsValidPackagePlatform(platform))
                return Result<PackagePlatform>::Failure(MakeError(InvalidLock));
            return Result<PackagePlatform>::Success(std::move(platform));
        }

        /** @brief Decodes and validates every bounded platform tuple without publishing partial state. */
        [[nodiscard]] Result<std::vector<PackagePlatform>> DecodePlatforms(const Json &values) {
            std::vector<PackagePlatform> result;
            result.reserve(values.size());
            for (const Json &encoded : values) {
                auto platform = DecodePlatform(encoded);
                if (platform.HasError())
                    return Result<std::vector<PackagePlatform>>::Failure(platform.ErrorValue());
                result.push_back(std::move(platform).Value());
            }
            if (!StrictlySorted(result, PlatformLess))
                return Result<std::vector<PackagePlatform>>::Failure(MakeError(InvalidLock));
            return Result<std::vector<PackagePlatform>>::Success(std::move(result));
        }

        /** @brief Decodes exact-version references and requires canonical package-ID ordering. */
        [[nodiscard]] Result<std::vector<LockedPackageReference>> DecodeReferences(const Json &values) {
            std::vector<LockedPackageReference> result;
            result.reserve(values.size());
            for (const Json &encoded : values) {
                auto reference = DecodeReference(encoded);
                if (reference.HasError())
                    return Result<std::vector<LockedPackageReference>>::Failure(reference.ErrorValue());
                result.push_back(std::move(reference).Value());
            }
            if (!StrictlySorted(result, ReferenceLess))
                return Result<std::vector<LockedPackageReference>>::Failure(MakeError(InvalidLock));
            return Result<std::vector<LockedPackageReference>>::Success(std::move(result));
        }

        /** @brief Decodes canonical contribution identities in stable order. */
        [[nodiscard]] Result<std::vector<std::string>> DecodeContributions(const Json &values) {
            std::vector<std::string> result;
            result.reserve(values.size());
            for (const Json &encoded : values) {
                if (!encoded.is_string() || !Detail::IsCanonicalPackageToken(encoded.get_ref<const std::string &>(), 128U))
                    return Result<std::vector<std::string>>::Failure(MakeError(InvalidLock));
                result.push_back(encoded.get<std::string>());
            }
            if (!StrictlySorted(result, std::less<>{}))
                return Result<std::vector<std::string>>::Failure(MakeError(InvalidLock));
            return Result<std::vector<std::string>>::Success(std::move(result));
        }

        [[nodiscard]] Result<LockedPackage> DecodePackage(const Json &value, const PackageLockfileLimits &limits) {
            if (!HasExactKeys(value, {"id", "version", "source", "artifactSha256", "manifestSha256", "fileManifestSha256",
                                      "packageFormatVersion", "platforms", "dependencies", "contributions"}) ||
                !value.at("id").is_string() || !value.at("version").is_string() || !value.at("source").is_string() ||
                !value.at("artifactSha256").is_string() || !value.at("manifestSha256").is_string() ||
                !value.at("fileManifestSha256").is_string() || !value.at("packageFormatVersion").is_number_unsigned() ||
                !value.at("platforms").is_array() || !value.at("dependencies").is_array() || !value.at("contributions").is_array())
                return Result<LockedPackage>::Failure(MakeError(InvalidLock));
            if (value.at("platforms").size() > limits.platformsPerPackage ||
                value.at("dependencies").size() > limits.dependenciesPerPackage ||
                value.at("contributions").size() > limits.contributionsPerPackage)
                return Result<LockedPackage>::Failure(MakeError(ResourceLimit));

            auto package = HoroPackageId::Parse(value.at("id").get_ref<const std::string &>());
            auto version = PackageVersion::Parse(value.at("version").get_ref<const std::string &>());
            auto source = HoroPackageSourceId::Parse(value.at("source").get_ref<const std::string &>());
            auto artifact = ParseSha256(value.at("artifactSha256").get_ref<const std::string &>());
            auto manifest = ParseSha256(value.at("manifestSha256").get_ref<const std::string &>());
            auto fileManifest = ParseSha256(value.at("fileManifestSha256").get_ref<const std::string &>());
            auto platforms = DecodePlatforms(value.at("platforms"));
            auto dependencies = DecodeReferences(value.at("dependencies"));
            auto contributions = DecodeContributions(value.at("contributions"));
            const auto format = value.at("packageFormatVersion").get<std::uint64_t>();
            if (package.HasError() || version.HasError() || source.HasError() || artifact.HasError() || manifest.HasError() ||
                fileManifest.HasError() || platforms.HasError() || dependencies.HasError() || contributions.HasError() || format == 0U ||
                format > std::numeric_limits<std::uint32_t>::max())
                return Result<LockedPackage>::Failure(MakeError(InvalidLock));

            LockedPackage result{std::move(package).Value(),
                                 std::move(version).Value(),
                                 std::move(source).Value(),
                                 artifact.Value(),
                                 manifest.Value(),
                                 fileManifest.Value(),
                                 static_cast<std::uint32_t>(format),
                                 std::move(platforms).Value(),
                                 std::move(dependencies).Value(),
                                 std::move(contributions).Value()};
            return Result<LockedPackage>::Success(std::move(result));
        }

        [[nodiscard]] Result<void> ValidateGraph(const std::vector<LockedPackageReference> &roots,
                                                 const std::vector<LockedPackage> &packages) {
            if (roots.empty() || packages.empty() || !StrictlySorted(roots, ReferenceLess) || !StrictlySorted(packages, PackageLess))
                return Result<void>::Failure(MakeError(InvalidLock));
            std::map<std::string_view, std::size_t, std::less<>> indices;
            for (std::size_t index = 0; index < packages.size(); ++index)
                indices.try_emplace(packages[index].package.Value(), index);

            auto resolve = [&](const LockedPackageReference &reference) -> std::optional<std::size_t> {
                const auto found = indices.find(reference.package.Value());
                if (found == indices.end() || packages[found->second].version != reference.version)
                    return std::nullopt;
                return found->second;
            };
            for (const LockedPackage &package : packages) {
                if (std::ranges::any_of(package.dependencies, [&](const LockedPackageReference &dependency) {
                    return !resolve(dependency).has_value();
                }))
                    return Result<void>::Failure(MakeError(InvalidLock, "Lockfile contains a stale dependency edge."));
            }

            enum class Mark : std::uint8_t {
                Visiting,
                Visited
            };
            using enum Mark;
            std::vector<std::optional<Mark>> marks(packages.size());
            std::size_t visited{};
            std::function<bool(std::size_t)> visit = [&](const std::size_t index) {
                if (marks[index].has_value())
                    return *marks[index] == Visited;
                marks[index] = Visiting;
                for (const auto &dependency : packages[index].dependencies) {
                    const auto child = resolve(dependency);
                    if (!child.has_value() || (marks[*child] == Visiting) || !visit(*child))
                        return false;
                }
                marks[index] = Visited;
                ++visited;
                return true;
            };
            for (const auto &root : roots) {
                const auto index = resolve(root);
                if (!index.has_value() || !visit(*index))
                    return Result<void>::Failure(MakeError(InvalidLock, "Lockfile graph root is stale or cyclic."));
            }
            if (visited != packages.size())
                return Result<void>::Failure(MakeError(InvalidLock, "Lockfile contains a stale unreachable entry."));
            return Result<void>::Success();
        }

        using ResolvedIndex = std::map<std::string_view, const ResolvedPackage *, std::less<>>;
        using ArtifactIndex = std::map<std::string_view, const PackageLockArtifact *, std::less<>>;

        [[nodiscard]] Result<ResolvedIndex> IndexResolvedPackages(const PackageResolutionPlan &plan, const PackageLockfileLimits &limits) {
            ResolvedIndex result;
            for (const auto &package : plan.packages) {
                if (!result.try_emplace(package.package.Value(), &package).second ||
                    package.dependencies.size() > limits.dependenciesPerPackage)
                    return Result<ResolvedIndex>::Failure(MakeError(InvalidLock));
            }
            return Result<ResolvedIndex>::Success(std::move(result));
        }

        [[nodiscard]] Result<ArtifactIndex> IndexArtifacts(const std::span<const PackageLockArtifact> artifacts,
                                                           const PackageLockfileLimits &limits) {
            ArtifactIndex result;
            for (const auto &artifact : artifacts) {
                if (!result.try_emplace(artifact.package.Value(), &artifact).second)
                    return Result<ArtifactIndex>::Failure(MakeError(InvalidLock));
                if (artifact.platforms.size() > limits.platformsPerPackage ||
                    artifact.contributions.size() > limits.contributionsPerPackage)
                    return Result<ArtifactIndex>::Failure(MakeError(ResourceLimit));
            }
            return Result<ArtifactIndex>::Success(std::move(result));
        }

        [[nodiscard]] Result<LockedPackage> BuildLockedPackage(const ResolvedPackage &package, const PackageLockArtifact &artifact,
                                                               const ResolvedIndex &resolved) {
            if (artifact.version != package.version || artifact.source != package.source ||
                artifact.artifactDigest != package.artifactDigest || artifact.packageFormatVersion == 0U ||
                !std::ranges::all_of(artifact.platforms, Detail::IsValidPackagePlatform) ||
                !std::ranges::all_of(artifact.contributions, [](const std::string &value) {
                return Detail::IsCanonicalPackageToken(value, 128U);
            }))
                return Result<LockedPackage>::Failure(MakeError(InvalidLock, "Artifact evidence does not match resolution."));

            LockedPackage entry{package.package,
                                package.version,
                                package.source,
                                package.artifactDigest,
                                artifact.manifestDigest,
                                artifact.fileManifestDigest,
                                artifact.packageFormatVersion,
                                artifact.platforms,
                                {},
                                artifact.contributions};
            for (const HoroPackageId &dependency : package.dependencies) {
                const auto selected = resolved.find(dependency.Value());
                if (selected == resolved.end())
                    return Result<LockedPackage>::Failure(MakeError(InvalidLock, "Resolution has a stale dependency edge."));
                entry.dependencies.emplace_back(dependency, selected->second->version);
            }
            std::ranges::sort(entry.platforms, PlatformLess);
            std::ranges::sort(entry.dependencies, ReferenceLess);
            std::ranges::sort(entry.contributions);
            if (!StrictlySorted(entry.platforms, PlatformLess) || !StrictlySorted(entry.dependencies, ReferenceLess) ||
                !StrictlySorted(entry.contributions, std::less<>{}))
                return Result<LockedPackage>::Failure(MakeError(InvalidLock));
            return Result<LockedPackage>::Success(std::move(entry));
        }

        [[nodiscard]] Result<std::vector<LockedPackage>> BuildLockedPackages(const ResolvedIndex &resolved,
                                                                             const ArtifactIndex &artifacts) {
            std::vector<LockedPackage> result;
            result.reserve(resolved.size());
            for (const auto &[id, package] : resolved) {
                const auto evidence = artifacts.find(id);
                if (evidence == artifacts.end())
                    return Result<std::vector<LockedPackage>>::Failure(MakeError(InvalidLock, "Artifact evidence is incomplete."));
                auto entry = BuildLockedPackage(*package, *evidence->second, resolved);
                if (entry.HasError())
                    return Result<std::vector<LockedPackage>>::Failure(entry.ErrorValue());
                result.push_back(std::move(entry).Value());
            }
            return Result<std::vector<LockedPackage>>::Success(std::move(result));
        }

        [[nodiscard]] Result<std::vector<LockedPackageReference>> BuildLockedRoots(const std::span<const HoroPackageId> roots,
                                                                                   const ResolvedIndex &resolved) {
            std::vector<LockedPackageReference> result;
            result.reserve(roots.size());
            for (const HoroPackageId &root : roots) {
                const auto selected = resolved.find(root.Value());
                if (selected == resolved.end())
                    return Result<std::vector<LockedPackageReference>>::Failure(
                        MakeError(InvalidLock, "Requested root is absent from resolution."));
                result.emplace_back(root, selected->second->version);
            }
            std::ranges::sort(result, ReferenceLess);
            return Result<std::vector<LockedPackageReference>>::Success(std::move(result));
        }

        [[nodiscard]] OrderedJson EncodeReference(const LockedPackageReference &reference) {
            return OrderedJson{{"id", reference.package.Value()}, {"version", reference.version.ToString()}};
        }

        [[nodiscard]] OrderedJson EncodePackage(const LockedPackage &package) {
            OrderedJson platforms = OrderedJson::array();
            for (const auto &platform : package.platforms)
                platforms.push_back(
                    {{"operatingSystem", platform.operatingSystem}, {"architecture", platform.architecture}, {"sdkAbi", platform.sdkAbi}});
            OrderedJson dependencies = OrderedJson::array();
            for (const auto &dependency : package.dependencies)
                dependencies.push_back(EncodeReference(dependency));
            return OrderedJson{{"id", package.package.Value()},
                               {"version", package.version.ToString()},
                               {"source", package.source.Value()},
                               {"artifactSha256", FormatSha256(package.artifactDigest)},
                               {"manifestSha256", FormatSha256(package.manifestDigest)},
                               {"fileManifestSha256", FormatSha256(package.fileManifestDigest)},
                               {"packageFormatVersion", package.packageFormatVersion},
                               {"platforms", std::move(platforms)},
                               {"dependencies", std::move(dependencies)},
                               {"contributions", package.contributions}};
        }
    }  // namespace

    /** @copydoc ValidatedPackageLockfileV1::Generate */
    Result<ValidatedPackageLockfileV1> ValidatedPackageLockfileV1::Generate(const PackageResolutionPlan &plan,
                                                                            const std::span<const HoroPackageId> roots,
                                                                            const Sha256Digest &requestHash,
                                                                            const std::span<const PackageLockArtifact> artifacts,
                                                                            const PackageLockfileLimits &limits) {
        if (plan.packages.empty() || roots.empty() || artifacts.size() != plan.packages.size())
            return Result<ValidatedPackageLockfileV1>::Failure(MakeError(InvalidLock));
        if (plan.packages.size() > limits.packages || roots.size() > limits.roots)
            return Result<ValidatedPackageLockfileV1>::Failure(MakeError(ResourceLimit));

        auto resolved = IndexResolvedPackages(plan, limits);
        auto evidence = IndexArtifacts(artifacts, limits);
        if (resolved.HasError())
            return Result<ValidatedPackageLockfileV1>::Failure(resolved.ErrorValue());
        if (evidence.HasError())
            return Result<ValidatedPackageLockfileV1>::Failure(evidence.ErrorValue());
        auto locked = BuildLockedPackages(resolved.Value(), evidence.Value());
        auto lockedRoots = BuildLockedRoots(roots, resolved.Value());
        if (locked.HasError())
            return Result<ValidatedPackageLockfileV1>::Failure(locked.ErrorValue());
        if (lockedRoots.HasError())
            return Result<ValidatedPackageLockfileV1>::Failure(lockedRoots.ErrorValue());
        if (const auto graph = ValidateGraph(lockedRoots.Value(), locked.Value()); graph.HasError())
            return Result<ValidatedPackageLockfileV1>::Failure(graph.ErrorValue());
        ValidatedPackageLockfileV1 result{requestHash, std::move(lockedRoots).Value(), std::move(locked).Value()};
        if (result.SerializeCanonical().size() > limits.documentBytes)
            return Result<ValidatedPackageLockfileV1>::Failure(MakeError(ResourceLimit));
        return Result<ValidatedPackageLockfileV1>::Success(std::move(result));
    }

    /** @copydoc ValidatedPackageLockfileV1::Parse */
    Result<ValidatedPackageLockfileV1> ValidatedPackageLockfileV1::Parse(const std::string_view json, const PackageLockfileLimits &limits) {
        if (json.size() > limits.documentBytes)
            return Result<ValidatedPackageLockfileV1>::Failure(MakeError(ResourceLimit));
        try {
            Detail::PackageJsonGuard guard;
            const Json root = Json::parse(json, std::ref(guard));
            if (!guard.IsValid() || !HasExactKeys(root, {"schemaVersion", "requestHash", "roots", "packages"}) ||
                !root.at("schemaVersion").is_number_unsigned() || root.at("schemaVersion") != 1U || !root.at("requestHash").is_string() ||
                !root.at("roots").is_array() || !root.at("packages").is_array())
                return Result<ValidatedPackageLockfileV1>::Failure(MakeError(InvalidLock));
            if (root.at("roots").empty() || root.at("roots").size() > limits.roots || root.at("packages").empty() ||
                root.at("packages").size() > limits.packages)
                return Result<ValidatedPackageLockfileV1>::Failure(MakeError(ResourceLimit));
            auto requestHash = ParseSha256(root.at("requestHash").get_ref<const std::string &>());
            if (requestHash.HasError())
                return Result<ValidatedPackageLockfileV1>::Failure(MakeError(InvalidLock));

            std::vector<LockedPackageReference> roots;
            roots.reserve(root.at("roots").size());
            for (const Json &value : root.at("roots")) {
                auto reference = DecodeReference(value);
                if (reference.HasError())
                    return Result<ValidatedPackageLockfileV1>::Failure(reference.ErrorValue());
                roots.push_back(std::move(reference).Value());
            }
            std::vector<LockedPackage> packages;
            packages.reserve(root.at("packages").size());
            for (const Json &value : root.at("packages")) {
                auto package = DecodePackage(value, limits);
                if (package.HasError())
                    return Result<ValidatedPackageLockfileV1>::Failure(package.ErrorValue());
                packages.push_back(std::move(package).Value());
            }
            if (const auto graph = ValidateGraph(roots, packages); graph.HasError())
                return Result<ValidatedPackageLockfileV1>::Failure(graph.ErrorValue());
            return Result<ValidatedPackageLockfileV1>::Success(
                ValidatedPackageLockfileV1{requestHash.Value(), std::move(roots), std::move(packages)});
        } catch (const Json::exception &) {
            return Result<ValidatedPackageLockfileV1>::Failure(MakeError(InvalidLock));
        }
    }

    /** @copydoc ValidatedPackageLockfileV1::ValidateForRestore */
    Result<void> ValidatedPackageLockfileV1::ValidateForRestore(const Sha256Digest &expectedRequestHash, const PackagePlatform &platform,
                                                                const std::uint32_t supportedPackageFormatVersion) const {
        if (expectedRequestHash != m_requestHash)
            return Result<void>::Failure(MakeError(StaleLock));
        if (!Detail::IsValidPackagePlatform(platform))
            return Result<void>::Failure(MakeError(InvalidLock, "Restore platform is not canonical."));
        for (const LockedPackage &package : m_packages) {
            if (package.packageFormatVersion != supportedPackageFormatVersion)
                return Result<void>::Failure(MakeError(UnsupportedFormat, "Unsupported package: " + package.package.Value()));
            if (!package.platforms.empty() && std::ranges::find(package.platforms, platform) == package.platforms.end())
                return Result<void>::Failure(MakeError(UnsupportedPlatform, "Unsupported package: " + package.package.Value()));
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidatedPackageLockfileV1::SerializeCanonical */
    std::string ValidatedPackageLockfileV1::SerializeCanonical() const {
        OrderedJson roots = OrderedJson::array();
        for (const auto &root : m_roots)
            roots.push_back(EncodeReference(root));
        OrderedJson packages = OrderedJson::array();
        for (const auto &package : m_packages)
            packages.push_back(EncodePackage(package));
        return OrderedJson{{"schemaVersion", 1U},
                           {"requestHash", FormatSha256(m_requestHash)},
                           {"roots", std::move(roots)},
                           {"packages", std::move(packages)}}
                   .dump(2) +
               '\n';
    }

    /** @copydoc ValidatedPackageLockfileV1::RequestHash */
    const Sha256Digest &ValidatedPackageLockfileV1::RequestHash() const noexcept {
        return m_requestHash;
    }

    /** @copydoc ValidatedPackageLockfileV1::Roots */
    std::span<const LockedPackageReference> ValidatedPackageLockfileV1::Roots() const noexcept {
        return m_roots;
    }

    /** @copydoc ValidatedPackageLockfileV1::Packages */
    std::span<const LockedPackage> ValidatedPackageLockfileV1::Packages() const noexcept {
        return m_packages;
    }

    /** @copydoc ValidatedPackageLockfileV1::ValidatedPackageLockfileV1 */
    ValidatedPackageLockfileV1::ValidatedPackageLockfileV1(const Sha256Digest &requestHash, std::vector<LockedPackageReference> roots,
                                                           std::vector<LockedPackage> packages)
        : m_requestHash(requestHash), m_roots(std::move(roots)), m_packages(std::move(packages)) {}
}  // namespace Horo::Packages
