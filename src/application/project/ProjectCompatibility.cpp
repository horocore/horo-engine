#include "Horo/Application/ProjectCompatibility.h"

#include "GeneratedProjectCompatibility.h"
#include "Horo/Application/ProjectSourceDocument.h"
#include "Horo/Foundation/String.h"
#include "ProjectErrors.h"

#include <algorithm>
#include <exception>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <utility>

namespace Horo::Application {
    namespace {
        using Json = nlohmann::json;
        constexpr std::uintmax_t kMaximumMetadataBytes = 64U * 1024U;
        constexpr std::size_t kMaximumDepth = 16;
        constexpr std::size_t kMaximumKeys = 256;
        constexpr std::size_t kMaximumKeyBytes = 128;
        constexpr std::size_t kMaximumStringBytes = 8U * 1024U;
        constexpr std::size_t kMaximumProjectIdBytes = 256;
        constexpr std::size_t kMaximumProjectNameBytes = 1024;
        constexpr std::size_t kMaximumProjectVersionBytes = 64;
        constexpr std::size_t kMaximumTimestampBytes = 64;
        constexpr std::size_t kMaximumBackendIdBytes = 64;
        constexpr std::size_t kMaximumSignatureBytes = 4U * 1024U;

        Error MetadataError(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return MakeError(descriptor, std::move(message));
        }

        bool IsCanonicalBackendId(const std::string_view value) noexcept {
            if (value.empty() || value.size() > kMaximumBackendIdBytes || value.back() == '-')
                return false;
            const auto alpha = [](const char c) {
                return c >= 'a' && c <= 'z';
            };
            const auto digit = [](const char c) {
                return c >= '0' && c <= '9';
            };
            if (!alpha(value.front()))
                return false;
            return std::ranges::all_of(value, [&](const char c) {
                return alpha(c) || digit(c) || c == '-';
            });
        }

        Result<std::string> ReadMetadata(const std::filesystem::path &path) {
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (error) {
                std::error_code existsError;
                const bool exists = std::filesystem::exists(path, existsError);
                const ErrorCodeDescriptor &descriptor =
                    !existsError && !exists ? ProjectErrors::MetadataNotFound : ProjectErrors::MetadataReadFailed;
                return Result<std::string>::Failure(MetadataError(descriptor, "Unable to inspect " + path.string() + '.'));
            }
            if (size == 0 || size > 1024U * 1024U) {
                return Result<std::string>::Failure(MetadataError(ProjectErrors::MetadataSizeInvalid));
            }
            std::ifstream input(path, std::ios::binary);
            if (!input.is_open()) {
                return Result<std::string>::Failure(MetadataError(ProjectErrors::MetadataReadFailed));
            }
            std::string contents(size, '\0');
            input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (input.gcount() != static_cast<std::streamsize>(contents.size()) || input.bad()) {
                return Result<std::string>::Failure(MetadataError(ProjectErrors::MetadataReadFailed));
            }
            return Result<std::string>::Success(std::move(contents));
        }

        struct ParserState {
            enum class RootMarker {
                None,
                Release,
                Contract
            };
            std::size_t maximumDepth = 32;
            std::size_t maximumKeys = 32768;
            bool duplicate = false;
            bool limitExceeded = false;
            std::size_t keyCount = 0;
            std::vector<std::unordered_set<std::string>> keys;
            RootMarker rootMarker = RootMarker::None;
            std::string release;
            std::string contract;

            /** @brief Retains bounded root markers even when a later callback discards an over-limit DOM. */
            void CaptureRootAuthority(const int depth, const Json::parse_event_t event, const Json &parsed) {
                if (depth != 1)
                    return;
                if (event == Json::parse_event_t::key) {
                    const auto &value = parsed.get_ref<const std::string &>();
                    rootMarker = value == "horoVersion"          ? RootMarker::Release
                                 : value == "persistentContract" ? RootMarker::Contract
                                                                 : RootMarker::None;
                } else if (event == Json::parse_event_t::value && parsed.is_string()) {
                    const auto &value = parsed.get_ref<const std::string &>();
                    constexpr std::size_t maximumMarkerBytes = 7U + 64U;  // Canonical sha256: identity.
                    if (value.size() > maximumMarkerBytes)
                        return;
                    if (rootMarker == RootMarker::Release)
                        release = value;
                    else if (rootMarker == RootMarker::Contract)
                        contract = value;
                }
            }

