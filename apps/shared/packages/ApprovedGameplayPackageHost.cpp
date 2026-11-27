#include "ApprovedGameplayPackageHost.h"

#include "Horo/Packages/GameplayPackageModuleHost.h"
#include "Horo/Packages/NativePackageModuleHost.h"
#include "Horo/Security/SecurityErrors.h"
#include "HostSignatureDocument.h"
#include "PackageHostPolicy.h"

#include <algorithm>
#include <charconv>
#include <chrono>

namespace Horo::Application::Internal {
    namespace {
        constexpr std::size_t MaximumApprovals = 64;
        constexpr std::string_view GameplayGrant = "runtime.participate";
        using namespace Packages;

        Error InvalidPolicy() {
            return MakeError(PackageLifecycleErrors::InvalidCandidate,
                             "Use explicit package module approvals and an independent publisher trust file for one startup project.");
        }

        /** @brief Binds each package's approved modules to one exact signed archive and extension identity. */
        bool HasExactBindings(std::span<const ApprovedGameplayPackage> approvals) {
            for (std::size_t index = 0; index < approvals.size(); ++index) {
                const auto &entry = approvals[index];
                const auto runtime = entry.role == Extensions::ExtensionModuleRole::RuntimeParticipant;
                const auto importer = entry.role == Extensions::ExtensionModuleRole::BackendCapability;
                if (entry.extension.empty() || entry.module.empty() || (!runtime && !importer) ||
                    (entry.descriptorRevision != 0) != runtime || entry.archive == Sha256Digest{} || entry.manifest == Sha256Digest{})
                    return false;
                for (const auto &prior : approvals.first(index)) {
                    if (entry.package != prior.package)
                        continue;
                    if (entry.module == prior.module || entry.extension != prior.extension || entry.archive != prior.archive ||
                        entry.manifest != prior.manifest)
                        return false;
                }
            }
            return true;
        }

        /** @brief Parses one launcher-owned identity tuple without accepting inferred SDK expectations. */
        Result<ApprovedGameplayPackage> ParseApproval(std::span<char *const> fields, const Extensions::ExtensionModuleRole role) {
            using Output = Result<ApprovedGameplayPackage>;
            const bool runtime = role == Extensions::ExtensionModuleRole::RuntimeParticipant;
            if (fields.size() != (runtime ? 6U : 5U) || std::ranges::any_of(fields, [](const char *value) {
                return !value || std::string_view{value}.empty() || std::string_view{value}.starts_with("--");
            }))
                return Output::Failure(InvalidPolicy());
            auto package = HoroPackageId::Parse(fields[0]);
            const std::string_view extension{fields[1]}, module{fields[2]};
            const auto archive = ParseSha256(fields[3]);
            const auto manifest = ParseSha256(fields[4]);
            std::uint64_t expectedRevision{};
            if (package.HasError() || archive.HasError() || manifest.HasError() || archive.Value() == Sha256Digest{} ||
                manifest.Value() == Sha256Digest{} || extension.size() > Extensions::ExtensionManifestLimits{}.maximumIdentifierBytes ||
                module.size() > Extensions::ExtensionManifestLimits{}.maximumIdentifierBytes)
                return Output::Failure(InvalidPolicy());
            if (runtime) {
                const std::string_view revision{fields[5]};
                const auto *end = revision.data() + revision.size();
                const auto parsed = std::from_chars(revision.data(), end, expectedRevision);
                if (parsed.ec != std::errc{} || parsed.ptr != end || expectedRevision == 0)
                    return Output::Failure(InvalidPolicy());
            }
            return Output::Success({std::move(package).Value(), std::string{extension}, std::string{module}, archive.Value(),
                                    manifest.Value(), expectedRevision, role});
        }

        /** @brief Rejects directories outside the exact launcher namespace, including redirects. */
        bool IsCanonical(const std::filesystem::path &path) {
            std::error_code error;
            return path.is_absolute() && path == path.lexically_normal() && std::filesystem::canonical(path, error) == path && !error;
        }

        /** @brief Uses existing filesystem identities to reject trust files under either mutable authority. */
        bool IsOutside(const std::filesystem::path &file, const std::filesystem::path &root) {
            std::error_code error;
            if (!std::filesystem::exists(root, error))
                return !error;
            for (auto parent = file.parent_path(); !parent.empty(); parent = parent.parent_path()) {
                if (std::filesystem::equivalent(parent, root, error) || error)
                    return false;
                if (parent == parent.root_path())
                    return true;
            }
            return false;
        }

        /** @brief Admits only explicit launcher approval of the already verified package and exact descriptor identity. */
        class ExecutionApproval final : public IPackageExtensionTrust {
        public:
            explicit ExecutionApproval(std::vector<ApprovedGameplayPackage> approvals) : approvals_(std::move(approvals)) {}

