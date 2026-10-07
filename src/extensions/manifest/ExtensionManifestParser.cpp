#include "ExtensionManifestParsing.h"
#include "ExtensionManifestValidation.h"
#include "Horo/Extensions/ExtensionManifest.h"

#include <array>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace Horo::Extensions {
    namespace {
        using namespace ManifestParsing;
        using namespace ManifestValidation;

        class ManifestValidator final : private ManifestReader {
        public:
            explicit ManifestValidator(const ExtensionManifestLimits &limits) : ManifestReader(limits) {}

            [[nodiscard]] Result<ExtensionManifest> Validate(const Json &document) {
                if (!document.is_object())
                    return Failure("$", "extension.manifest.invalid_type", "Manifest root must be an object.");
                ExtensionManifest manifest;
                if (!ValidateRootFields(document) || !ValidateSchemaVersion(document, manifest.schemaVersion))
                    return CurrentFailure();

                const Json *package = SelectPackage(document);
                if (package == nullptr)
                    return CurrentFailure();
                if (const std::string packagePath = document.contains("package") ? "$.package" : "$";
                    !ParseManifestSections(document, *package, packagePath, manifest))
                    return CurrentFailure();
                return Result<ExtensionManifest>::Success(std::move(manifest));
            }

        private:
            [[nodiscard]] bool ParsePackage(const Json &package, const std::string_view path, ExtensionManifest &manifest) {
                if (!ReadPackageIdentity(package, path, manifest))
                    return false;
                if (!IsCanonicalSemanticVersion(manifest.version))
                    return Reject(ChildPath(path, "version"), "extension.manifest.invalid_version",
                                  "Version must be canonical semantic version text.");
                if (!ReadPackageMetadata(package, path, manifest))
                    return false;
                return manifest.kind.empty() || IsCanonicalToken(manifest.kind, Limits().maximumIdentifierBytes) ||
                       Reject(ChildPath(path, "kind"), "extension.manifest.invalid_value", "Package kind is not canonical.");
            }

            [[nodiscard]] bool ReadPackageIdentity(const Json &package, const std::string_view path, ExtensionManifest &manifest) {
                return ReadId(package, "id", path, manifest.id) &&
                       ReadString(package, "version", path, manifest.version, MaximumSemanticVersionBytes, true);
            }

            [[nodiscard]] bool ReadPackageMetadata(const Json &package, const std::string_view path, ExtensionManifest &manifest) {
                return ReadString(package, "kind", path, manifest.kind, Limits().maximumIdentifierBytes, false) &&
                       ReadString(package, "displayName", path, manifest.displayName, Limits().maximumStringBytes, false) &&
                       ReadString(package, "description", path, manifest.description, Limits().maximumStringBytes, false) &&
                       ReadString(package, "author", path, manifest.author, Limits().maximumStringBytes, false);
            }

            [[nodiscard]] bool ParseManifestSections(const Json &document, const Json &package, const std::string_view packagePath,
                                                     ExtensionManifest &manifest) {
                if (!ParsePackage(package, packagePath, manifest))
                    return false;
                if (!ParseCompatibility(document, manifest))
                    return false;
                if (!ParseModules(document, manifest))
                    return false;
                if (!ValidateCompatibilityAuthority(manifest))
                    return false;
                return ParseContributions(document, manifest);
            }

            [[nodiscard]] bool ValidateCompatibilityAuthority(const ExtensionManifest &manifest) {
                if (const bool hasTypedAbi = std::ranges::any_of(manifest.modules,
                                                                 [](const ExtensionModuleManifest &moduleManifest) {
                    return moduleManifest.abi.has_value();
                });
                    !manifest.sdkAbi.empty() && hasTypedAbi)
                    return Reject("$.compatibility.sdkAbi", "extension.manifest.ambiguous_field",
                                  "Legacy SDK ABI and typed module ABI requirements cannot both be declared.");
                const bool hasTypedEntries = std::ranges::any_of(manifest.modules, [](const ExtensionModuleManifest &moduleManifest) {
                    return !moduleManifest.entries.empty();
                });
                return manifest.platforms.empty() || !hasTypedEntries ||
                       Reject("$.compatibility.platforms", "extension.manifest.ambiguous_field",
                              "Legacy platforms and typed module entries cannot both be declared.");
            }

            [[nodiscard]] bool ParseCompatibility(const Json &document, ExtensionManifest &manifest) {
                const auto found = document.find("compatibility");
                if (found == document.end())
                    return true;
                if (!found->is_object())
                    return Reject("$.compatibility", "extension.manifest.invalid_type", "Compatibility field must be an object.");
                if (!ReadCompatibilityFields(*found, manifest))
                    return false;
                if (!ValidateCompatibilityValues(manifest))
                    return false;
                return ParsePlatforms(*found, manifest.platforms);
            }

            [[nodiscard]] bool ReadCompatibilityFields(const Json &compatibility, ExtensionManifest &manifest) {
                return AllowFields(compatibility, "$.compatibility", {"engineMin", "engineMax", "sdkAbi", "platforms"}) &&
                       ReadString(compatibility, "engineMin", "$.compatibility", manifest.engineMin, MaximumSemanticVersionBytes, false) &&
                       ReadString(compatibility, "engineMax", "$.compatibility", manifest.engineMax, MaximumSemanticVersionBytes, false) &&
                       ReadString(compatibility, "sdkAbi", "$.compatibility", manifest.sdkAbi, Limits().maximumIdentifierBytes, false);
            }

            [[nodiscard]] bool ValidateCompatibilityValues(const ExtensionManifest &manifest) {
                const bool invalidMinimum = !manifest.engineMin.empty() && !IsCanonicalSemanticVersion(manifest.engineMin);
                if (const bool invalidMaximum = !manifest.engineMax.empty() && !IsCanonicalSemanticVersion(manifest.engineMax);
                    invalidMinimum || invalidMaximum) {
                    return Reject("$.compatibility", "extension.manifest.invalid_version",
                                  "Engine compatibility values must be canonical semantic versions.");
                }
                if (!manifest.sdkAbi.empty() && !IsCanonicalId(manifest.sdkAbi, Limits().maximumIdentifierBytes))
                    return Reject("$.compatibility.sdkAbi", "extension.manifest.invalid_identifier", "SDK ABI ID is not canonical.");
                return true;
            }

            [[nodiscard]] bool ParsePlatforms(const Json &compatibility, std::vector<std::string> &platforms) {
                const auto found = compatibility.find("platforms");
                if (found == compatibility.end())
                    return true;
                if (!found->is_array())
                    return Reject("$.compatibility.platforms", "extension.manifest.invalid_type", "Platforms must be an array.");
                if (found->size() > Limits().maximumPlatforms)
                    return Reject("$.compatibility.platforms", "extension.manifest.collection_limit",
                                  "Platform count exceeds the configured limit.");
                std::set<std::string, std::less<>> identities;
                platforms.reserve(found->size());
                for (std::size_t index = 0; index < found->size(); ++index) {
                    const std::string path = ElementPath("$.compatibility.platforms", index);
                    if (!AppendPlatform((*found)[index], path, identities, platforms))
                        return false;
                }
                return true;
            }

            [[nodiscard]] bool AppendPlatform(const Json &value, const std::string_view path,
                                              std::set<std::string, std::less<>> &identities, std::vector<std::string> &platforms) {
                if (!value.is_string())
                    return Reject(path, "extension.manifest.invalid_type", "Platform ID must be a string.");
                std::string platform = value.get<std::string>();
                if (!IsCanonicalToken(platform, Limits().maximumIdentifierBytes))
                    return Reject(path, "extension.manifest.invalid_identifier", "Platform ID is not canonical.");
                if (!identities.insert(platform).second)
                    return Reject(path, "extension.manifest.duplicate_identifier", "Platform ID must be unique.");
                platforms.push_back(std::move(platform));
                return true;
            }

            [[nodiscard]] bool ParseModules(const Json &document, ExtensionManifest &manifest) {
                const auto found = document.find("modules");
                if (found == document.end())
                    return Reject("$.modules", "extension.manifest.missing_field", "At least one explicit module is required.");
                if (!found->is_array())
                    return Reject("$.modules", "extension.manifest.invalid_type", "Modules must be an array.");
                if (found->empty() || found->size() > Limits().maximumModules)
                    return Reject("$.modules", "extension.manifest.collection_limit", "Module count must be within configured limits.");

                std::set<std::string, std::less<>> identities;
                manifest.modules.reserve(found->size());
                for (std::size_t index = 0; index < found->size(); ++index) {
                    const std::string path = ElementPath("$.modules", index);
                    if (!AppendModule((*found)[index], path, identities, manifest.modules))
                        return false;
                }
                return true;
            }

            [[nodiscard]] bool AppendModule(const Json &value, const std::string_view path, std::set<std::string, std::less<>> &identities,
                                            std::vector<ExtensionModuleManifest> &modules) {
                ExtensionModuleManifest moduleManifest;
                if (!ParseModule(value, path, moduleManifest))
                    return false;
                if (!identities.insert(moduleManifest.id).second)
                    return Reject(ChildPath(path, "id"), "extension.manifest.duplicate_identifier",
                                  "Module ID must be unique within the package.");
                modules.push_back(std::move(moduleManifest));
                return true;
            }

            [[nodiscard]] bool ParseModule(const Json &value, const std::string_view path, ExtensionModuleManifest &moduleManifest) {
                if (!value.is_object())
                    return Reject(path, "extension.manifest.invalid_type", "Module must be an object.");
                if (!AllowFields(value, path,
                                 {"id", "version", "kind", "entry", "roles", "dependencies", "exports", "imports", "abi", "entries",
                                  "requiredCapabilities"}))
                    return false;
                return ReadModuleFields(value, path, moduleManifest) && ValidateModuleValues(path, moduleManifest) &&
                       ParseModuleRelationships(value, path, moduleManifest) && ParseModuleCompatibility(value, path, moduleManifest);
            }

            [[nodiscard]] bool ParseModuleRelationships(const Json &value, const std::string_view path,
                                                        ExtensionModuleManifest &moduleManifest) {
                return ParseModuleRoles(value, path, moduleManifest.roles) &&
                       ParseModuleDependencies(value, path, moduleManifest.dependencies) &&
                       ParseModuleExports(value, path, moduleManifest.exports) && ParseModuleImports(value, path, moduleManifest.imports);
            }

            [[nodiscard]] bool ParseModuleCompatibility(const Json &value, const std::string_view path,
                                                        ExtensionModuleManifest &moduleManifest) {
                return ParseModuleAbi(value, path, moduleManifest.abi) &&
                       ParseModuleEntries(value, path, moduleManifest.entry, moduleManifest.entries) &&
                       ParseModuleCapabilities(value, path, moduleManifest.requiredCapabilities);
            }

            [[nodiscard]] bool ReadModuleFields(const Json &value, const std::string_view path, ExtensionModuleManifest &moduleManifest) {
                return ReadId(value, "id", path, moduleManifest.id) &&
                       ReadString(value, "version", path, moduleManifest.version, MaximumSemanticVersionBytes, true) &&
                       ReadString(value, "kind", path, moduleManifest.kind, Limits().maximumIdentifierBytes, true) &&
                       ReadString(value, "entry", path, moduleManifest.entry, MaximumEntryBytes, false);
            }

            [[nodiscard]] bool ValidateModuleValues(const std::string_view path, const ExtensionModuleManifest &moduleManifest) {
                if (!IsCanonicalSemanticVersion(moduleManifest.version))
                    return Reject(ChildPath(path, "version"), "extension.manifest.invalid_version",
                                  "Module version must be canonical semantic version text.");
                if (!IsCanonicalToken(moduleManifest.kind, Limits().maximumIdentifierBytes))
                    return Reject(ChildPath(path, "kind"), "extension.manifest.invalid_value", "Module kind is not canonical.");
                if (!moduleManifest.entry.empty() && !IsSafeEntry(moduleManifest.entry))
                    return Reject(ChildPath(path, "entry"), "extension.manifest.invalid_path",
                                  "Module entry must be a safe package-relative path.");
                return true;
            }

            [[nodiscard]] bool ParseModuleAbi(const Json &moduleObject, const std::string_view path,
                                              std::optional<ExtensionAbiRequirement> &requirement) {
                const auto found = moduleObject.find("abi");
                if (found == moduleObject.end())
                    return true;
                const std::string abiPath = ChildPath(path, "abi");
                if (!found->is_object())
                    return Reject(abiPath, "extension.manifest.invalid_type", "Module ABI requirement must be an object.");
                if (!AllowFields(*found, abiPath, {"major", "minimumMinor"}))
                    return false;
                const auto major = found->find("major");
                const auto minimumMinor = found->find("minimumMinor");
                if (major == found->end() || minimumMinor == found->end())
                    return Reject(abiPath, "extension.manifest.missing_field", "Module ABI major and minimumMinor are required.");
                return AssignAbiRequirement(*major, *minimumMinor, abiPath, requirement);
            }

            [[nodiscard]] bool AssignAbiRequirement(const Json &major, const Json &minimumMinor, const std::string_view abiPath,
                                                    std::optional<ExtensionAbiRequirement> &requirement) {
                if (!major.is_number_unsigned() || !minimumMinor.is_number_unsigned())
                    return Reject(abiPath, "extension.manifest.invalid_type", "Module ABI versions must be unsigned integers.");
                const std::uint64_t majorValue = major.get<std::uint64_t>();
                const std::uint64_t minorValue = minimumMinor.get<std::uint64_t>();
                if (majorValue == 0 || majorValue > std::numeric_limits<std::uint32_t>::max() ||
                    minorValue > std::numeric_limits<std::uint32_t>::max())
                    return Reject(abiPath, "extension.manifest.invalid_value", "Module ABI versions are outside supported bounds.");
                requirement = ExtensionAbiRequirement{static_cast<std::uint32_t>(majorValue), static_cast<std::uint32_t>(minorValue)};
                return true;
            }

            [[nodiscard]] bool ParseModuleEntry(const Json &encoded, const std::string_view path, ExtensionNativeEntryManifest &entry) {
                if (!encoded.is_object())
                    return Reject(path, "extension.manifest.invalid_type", "Module entry variant must be an object.");
                if (!AllowFields(encoded, path, {"platform", "architecture", "buildProfile", "entry"}))
                    return false;
                std::string platform;
                std::string architecture;
                std::string buildProfile;
                if (!ReadModuleEntryFields(encoded, path, platform, architecture, buildProfile, entry.entry))
                    return false;
                return AssignModuleEntrySelectors(platform, architecture, buildProfile, path, entry);
            }

            [[nodiscard]] bool ReadModuleEntryFields(const Json &encoded, const std::string_view path, std::string &platform,
                                                     std::string &architecture, std::string &buildProfile, std::string &entry) {
                return ReadString(encoded, "platform", path, platform, Limits().maximumIdentifierBytes, true) &&
                       ReadString(encoded, "architecture", path, architecture, Limits().maximumIdentifierBytes, true) &&
                       ReadString(encoded, "buildProfile", path, buildProfile, Limits().maximumIdentifierBytes, true) &&
                       ReadString(encoded, "entry", path, entry, MaximumEntryBytes, true);
            }

            [[nodiscard]] bool AssignModuleEntrySelectors(const std::string_view platform, const std::string_view architecture,
                                                          const std::string_view buildProfile, const std::string_view path,
                                                          ExtensionNativeEntryManifest &entry) {
                const auto parsedPlatform = ParseHostPlatform(platform);
                const auto parsedArchitecture = ParseHostArchitecture(architecture);
                const auto parsedBuildProfile = ParseBuildProfile(buildProfile);
                if (!parsedPlatform || !parsedArchitecture || !parsedBuildProfile)
                    return Reject(path, "extension.manifest.invalid_value", "Module entry selector is not recognized.");
                if (!IsSafeEntry(entry.entry))
                    return Reject(ChildPath(path, "entry"), "extension.manifest.invalid_path",
                                  "Module entry must be a safe package-relative path.");
                entry.platform = *parsedPlatform;
                entry.architecture = *parsedArchitecture;
                entry.buildProfile = *parsedBuildProfile;
                return true;
            }

            [[nodiscard]] bool ParseModuleEntries(const Json &moduleObject, const std::string_view path, const std::string_view legacyEntry,
                                                  std::vector<ExtensionNativeEntryManifest> &entries) {
                const auto found = moduleObject.find("entries");
                if (found == moduleObject.end())
                    return true;
                const std::string entriesPath = ChildPath(path, "entries");
                if (!legacyEntry.empty())
                    return Reject(entriesPath, "extension.manifest.ambiguous_field",
                                  "Legacy entry and typed entries cannot both be declared.");
                if (!found->is_array())
                    return Reject(entriesPath, "extension.manifest.invalid_type", "Module entries must be an array.");
                if (found->empty() || found->size() > Limits().maximumPlatforms)
                    return Reject(entriesPath, "extension.manifest.collection_limit", "Module entry count is outside its limit.");
                std::set<std::tuple<ExtensionHostPlatform, ExtensionHostArchitecture, ExtensionBuildProfile>> selectors;
                entries.reserve(found->size());
                for (std::size_t index = 0; index < found->size(); ++index) {
                    ExtensionNativeEntryManifest entry;
                    const std::string elementPath = ElementPath(entriesPath, index);
                    if (!ParseModuleEntry((*found)[index], elementPath, entry))
                        return false;
                    if (!selectors.emplace(entry.platform, entry.architecture, entry.buildProfile).second)
                        return Reject(elementPath, "extension.manifest.duplicate_identifier", "Module entry selector must be unique.");
                    entries.push_back(std::move(entry));
                }
                return true;
            }

            [[nodiscard]] bool ParseModuleCapabilities(const Json &moduleObject, const std::string_view path,
                                                       std::vector<std::string> &capabilities) {
                const auto found = moduleObject.find("requiredCapabilities");
                if (found == moduleObject.end())
                    return true;
                const std::string capabilitiesPath = ChildPath(path, "requiredCapabilities");
                if (!found->is_array())
                    return Reject(capabilitiesPath, "extension.manifest.invalid_type", "Required capabilities must be an array.");
                if (found->size() > Limits().maximumContributions)
                    return Reject(capabilitiesPath, "extension.manifest.collection_limit", "Required capability count exceeds its limit.");
                std::set<std::string, std::less<>> identities;
                for (std::size_t index = 0; index < found->size(); ++index) {
                    const std::string elementPath = ElementPath(capabilitiesPath, index);
                    const Json &encoded = (*found)[index];
                    if (!encoded.is_string())
                        return Reject(elementPath, "extension.manifest.invalid_type", "Capability ID must be a string.");
                    std::string capability = encoded.get<std::string>();
                    if (!IsCanonicalId(capability, Limits().maximumIdentifierBytes))
                        return Reject(elementPath, "extension.manifest.invalid_identifier", "Capability ID is not canonical.");
                    if (!identities.insert(capability).second)
                        return Reject(elementPath, "extension.manifest.duplicate_identifier", "Capability ID must be unique.");
                    capabilities.push_back(std::move(capability));
                }
                return true;
            }

            [[nodiscard]] bool ParseModuleRoles(const Json &moduleObject, const std::string_view path,
                                                std::vector<ExtensionModuleRole> &roles) {
                const auto found = moduleObject.find("roles");
                const std::string rolesPath = ChildPath(path, "roles");
                if (found == moduleObject.end())
                    return true;
                if (!found->is_array())
                    return Reject(rolesPath, "extension.manifest.invalid_type", "Module roles must be an array.");
                if (found->empty() || found->size() > 5)
                    return Reject(rolesPath, "extension.manifest.collection_limit", "Module role count must be between one and five.");
                for (std::size_t index = 0; index < found->size(); ++index) {
                    const Json &encoded = (*found)[index];
                    const std::string elementPath = ElementPath(rolesPath, index);
                    if (!encoded.is_string())
                        return Reject(elementPath, "extension.manifest.invalid_type", "Module role must be a string.");
                    const auto role = ParseRole(encoded.get_ref<const std::string &>());
                    if (!role.has_value())
                        return Reject(elementPath, "extension.manifest.invalid_value", "Module role is not recognized.");
                    if (std::ranges::find(roles, *role) != roles.end())
                        return Reject(elementPath, "extension.manifest.duplicate_identifier", "Module role must be unique.");
                    roles.push_back(*role);
                }
                return true;
            }

            [[nodiscard]] bool ParseModuleDependencies(const Json &moduleObject, const std::string_view path,
                                                       std::vector<std::string> &dependencies) {
                const auto found = moduleObject.find("dependencies");
                if (found == moduleObject.end())
                    return true;
                const std::string dependenciesPath = ChildPath(path, "dependencies");
                if (!found->is_array())
                    return Reject(dependenciesPath, "extension.manifest.invalid_type", "Module dependencies must be an array.");
                if (found->size() > Limits().maximumModules)
                    return Reject(dependenciesPath, "extension.manifest.collection_limit", "Module dependency count exceeds its limit.");
                for (std::size_t index = 0; index < found->size(); ++index) {
                    const Json &encoded = (*found)[index];
                    const std::string elementPath = ElementPath(dependenciesPath, index);
                    if (!encoded.is_string())
                        return Reject(elementPath, "extension.manifest.invalid_type", "Module dependency must be a string.");
                    std::string dependency = encoded.get<std::string>();
                    if (!IsCanonicalId(dependency, Limits().maximumIdentifierBytes))
                        return Reject(elementPath, "extension.manifest.invalid_identifier", "Module dependency is not canonical.");
                    if (std::ranges::find(dependencies, dependency) != dependencies.end())
                        return Reject(elementPath, "extension.manifest.duplicate_identifier", "Module dependency must be unique.");
                    dependencies.push_back(std::move(dependency));
                }
                return true;
            }

            template <typename Entry, typename ParseEntry>
            [[nodiscard]] bool ParseModuleServiceEntries(const Json &moduleObject, const std::string_view path,
                                                         const std::string_view fieldName, const std::string_view entryName,
                                                         std::vector<Entry> &entries, ParseEntry parseEntry) {
                const auto found = moduleObject.find(fieldName);
                if (found == moduleObject.end())
                    return true;
                const std::string entriesPath = ChildPath(path, fieldName);
                if (!found->is_array())
                    return Reject(entriesPath, "extension.manifest.invalid_type",
                                  "Module " + std::string{entryName} + "s must be an array.");
                if (found->size() > Limits().maximumContributions)
                    return Reject(entriesPath, "extension.manifest.collection_limit",
                                  "Module " + std::string{entryName} + " count exceeds its limit.");

                std::set<std::string, std::less<>> identities;
                for (std::size_t index = 0; index < found->size(); ++index) {
                    const Json &encoded = (*found)[index];
                    const std::string elementPath = ElementPath(entriesPath, index);
                    Entry value;
                    if (!encoded.is_object())
                        return Reject(elementPath, "extension.manifest.invalid_type",
                                      "Module " + std::string{entryName} + " must be an object.");
                    if (!parseEntry(encoded, elementPath, value))
                        return false;
                    if (!identities.insert(value.id).second)
                        return Reject(ChildPath(elementPath, "id"), "extension.manifest.duplicate_identifier",
                                      "Service " + std::string{entryName} + " ID must be unique within a module.");
                    entries.push_back(std::move(value));
                }
                return true;
            }

            [[nodiscard]] bool ParseModuleExports(const Json &moduleObject, const std::string_view path,
                                                  std::vector<ExtensionServiceExportManifest> &exports) {
                return ParseModuleServiceEntries(moduleObject, path, "exports", "export", exports,
                                                 [this](const Json &encoded, const std::string_view elementPath,
                                                        ExtensionServiceExportManifest &value) {
                    if (!AllowFields(encoded, elementPath, {"id", "contract", "version"}) ||
                        !ReadId(encoded, "id", elementPath, value.id) || !ReadId(encoded, "contract", elementPath, value.contract) ||
                        !ReadString(encoded, "version", elementPath, value.version, MaximumSemanticVersionBytes, true))
                        return false;
                    return IsCanonicalSemanticVersion(value.version) ||
                           Reject(ChildPath(elementPath, "version"), "extension.manifest.invalid_version",
                                  "Service export version must be canonical semantic version text.");
                });
            }

            [[nodiscard]] bool ParseModuleImports(const Json &moduleObject, const std::string_view path,
                                                  std::vector<ExtensionServiceImportManifest> &imports) {
                return ParseModuleServiceEntries(moduleObject, path, "imports", "import", imports,
                                                 [this](const Json &encoded, const std::string_view elementPath,
                                                        ExtensionServiceImportManifest &value) {
                    if (!AllowFields(encoded, elementPath, {"id", "service", "contract", "minimumVersion", "required"}) ||
                        !ReadId(encoded, "id", elementPath, value.id) || !ReadId(encoded, "service", elementPath, value.service) ||
                        !ReadId(encoded, "contract", elementPath, value.contract) ||
                        !ReadString(encoded, "minimumVersion", elementPath, value.minimumVersion, MaximumSemanticVersionBytes, true))
                        return false;
                    if (const auto required = encoded.find("required"); required != encoded.end()) {
                        if (!required->is_boolean())
                            return Reject(ChildPath(elementPath, "required"), "extension.manifest.invalid_type",
                                          "Service import required policy must be a boolean.");
                        value.required = required->get<bool>();
                    }
                    return IsCanonicalSemanticVersion(value.minimumVersion) ||
                           Reject(ChildPath(elementPath, "minimumVersion"), "extension.manifest.invalid_version",
                                  "Service import minimum version must be canonical semantic version text.");
                });
            }

            [[nodiscard]] bool ParseContributions(const Json &document, ExtensionManifest &manifest) {
                const auto found = document.find("contributions");
                if (found == document.end())
                    return true;
                if (!found->is_array())
                    return Reject("$.contributions", "extension.manifest.invalid_type", "Contributions must be an array.");
                if (found->size() > Limits().maximumContributions)
                    return Reject("$.contributions", "extension.manifest.collection_limit",
                                  "Contribution count exceeds the configured limit.");

                std::set<std::string, std::less<>> identities;
                manifest.contributions.reserve(found->size());
                for (std::size_t index = 0; index < found->size(); ++index) {
                    const std::string path = ElementPath("$.contributions", index);
                    if (!AppendContribution((*found)[index], path, identities, manifest.modules, manifest.contributions))
                        return false;
                }
                return true;
            }

            [[nodiscard]] bool AppendContribution(const Json &value, const std::string_view path,
                                                  std::set<std::string, std::less<>> &identities,
                                                  const std::vector<ExtensionModuleManifest> &modules,
                                                  std::vector<ExtensionContributionManifest> &contributions) {
                ExtensionContributionManifest contribution;
                if (!ParseContribution(value, path, contribution))
                    return false;
                if (!identities.insert(contribution.id).second)
                    return Reject(ChildPath(path, "id"), "extension.manifest.duplicate_identifier",
                                  "Contribution ID must be unique within the package.");
                if (!HasOwningModule(modules, contribution.owningModule))
                    return Reject(ChildPath(path, "module"), "extension.manifest.unresolved_reference",
                                  "Contribution references an undeclared module.");
                contributions.push_back(std::move(contribution));
                return true;
            }

            [[nodiscard]] bool ParseContribution(const Json &value, const std::string_view path,
                                                 ExtensionContributionManifest &contribution) {
                if (!value.is_object())
                    return Reject(path, "extension.manifest.invalid_type", "Contribution must be an object.");
                if (!AllowFields(value, path, {"type", "id", "module"}))
                    return false;
                if (!ReadString(value, "type", path, contribution.type, Limits().maximumIdentifierBytes, true) ||
                    !ReadId(value, "id", path, contribution.id) || !ReadId(value, "module", path, contribution.owningModule)) {
                    return false;
                }
                return IsCanonicalContributionType(contribution.type, Limits().maximumIdentifierBytes) ||
                       Reject(ChildPath(path, "type"), "extension.manifest.invalid_identifier", "Contribution type is not canonical.");
            }
        };
    }  // namespace

    /** @copydoc ParseExtensionManifest */
    Result<ExtensionManifest> ParseExtensionManifest(const std::string_view jsonContent, const ExtensionManifestLimits &limits) {
        auto parsed = ManifestParsing::ParseBoundedJson(jsonContent, limits);
        if (parsed.HasError())
            return Result<ExtensionManifest>::Failure(parsed.ErrorValue());
        return ManifestValidator{limits}.Validate(parsed.Value());
    }
}  // namespace Horo::Extensions