            bool OnEvent(const int depth, const Json::parse_event_t event, Json &parsed) {
                CaptureRootAuthority(depth, event, parsed);
                if (depth < 0 || static_cast<std::size_t>(depth) > maximumDepth) {
                    limitExceeded = true;
                    return false;
                }
                const auto objectDepth = static_cast<std::size_t>(depth);
                if (event == Json::parse_event_t::object_start) {
                    if (keys.size() <= objectDepth)
                        keys.resize(objectDepth + 1);
                    keys[objectDepth].clear();
                } else if (event == Json::parse_event_t::key) {
                    const std::string value = parsed.get<std::string>();
                    const auto parentDepth = objectDepth > 0 ? objectDepth - 1 : 0;
                    if (keys.size() <= parentDepth)
                        keys.resize(parentDepth + 1);
                    duplicate = !keys[parentDepth].insert(value).second || duplicate;
                    limitExceeded = ++keyCount > maximumKeys || value.size() > kMaximumKeyBytes || limitExceeded;
                } else if (event == Json::parse_event_t::value && parsed.is_string()) {
                    const auto &value = parsed.get_ref<const std::string &>();
                    limitExceeded = value.size() > kMaximumStringBytes || limitExceeded;
                }
                return !duplicate && !limitExceeded;
            }
        };

        /** @brief Selects the expanded bounds only for the exact application-registered development contract. */
        bool HasRegisteredExpandedProfile(const ParserState &state) {
            const auto *decision = BuiltInReleaseCompatibilityRegistry().Find({ParseHoroVersion("0.2.0").Value()});
            return decision != nullptr && state.release == "0.2.0" &&
                   state.contract == FormatPersistentContractHash(decision->persistentContract);
        }

        /** @brief Parses under the supplied finite callback state without selecting a release profile. */
        Result<Json> ParseBoundedDocument(const std::string_view contents, ParserState &state) {
            const Json::parser_callback_t callback = [&state](const int depth, const Json::parse_event_t event, Json &parsed) {
                return state.OnEvent(depth, event, parsed);
            };
            try {
                return Result<Json>::Success(Json::parse(contents, callback, true, false));
            } catch (const Json::exception &exception) {
                return Result<Json>::Failure(
                    MetadataError(ProjectErrors::MetadataInvalidJson, "Invalid project metadata JSON: " + std::string{exception.what()}));
            }
        }

        /** @brief Applies the registered or legacy profile and its stable diagnostic precedence. */
        Result<Json> ParseJson(const std::string_view contents, const bool legacy = false) {
            if (contents.empty() || contents.size() > (legacy ? kMaximumMetadataBytes : 1024U * 1024U))
                return Result<Json>::Failure(MetadataError(ProjectErrors::MetadataSizeInvalid));
            ParserState state;
            if (legacy) {
                state.maximumDepth = kMaximumDepth;
                state.maximumKeys = kMaximumKeys;
            }
            auto parsed = ParseBoundedDocument(contents, state);
            // The expanded parse is only a bounded authority probe. Until the exact contract
            // agrees, the legacy parser owns diagnostics, including size-before-duplicate.
            if (!legacy && !HasRegisteredExpandedProfile(state))
                return ParseJson(contents, true);
            if (state.duplicate)
                return Result<Json>::Failure(MetadataError(ProjectErrors::MetadataDuplicateKey));
            if (state.limitExceeded)
                return Result<Json>::Failure(MetadataError(ProjectErrors::MetadataLimitExceeded));
            return parsed;
        }

        bool HasString(const Json &object, const char *key) {
            return object.contains(key) && object[key].is_string();
        }

        Result<CompatibilityProof> ParseProof(const Json &value) {
            if (!value.is_object() || !HasString(value, "release") || !HasString(value, "contractBaseline") ||
                !HasString(value, "decisionHash") || !HasString(value, "signature")) {
                return Result<CompatibilityProof>::Failure(MetadataError(ProjectErrors::MetadataInvalidSchema));
            }
            const std::string &signature = value["signature"].get_ref<const std::string &>();
            if (signature.empty() || signature.size() > kMaximumSignatureBytes) {
                return Result<CompatibilityProof>::Failure(MetadataError(ProjectErrors::MetadataInvalidValue));
            }
            auto release = ParseHoroVersion(value["release"].get_ref<const std::string &>());
            auto baseline = ParseHoroVersion(value["contractBaseline"].get_ref<const std::string &>());
            auto decisionHash = ParseCompatibilityDecisionHash(value["decisionHash"].get_ref<const std::string &>());
            if (release.HasError() || baseline.HasError() || decisionHash.HasError()) {
                return Result<CompatibilityProof>::Failure(MetadataError(ProjectErrors::MetadataInvalidValue));
            }
            return Result<CompatibilityProof>::Success({{release.Value()}, {baseline.Value()}, decisionHash.Value(), signature});
        }