            Result<PackageExtensionTrustApproval> Approve(const PackageRestorePackage &package,
                                                          const Extensions::ExtensionManifest &descriptor) const override {
                using Output = Result<PackageExtensionTrustApproval>;
                if (!package.archive || !package.publisher || !package.publisher->installPermitted ||
                    package.publisher->outcome != PackagePublisherVerificationOutcome::Accepted ||
                    package.publisher->artifactDigest != package.archive->Digest())
                    return Output::Failure(MakeError(PackageLifecycleErrors::TrustRequired));
                const auto selected = std::ranges::find_if(approvals_, [&](const auto &approval) {
                    return approval.package == package.lock.package && approval.extension == descriptor.id &&
                           approval.archive == package.archive->Digest() && approval.manifest == package.archive->PackageManifestDigest();
                });
                if (selected == approvals_.end())
                    return Output::Failure(MakeError(PackageLifecycleErrors::TrustRequired));
                return Output::Success({selected->archive,
                                        selected->manifest,
                                        {.revision = 1,
                                         .knownPermissions = {{std::string{GameplayGrant}}},
                                         .approvedPermissions = {{std::string{GameplayGrant}}},
                                         .availableCapabilities = {{std::string{GameplayGrant}}}}});
            }

        private:
            std::vector<ApprovedGameplayPackage> approvals_;
        };

        /** @brief Supplies detached native evidence from the exact staged file's signed contribution inventory. */
        Result<std::shared_ptr<const Security::NativeArtifactGate>> MakeGameplayGate(const DurableFileSystem &files, const WallClock &clock,
                                                                                     const PackagePublisherVerificationPolicy &policy) {
            using Output = Result<std::shared_ptr<const Security::NativeArtifactGate>>;
            auto roots = std::make_shared<Security::TrustedRootStore>();
            for (const auto &record : policy.publishers) {
                if (record.publisherRevoked)
                    continue;
                auto added = roots->Add({record.algorithm, record.publisher.Value(), record.keyId, record.publicKey});
                if (added.HasError())
                    return Output::Failure(std::move(added).ErrorValue());
            }
            auto verifier = std::make_shared<Security::ArtifactVerifier>(Security::CreateMbedTlsSignatureProvider(), std::move(roots));
            auto resolver = [&files, &clock, publishers = policy.publishers](const std::filesystem::path &artifact) {
                using Envelope = Result<Security::DetachedSignatureEnvelope>;
                auto sibling = artifact;
                sibling += ".signature.json";
                auto raw = files.ReadFileBounded(sibling, MaximumHostSignatureDocumentBytes);
                if (raw.HasError())
                    return Envelope::Failure(WithCause(MakeError(SecurityErrors::MissingEvidence), std::move(raw).ErrorValue()));
                auto envelope = DecodeDetachedSignatureDocument(std::as_bytes(std::span{raw.Value()}));
                if (envelope.HasError())
                    return envelope;
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(clock.UtcNow().time_since_epoch()).count();
                const auto publisher = std::ranges::find_if(publishers, [&](const auto &record) {
                    return record.publisher.Value() == envelope.Value().publisherId && record.keyId == envelope.Value().keyId;
                });
                if (now < 0 || publisher == publishers.end() || publisher->publisherRevoked ||
                    (publisher->expiresAtUnixMilliseconds != 0 && static_cast<std::uint64_t>(now) >= publisher->expiresAtUnixMilliseconds))
                    return Envelope::Failure(MakeError(SecurityErrors::MissingEvidence));
                return envelope;
            };
            return Output::Success(std::make_shared<Security::DetachedFileArtifactGate>(std::move(verifier), std::move(resolver)));
        }
    }  // namespace

