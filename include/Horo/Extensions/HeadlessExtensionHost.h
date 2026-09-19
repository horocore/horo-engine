#pragma once

/**
 * @file HeadlessExtensionHost.h
 * @brief Composition-owned backend extension capabilities for CLI and automation hosts.
 */

#include "Horo/Assets/AssetImporter.h"
#include "Horo/Extensions/ApplicationCapabilityRegistry.h"
#include "Horo/Extensions/AssetCookerRegistry.h"
#include "Horo/Extensions/ExtensionDiscovery.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Extensions/PipelineStepRegistry.h"
#include "Horo/Extensions/ProjectValidatorRegistry.h"
#include "Horo/Extensions/ToolchainProviderRegistry.h"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Horo::Extensions {
    /** @brief Monotonic lifecycle state of one headless extension composition. */
    enum class HeadlessExtensionHostState : std::uint8_t {
        Configuring,
        Ready,
        Failed,
        ShuttingDown,
        Shutdown,
    };

    /** @brief Stable operation stage attributed to one headless-host diagnostic. */
    enum class HeadlessExtensionHostStage : std::uint8_t {
        Discovery,
        Activation,
        Registration,
        Import,
        Cook,
        Validation,
        Pipeline,
        Toolchain,
        CapabilityResolution,
    };

    /** @brief One bounded typed failure retained for CLI, MCP, or automation presentation. */
    struct HeadlessExtensionHostDiagnostic final {
        HeadlessExtensionHostStage stage{HeadlessExtensionHostStage::Activation}; /**< Exact failing host stage. */
        std::string subject;                                                      /**< Package, provider, or work identity. */
        Error error;                                                              /**< Original typed failure and cause chain. */
    };

    /** @brief Immutable inspection copy of headless discovery, activation, and diagnostic state. */
    struct HeadlessExtensionHostSnapshot final {
        HeadlessExtensionHostState state{HeadlessExtensionHostState::Configuring}; /**< Lifecycle state at inspection time. */
        std::vector<std::string> discoveredPackages;                               /**< Stable package identities in discovery order. */
        std::vector<std::string> loadedExtensions;                                 /**< Manager-owned active extension identities. */
        std::vector<Discovery::RootDiagnostic> rootDiagnostics;                    /**< Root-policy decisions from the last startup. */
        std::vector<HeadlessExtensionHostDiagnostic> diagnostics;                  /**< Bounded attributed failures retained so far. */
    };

    /** @brief Host-owned construction policy for one headless extension composition. */
    struct HeadlessExtensionHostConfiguration final {
        std::vector<Discovery::RootRequest> roots;                        /**< Approved roots supplied by package/product composition. */
        Discovery::RootPolicy discoveryPolicy;                            /**< Explicit discovery profile and development policy. */
        std::vector<std::string> capabilities;                            /**< Stable capabilities granted to activated modules. */
        ErrorCodeRegistry validationErrors;                               /**< Declared errors accepted from project validators. */
        ValidationResultLimits validationLimits;                          /**< Host bounds for each validator result. */
        std::shared_ptr<const Security::NativeArtifactGate> artifactGate; /**< Mandatory gate when native packages are activated. */
        ExtensionManager::NativeLibraryLoader libraryLoader;              /**< Optional explicit native loader boundary. */
        std::size_t maximumDiagnostics{1024U};                            /**< Retained failure bound. */
    };

    /**
     * @brief Owns backend extension discovery, publication, execution, diagnostics, and shutdown for a headless host.
     *
     * Registration is accepted only while Configuring. Start discovers only declared packages,
     * activates them under the Headless profile, then atomically publishes the importer catalog.
     * Work is admitted only while Ready. Shutdown closes every typed registry before releasing
     * native module ownership and is safe to call repeatedly.
     */
    class HeadlessExtensionHost final {
    public:
        /**
         * @brief Creates a host using explicit process policy and platform execution authorities.
         * @param configuration Owned discovery, validation, trust, and diagnostic policy.
         * @param toolchainPolicy Borrowed host policy retained for the host lifetime.
         * @param processes Borrowed platform runner retained for the host lifetime.
         * @return Composition owner, or a typed configuration failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<HeadlessExtensionHost>> Create(HeadlessExtensionHostConfiguration configuration,
                                                                                   const IToolchainInvocationPolicy &toolchainPolicy,
                                                                                   IExternalProcessRunner &processes);

        ~HeadlessExtensionHost() noexcept;
        HeadlessExtensionHost(const HeadlessExtensionHost &) = delete;
        HeadlessExtensionHost &operator=(const HeadlessExtensionHost &) = delete;
        HeadlessExtensionHost(HeadlessExtensionHost &&) = delete;
        HeadlessExtensionHost &operator=(HeadlessExtensionHost &&) = delete;

        /**
         * @brief Registers one application capability publication and retains its lifetime.
         * @param descriptor Exact capability, provider, version, and generation identity.
         * @return Success, or a typed registration or lifecycle failure.
         */
        [[nodiscard]] Result<void> RegisterCapability(ApplicationCapabilityProviderDescriptor descriptor);
        /**
         * @brief Registers one built-in importer in the unpublished host catalog.
         * @param contribution Complete host-owned importer contribution.
         * @return Success, or a typed catalog or lifecycle failure.
         */
        [[nodiscard]] Result<void> RegisterImporter(Assets::AssetImporterContribution contribution);
        /**
         * @brief Registers one cooker and retains its publication lifetime.
         * @param descriptor Stable cooker identity, target support, and format contract.
         * @param provider Shared immutable cooker implementation.
         * @return Success, or a typed registration or lifecycle failure.
         */
        [[nodiscard]] Result<void> RegisterCooker(AssetCookerDescriptor descriptor, std::shared_ptr<const IAssetCooker> provider);
        /**
         * @brief Registers one validator and retains its publication lifetime.
         * @param descriptor Stable validator and provider-generation identity.
         * @param provider Shared immutable validator implementation.
         * @return Success, or a typed registration or lifecycle failure.
         */
        [[nodiscard]] Result<void> RegisterValidator(ProjectValidatorProviderDescriptor descriptor,
                                                     std::shared_ptr<const IProjectValidator> provider);
        /**
         * @brief Registers one pipeline step and retains its publication lifetime.
         * @param descriptor Stable step identity, phase, and artifact graph contract.
         * @param provider Shared immutable step implementation.
         * @return Success, or a typed registration or lifecycle failure.
         */
        [[nodiscard]] Result<void> RegisterPipelineStep(PipelineStepDescriptor descriptor, std::shared_ptr<const IPipelineStep> provider);
        /**
         * @brief Registers one toolchain provider and retains its publication lifetime.
         * @param descriptor Stable provider identity, generation, and logical tool set.
         * @return Generation-bound invocation authority, or a typed registration or lifecycle failure.
         */
        [[nodiscard]] Result<ToolchainInvocationAuthority> RegisterToolchain(ToolchainProviderDescriptor descriptor);

        /**
         * @brief Discovers and activates the declared package set, then publishes the importer snapshot.
         * @param locations Package-graph-selected relative locations; no directory scanning occurs.
         * @return Success only after every package is active and the host is Ready.
         */
        [[nodiscard]] Result<void> Start(std::span<const Discovery::PackageLocation> locations);

        /**
         * @brief Invokes one exact importer through the published immutable catalog.
         * @param contributionId Exact published contribution identity.
         * @param input Immutable source and import settings.
         * @param cancellation Host-owned cancellation token.
         * @return Prepared asset data, or an attributed typed failure.
         */
        [[nodiscard]] Result<Assets::PreparedAssetImport> Import(std::string_view contributionId, const Assets::AssetImportInput &input,
                                                                 const CancellationToken &cancellation) const;
        /**
         * @brief Executes one host-mediated cook operation.
         * @param request Immutable asset identity, target, digests, and input bytes.
         * @param cancellation Host-owned cancellation token.
         * @return Validated cooked artifact, or an attributed typed failure.
         */
        [[nodiscard]] Result<AssetCookerResult> Cook(const AssetCookerRequest &request, const CancellationToken &cancellation) const;
        /**
         * @brief Executes every registered validator over one immutable project snapshot.
         * @param snapshot Immutable bounded project input.
         * @param cancellation Host-owned cancellation token.
         * @return Deterministically attributed validation results, or a typed failure.
         */
        [[nodiscard]] Result<std::vector<AttributedProjectValidationResult>> Validate(const ProjectValidationSnapshot &snapshot,
                                                                                      const CancellationToken &cancellation) const;
        /**
         * @brief Executes the registered dependency graph transactionally.
         * @param initialArtifacts Immutable caller-owned pipeline inputs.
         * @param cancellation Host-owned cancellation token.
         * @return Committed outputs and execution records, or a typed failure with no published partial result.
         */
        [[nodiscard]] Result<PipelineRunResult> ExecutePipeline(std::span<const PipelineArtifactView> initialArtifacts,
                                                                const CancellationToken &cancellation) const;
        /**
         * @brief Invokes one logical tool through host policy and the platform process boundary.
         * @param authority Generation-bound provider authority returned during registration.
         * @param intent Logical tool and bounded arguments requested by the caller.
         * @param cancellation Host-owned cancellation token.
         * @return Attributed platform-process result, or a typed failure.
         */
        [[nodiscard]] Result<ToolchainInvocationResult> InvokeToolchain(const ToolchainInvocationAuthority &authority,
                                                                        const ToolchainInvocationIntent &intent,
                                                                        const CancellationToken &cancellation) const;
        /**
         * @brief Resolves one versioned application capability under an admitted caller authority.
         * @param authority Unforgeable capability admission for the caller activation.
         * @param versions Closed provider-version interval accepted by the caller.
         * @param extensionId Exact caller extension identity.
         * @param moduleId Exact caller module identity.
         * @param activationGeneration Exact caller activation generation.
         * @return Compatible provider lease, or an attributed availability, version, admission, or lifecycle failure.
         */
        [[nodiscard]] Result<ApplicationCapabilityProviderLease> ResolveCapability(const ExtensionCapabilityHandle &authority,
                                                                                   const ApplicationCapabilityVersionRange &versions,
                                                                                   std::string_view extensionId, std::string_view moduleId,
                                                                                   std::uint64_t activationGeneration) const;

        /**
         * @brief Returns an owned, internally consistent inspection snapshot.
         * @return Lifecycle, discovery, activation, root-policy, and retained diagnostic state.
         */
        [[nodiscard]] HeadlessExtensionHostSnapshot Inspect() const;
        /** @brief Closes admission, cancels registry-owned work, and releases module ownership in teardown order. */
        void Shutdown() noexcept;

    private:
        struct Impl;
        explicit HeadlessExtensionHost(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Extensions