        bool SameReleaseLine(const HoroVersion &lhs, const HoroVersion &rhs) noexcept {
            return lhs.IsStable() && rhs.IsStable() && lhs.major == rhs.major && lhs.minor == rhs.minor;
        }

        bool IsCanonicalVersionValue(const HoroVersion &version) {
            const auto parsed = ParseHoroVersion(FormatHoroVersion(version));
            return parsed.HasValue() && parsed.Value() == version;
        }

        ProjectCompatibilitySnapshot FailedSnapshot(const EngineReleaseVersion &target, const ProjectCompatibilityStatus status,
                                                    Error error) {
            return {.status = status, .targetVersion = target, .diagnostic = std::move(error)};
        }
    }  // namespace

    ReleaseCompatibilityRegistry::ReleaseCompatibilityRegistry(std::vector<ReleaseCompatibilityDecision> decisions)
        : decisions_(std::move(decisions)) {}

    /** @copydoc ReleaseCompatibilityRegistry::Create */
    Result<ReleaseCompatibilityRegistry> ReleaseCompatibilityRegistry::Create(
        const std::span<const ReleaseCompatibilityDecision> decisions) {
        if (decisions.empty()) {
            return Result<ReleaseCompatibilityRegistry>::Failure(MakeError(ProjectErrors::RegistryInvalid));
        }
        std::vector<ReleaseCompatibilityDecision> copy{decisions.begin(), decisions.end()};
        std::ranges::sort(copy, [](const auto &lhs, const auto &rhs) {
            return CompareHoroVersions(lhs.release.value, rhs.release.value) == std::strong_ordering::less;
        });
        for (std::size_t i = 0; i < copy.size(); ++i) {
            const auto &decision = copy[i];
            if (!IsCanonicalVersionValue(decision.release.value) || !IsCanonicalVersionValue(decision.contractBaseline.value)) {
                return Result<ReleaseCompatibilityRegistry>::Failure(MakeError(ProjectErrors::RegistryInvalid));
            }
            if (i > 0 && copy[i - 1].release == decision.release) {
                return Result<ReleaseCompatibilityRegistry>::Failure(MakeError(ProjectErrors::RegistryDuplicateRelease));
            }
            if (CompareHoroVersions(decision.contractBaseline.value, decision.release.value) == std::strong_ordering::greater) {
                return Result<ReleaseCompatibilityRegistry>::Failure(MakeError(ProjectErrors::RegistryBaselineMismatch));
            }
            if (decision.kind == CompatibilityDecisionKind::EstablishBaseline &&
                !(decision.release.value == decision.contractBaseline.value)) {
                return Result<ReleaseCompatibilityRegistry>::Failure(MakeError(ProjectErrors::RegistryBaselineMismatch));
            }
            if (const auto baseline = std::ranges::find_if(copy,
                                                           [&](const auto &candidate) {
                return candidate.release.value == decision.contractBaseline.value;
            });
                baseline == copy.end() || baseline->kind != CompatibilityDecisionKind::EstablishBaseline ||
                baseline->persistentContract != decision.persistentContract) {
                return Result<ReleaseCompatibilityRegistry>::Failure(MakeError(ProjectErrors::RegistryBaselineMismatch));
            }
            for (std::size_t prior = 0; prior < i; ++prior) {
                if (SameReleaseLine(copy[prior].release.value, decision.release.value) &&
                    (copy[prior].contractBaseline != decision.contractBaseline ||
                     copy[prior].persistentContract != decision.persistentContract)) {
                    return Result<ReleaseCompatibilityRegistry>::Failure(MakeError(ProjectErrors::RegistryPatchContractDrift));
                }
            }
        }
        return Result<ReleaseCompatibilityRegistry>::Success(ReleaseCompatibilityRegistry{std::move(copy)});
    }