    /** @copydoc ParseApprovedGameplayPackageArguments */
    Result<std::optional<ApprovedGameplayPackageArguments>> ParseApprovedGameplayPackageArguments(std::span<char *const> arguments) {
        using Output = Result<std::optional<ApprovedGameplayPackageArguments>>;
        ApprovedGameplayPackageArguments result;
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            const auto *argument = arguments[index];
            if (!argument)
                return Output::Failure(InvalidPolicy());
            const std::string_view flag{argument};
            if (flag == "--gameplay-package-publisher-trust") {
                if (!result.publisherTrust.empty() || index + 1 == arguments.size() || !arguments[index + 1])
                    return Output::Failure(InvalidPolicy());
                result.publisherTrust = std::filesystem::u8path(arguments[++index]);
                if (!result.publisherTrust.is_absolute())
                    return Output::Failure(InvalidPolicy());
            } else if (flag == "--approve-gameplay-package" || flag == "--approve-importer-package") {
                const bool runtime = flag == "--approve-gameplay-package";
                const std::size_t fields = runtime ? 6 : 5;
                if (arguments.size() - index <= fields || result.approvals.size() == MaximumApprovals)
                    return Output::Failure(InvalidPolicy());
                const auto role = runtime ? Extensions::ExtensionModuleRole::RuntimeParticipant
                                          : Extensions::ExtensionModuleRole::BackendCapability;
                auto next = ParseApproval(arguments.subspan(index + 1, fields), role);
                if (next.HasError())
                    return Output::Failure(std::move(next).ErrorValue());
                result.approvals.push_back(std::move(next).Value());
                index += fields;
            }
        }
        if (result.publisherTrust.empty() && result.approvals.empty())
            return Output::Success(std::nullopt);
        if (result.publisherTrust.empty() || result.approvals.empty() || !HasExactBindings(result.approvals))
            return Output::Failure(InvalidPolicy());
        return Output::Success(std::move(result));
    }

    /** @copydoc PrepareApprovedGameplayPackageHost */
    Result<ApprovedGameplayPackageHost> PrepareApprovedGameplayPackageHost(DurableFileSystem &files, const WallClock &clock,
                                                                           PackageProjectPolicy policy,
                                                                           const ApprovedGameplayPackageArguments &arguments) {
        using Output = Result<ApprovedGameplayPackageHost>;
        if (arguments.approvals.empty() || arguments.approvals.size() > MaximumApprovals || !HasExactBindings(arguments.approvals) ||
            !IsCanonical(policy.projectRoot) || !IsCanonical(arguments.publisherTrust) ||
            !IsOutside(arguments.publisherTrust, policy.projectRoot) || !IsOutside(arguments.publisherTrust, policy.cacheRoot))
            return Output::Failure(InvalidPolicy());
        const auto raw = files.ReadFileBounded(arguments.publisherTrust, MaximumHostSignatureDocumentBytes);
        if (raw.HasError())
            return Output::Failure(raw.ErrorValue());
        auto trust = DecodePublisherTrustDocument(std::as_bytes(std::span{raw.Value()}), PackagePublisherVerificationPolicy{});
        if (trust.HasError())
            return Output::Failure(std::move(trust).ErrorValue());
        if (trust.Value().allowUnsigned || trust.Value().publishers.empty())
            return Output::Failure(InvalidPolicy());
        auto gate = MakeGameplayGate(files, clock, trust.Value());
        if (gate.HasError())
            return Output::Failure(std::move(gate).ErrorValue());
        auto publishers = PackagePublisherVerificationService::Create(std::move(trust).Value(), Security::CreateMbedTlsSignatureProvider());
        if (publishers.HasError())
            return Output::Failure(std::move(publishers).ErrorValue());
        std::error_code nativePathError;
        if (!policy.lifecycle.temporaryRoot.is_absolute() ||
            std::filesystem::weakly_canonical(policy.lifecycle.temporaryRoot, nativePathError) != policy.lifecycle.temporaryRoot ||
            nativePathError)
            return Output::Failure(InvalidPolicy());
        const auto created = files.CreateDirectoryPath(policy.lifecycle.temporaryRoot, DirectoryCreationMode::Parents);
        if (created.HasError())
            return Output::Failure(created.ErrorValue());
        if (!IsCanonical(policy.lifecycle.temporaryRoot))
            return Output::Failure(InvalidPolicy());
        std::vector<GameplayPackageModuleBinding> bindings;
        std::vector<NativePackageImporterBinding> importers;
        bindings.reserve(arguments.approvals.size());
        for (const auto &approved : arguments.approvals) {
            if (approved.role == Extensions::ExtensionModuleRole::BackendCapability) {
                importers.push_back({approved.package, approved.module, approved.archive, approved.manifest});
                continue;
            }
            bindings.push_back({approved.package,
                                approved.module,
                                std::string{Gameplay::CurrentGameplayBuildFingerprint()},
                                approved.descriptorRevision,
                                {},
                                {}});
        }
        policy.requirePublisherVerification = true;
        policy.executionProjectRoot = policy.projectRoot;
        policy.lifecycle.capabilities = {std::string{GameplayGrant}};
        policy.lifecycle.moduleHosts.clear();
        policy.gameplayModules = nullptr;
        if (!bindings.empty()) {
            auto host = std::make_shared<GameplayPackageModuleHost>(
                GameplayPackageHostConfiguration{policy.lifecycle.temporaryRoot, gate.Value()}, std::move(bindings));
            if (!host->Supports(Extensions::ExtensionModuleRole::RuntimeParticipant))
                return Output::Failure(InvalidPolicy());
            policy.gameplayModules = host.get();
            policy.lifecycle.moduleHosts.push_back(std::move(host));
        }
        if (!importers.empty()) {
            auto native = policy.lifecycle;
            native.artifactGate = std::move(gate).Value();
            policy.lifecycle.moduleHosts.push_back(std::make_shared<NativePackageModuleHost>(std::move(native), std::move(importers)));
        }
        // Only exact role owners may verify executable code in this launcher configuration.
        policy.lifecycle.artifactGate = std::make_shared<UnconfiguredPackageGate>();
        policy.lifecycle.moduleRetirements = std::make_shared<PackageModuleRetirementQueue>();
        return Output::Success({std::make_unique<ExecutionApproval>(arguments.approvals),
                                std::make_unique<PackagePublisherVerificationService>(std::move(publishers).Value()), std::move(policy)});
    }
}  // namespace Horo::Application::Internal