    /** @copydoc ReleaseCompatibilityRegistry::Find */
    const ReleaseCompatibilityDecision *ReleaseCompatibilityRegistry::Find(const EngineReleaseVersion &release) const noexcept {
        const auto found =
            std::ranges::lower_bound(decisions_, release, [](const EngineReleaseVersion &lhs, const EngineReleaseVersion &rhs) {
            return CompareHoroVersions(lhs.value, rhs.value) == std::strong_ordering::less;
        }, &ReleaseCompatibilityDecision::release);
        return found != decisions_.end() && found->release == release ? std::to_address(found) : nullptr;
    }

    /** @copydoc ReleaseCompatibilityRegistry::Decisions */
    std::span<const ReleaseCompatibilityDecision> ReleaseCompatibilityRegistry::Decisions() const noexcept {
        return decisions_;
    }

    /** @copydoc RejectingCompatibilityProofVerifier::Verify */
    Result<void> RejectingCompatibilityProofVerifier::Verify(const CompatibilityProof &proof,
                                                             const PersistentContractHash &expectedContract) const {
        static_cast<void>(proof);
        static_cast<void>(expectedContract);
        return Result<void>::Failure(MakeError(ProjectErrors::ProofRejected));
    }

    namespace {
        /** @brief Validates the existing startup metadata subset from the sole strict root parser. */
        Result<ProjectMetadata> DecodeMetadata(const Json &root) {
            if (!root.is_object() || !HasString(root, "horoVersion") || !HasString(root, "persistentContract") ||
                !HasString(root, "projectId") || !HasString(root, "name") || !HasString(root, "projectVersion") ||
                !HasString(root, "createdAt") || !root.contains("settings") || !root["settings"].is_object() ||
                !HasString(root["settings"], "renderBackend")) {
                return Result<ProjectMetadata>::Failure(MetadataError(ProjectErrors::MetadataInvalidSchema));
            }

            auto horoVersion = ParseHoroVersion(root["horoVersion"].get_ref<const std::string &>());
            auto contract = ParsePersistentContractHash(root["persistentContract"].get_ref<const std::string &>());
            if (horoVersion.HasError() || contract.HasError()) {
                return Result<ProjectMetadata>::Failure(MetadataError(ProjectErrors::MetadataInvalidValue));
            }

            const std::string projectId = root["projectId"].get<std::string>();
            const std::string name = root["name"].get<std::string>();
            const std::string projectVersion = root["projectVersion"].get<std::string>();
            const std::string createdAt = root["createdAt"].get<std::string>();
            const std::string renderBackend = root["settings"]["renderBackend"].get<std::string>();
            if (Text::IsBlank(projectId) || projectId.size() > kMaximumProjectIdBytes || Text::IsBlank(name) ||
                name.size() > kMaximumProjectNameBytes || Text::IsBlank(projectVersion) ||
                projectVersion.size() > kMaximumProjectVersionBytes || Text::IsBlank(createdAt) ||
                createdAt.size() > kMaximumTimestampBytes || !IsCanonicalBackendId(renderBackend)) {
                return Result<ProjectMetadata>::Failure(MetadataError(ProjectErrors::MetadataInvalidValue));
            }

            std::optional<CompatibilityProof> proof;
            if (root.contains("compatibilityProof")) {
                auto parsedProof = ParseProof(root["compatibilityProof"]);
                if (parsedProof.HasError())
                    return Result<ProjectMetadata>::Failure(parsedProof.ErrorValue());
                proof = std::move(parsedProof).Value();
            }
            std::optional<MigrationHistoryHead> historyHead;
            if (root.contains("migrationHistoryHead")) {
                if (!root["migrationHistoryHead"].is_string())
                    return Result<ProjectMetadata>::Failure(MetadataError(ProjectErrors::MetadataInvalidValue));
                auto parsedHead = ParsePersistentContractHash(root["migrationHistoryHead"].get_ref<const std::string &>());
                if (parsedHead.HasError())
                    return Result<ProjectMetadata>::Failure(MetadataError(ProjectErrors::MetadataInvalidValue));
                historyHead = MigrationHistoryHead{MigrationContentHash{parsedHead.Value().bytes}};
            }
            return Result<ProjectMetadata>::Success({{horoVersion.Value()},
                                                     contract.Value(),
                                                     std::move(proof),
                                                     projectId,
                                                     name,
                                                     projectVersion,
                                                     createdAt,
                                                     renderBackend,
                                                     historyHead});
        }
    }  // namespace

    /** @copydoc DecodeProjectSourceDocument */
    Result<ProjectSourceDocument> DecodeProjectSourceDocument(const std::string_view contents) {
        auto parsed = ParseJson(contents);
        if (parsed.HasError())
            return Result<ProjectSourceDocument>::Failure(parsed.ErrorValue());
        auto metadata = DecodeMetadata(parsed.Value());
        if (metadata.HasError())
            return Result<ProjectSourceDocument>::Failure(metadata.ErrorValue());
        std::optional<std::string> network;
        const auto &settings = parsed.Value()["settings"];
        if (settings.contains("network")) {
            // Network schema interpretation belongs exclusively to its codec. Legacy metadata
            // inspection must not start rejecting previously ignored settings members.
            network = settings["network"].dump();
        }
        return Result<ProjectSourceDocument>::Success({std::move(metadata).Value(), std::move(network)});
    }

    /** @copydoc LoadProjectSourceDocument */
    Result<ProjectSourceDocument> LoadProjectSourceDocument(const std::filesystem::path &projectRoot) {
        std::error_code error;
        const auto root = std::filesystem::weakly_canonical(projectRoot, error);
        if (projectRoot.empty() || error)
            return Result<ProjectSourceDocument>::Failure(MetadataError(ProjectErrors::MetadataReadFailed));
        const auto path = std::filesystem::weakly_canonical(root / ".horo/project.json", error);
        const auto relative = path.lexically_relative(root);
        if (error || relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            return Result<ProjectSourceDocument>::Failure(
                MetadataError(ProjectErrors::MetadataReadFailed, "Project metadata escapes the authorized project root."));
        auto contents = ReadMetadata(path);
        if (contents.HasError())
            return Result<ProjectSourceDocument>::Failure(contents.ErrorValue());
        return DecodeProjectSourceDocument(contents.Value());
    }

    /** @copydoc LoadProjectMetadata */
    Result<ProjectMetadata> LoadProjectMetadata(const std::filesystem::path &projectRoot) {
        auto source = LoadProjectSourceDocument(projectRoot);
        if (source.HasError())
            return Result<ProjectMetadata>::Failure(source.ErrorValue());
        return Result<ProjectMetadata>::Success(std::move(source).Value().metadata);
    }

    ProjectCompatibilityInspector::ProjectCompatibilityInspector(const ReleaseCompatibilityRegistry &registry,
                                                                 const EngineReleaseVersion &currentRelease,
                                                                 const ICompatibilityProofVerifier &proofVerifier) noexcept
        : registry_(registry), currentRelease_(currentRelease), proofVerifier_(proofVerifier) {}

    /** @copydoc ProjectCompatibilityInspector::Inspect */
    ProjectCompatibilitySnapshot ProjectCompatibilityInspector::Inspect(const std::filesystem::path &projectRoot) const {
        auto loaded = LoadProjectMetadata(projectRoot);
        if (loaded.HasError()) {
            const bool inaccessible = loaded.ErrorValue().code.Value() == "project.metadata.not_found" ||
                                      loaded.ErrorValue().code.Value() == "project.metadata.read_failed";
            return FailedSnapshot(currentRelease_,
                                  inaccessible ? ProjectCompatibilityStatus::Inaccessible : ProjectCompatibilityStatus::Corrupt,
                                  loaded.ErrorValue());
        }
        ProjectMetadata metadata = std::move(loaded).Value();
        const ReleaseCompatibilityDecision *current = registry_.Find(currentRelease_);
        if (current == nullptr) {
            return FailedSnapshot(currentRelease_, ProjectCompatibilityStatus::Corrupt, MakeError(ProjectErrors::RegistryInvalid));
        }

        ProjectCompatibilitySnapshot snapshot{.metadata = metadata, .targetVersion = currentRelease_};
        if (const ReleaseCompatibilityDecision *source = registry_.Find(metadata.horoVersion); source != nullptr) {
            using enum ProjectCompatibilityStatus;
            snapshot.sourceBaseline = source->contractBaseline;
            if (source->persistentContract != metadata.persistentContract) {
                snapshot.status = FutureVersion;
                snapshot.diagnostic = MakeError(ProjectErrors::HashInvalid, "Project contract does not match its release decision.");
                return snapshot;
            }
            const auto ordering = CompareHoroVersions(metadata.horoVersion.value, currentRelease_.value);
            if (ordering == std::strong_ordering::equal) {
                snapshot.status = Current;
                return snapshot;
            }
            if (SameReleaseLine(metadata.horoVersion.value, currentRelease_.value) &&
                source->contractBaseline == current->contractBaseline && source->persistentContract == current->persistentContract) {
                snapshot.status = CompatibleReleaseLine;
                snapshot.markerUpdateRequired = ordering == std::strong_ordering::less;
                return snapshot;
            }
            snapshot.status = ordering == std::strong_ordering::less ? MigrationPathMissing : FutureVersion;
            return snapshot;
        }

        using enum ProjectCompatibilityStatus;
        const auto ordering = CompareHoroVersions(metadata.horoVersion.value, currentRelease_.value);
        if (ordering == std::strong_ordering::greater && SameReleaseLine(metadata.horoVersion.value, currentRelease_.value) &&
            metadata.persistentContract == current->persistentContract && metadata.compatibilityProof.has_value() &&
            metadata.compatibilityProof->release == metadata.horoVersion &&
            metadata.compatibilityProof->contractBaseline == current->contractBaseline) {
            const Result<void> verification = proofVerifier_.Verify(*metadata.compatibilityProof, current->persistentContract);
            if (verification.HasValue()) {
                snapshot.status = CompatibleReleaseLine;
                snapshot.sourceBaseline = current->contractBaseline;
                return snapshot;
            }
            snapshot.diagnostic = verification.ErrorValue();
        }
        snapshot.status = ordering == std::strong_ordering::less ? MigrationPathMissing : FutureVersion;
        if (!snapshot.diagnostic.has_value() && ordering == std::strong_ordering::greater) {
            snapshot.diagnostic = MakeError(ProjectErrors::ProofRejected);
        }
        return snapshot;
    }

    /** @copydoc CurrentEngineReleaseVersion */
    EngineReleaseVersion CurrentEngineReleaseVersion() {
        const auto parsed = ParseHoroVersion(Generated::kHoroReleaseVersion);
        if (parsed.HasError())
            std::terminate();
        return {parsed.Value()};
    }

    namespace {
        ReleaseCompatibilityRegistry CreateBuiltInRegistry() {
            std::vector<ReleaseCompatibilityDecision> decisions;
            decisions.reserve(Generated::kHoroReleaseDecisionCount);
            for (const auto &generated : Generated::kHoroReleaseDecisions) {
                const auto release = ParseHoroVersion(generated.release);
                const auto baseline = ParseHoroVersion(generated.contractBaseline);
                const auto contract = ParsePersistentContractHash(generated.persistentContract);
                const auto decisionHash = ParseCompatibilityDecisionHash(generated.decisionHash);
                if (release.HasError() || baseline.HasError() || contract.HasError() || decisionHash.HasError())
                    std::terminate();
                decisions.push_back({{release.Value()},
                                     {baseline.Value()},
                                     contract.Value(),
                                     decisionHash.Value(),
                                     generated.establishesBaseline ? CompatibilityDecisionKind::EstablishBaseline
                                                                   : CompatibilityDecisionKind::CompatibleReleaseLine});
            }
            auto created = ReleaseCompatibilityRegistry::Create(decisions);
            if (created.HasError())
                std::terminate();
            return std::move(created).Value();
        }
    }  // namespace

    /** @copydoc BuiltInReleaseCompatibilityRegistry */
    const ReleaseCompatibilityRegistry &BuiltInReleaseCompatibilityRegistry() {
        static const ReleaseCompatibilityRegistry registry = CreateBuiltInRegistry();
        return registry;
    }

    /** @copydoc InspectProjectCompatibility */
    ProjectCompatibilitySnapshot InspectProjectCompatibility(const std::filesystem::path &projectRoot) {
        static const RejectingCompatibilityProofVerifier verifier;
        const ProjectCompatibilityInspector inspector{BuiltInReleaseCompatibilityRegistry(), CurrentEngineReleaseVersion(), verifier};
        return inspector.Inspect(projectRoot);
    }
}  // namespace Horo::Application
