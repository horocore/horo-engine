#pragma once
/** @file PackageProjectHost.h @brief Explicit asynchronous package project-binding and retirement owner. */
#include "PackageProjectSession.h"

namespace Horo::Application::Internal {
    /** @brief Fixed-capacity owner of one live/retiring project and one bounded metadata preparation.
     * @details The executable pumps this owner even without a modal or MCP call. Complete metadata samples
     * linearize policy generations. An observed change/error revokes old admission before any further activation.
     * Expired metadata permits only non-activating drainage until revalidation. Metadata documents are read only on owned I/O work. First
     * owner-lane composition retains existing canonical path admission.
     */
    class PackageProjectHost final {
        struct Impl;

    public:
        /** @brief Non-aggregate allocation authority issued only by the validating factory. */
        class ConstructionKey final {
            ConstructionKey() = default;
            friend class PackageProjectHost;
        };

        /** @brief Adopts factory-validated state through standard RAII allocation.
         * @param key Private factory authority. @param impl Complete validated state and its existing lifetime owners.
         * @details Clients continue to use Create; the allocation constructor cannot be called with a default key. */
        PackageProjectHost(const ConstructionKey &key, std::unique_ptr<Impl> impl) noexcept;

        /** @brief Binds explicit borrowed services and positive host-selected metadata age without discovering providers.
         * @param services Explicit borrowed scheduler, filesystem, clocks and execution trust.
         * @param policy Cache/platform/native policy.
         * @param metadataMaximumAge Positive maximum sampled metadata age, at most sixty seconds.
         * @param source Optional selected exact artifact transport. @param publishers Optional publisher verification.
         * @return Inert owner or typed invalid policy. */
        [[nodiscard]] static Result<std::unique_ptr<PackageProjectHost>> Create(
            PackageProjectHostServices services, PackageProjectPolicy policy, Duration metadataMaximumAge,
            Packages::IPackageRestoreSource *source = nullptr, Packages::PackagePublisherVerificationService *publishers = nullptr);
        ~PackageProjectHost() noexcept;
        /** @brief Admits one project binding or returns recoverable preparation/retirement status.
         * @param root Absolute normalized project root. @return Generation-bound application capability or original typed failure. */
        [[nodiscard]] Result<std::shared_ptr<PackageApplication>> Acquire(const std::filesystem::path &root);
        /** @brief Pumps completed metadata and the actual safe-point without waiting for worker completion.
         * @param boundary Actual execution boundary. @return Typed outcome; busy work remains owned. */
        [[nodiscard]] Result<void> Pump(Packages::PackageActivationBoundary boundary);
        /** @brief Applies a screen safe-point only to its exact current project binding.
         * @param root Screen project binding. @param boundary Actual play lifecycle boundary.
         * @return Typed stale-project failure or ordinary pump result. */
        [[nodiscard]] Result<void> PumpProject(const std::filesystem::path &root, Packages::PackageActivationBoundary boundary);
        /** @brief Revokes the selected project; the owner remains retained while callbacks drain.
         * @param root Exact project binding. @return Success or typed busy/foreign binding. */
        [[nodiscard]] Result<void> Retire(const std::filesystem::path &root);
        /** @brief Returns the current owner-lane project policy generation. @return Nonwrapping generation, zero before binding. */
        [[nodiscard]] std::uint64_t Generation() const;
        /** @brief Reads sealed availability for the exact current fresh project generation without pumping or activating it.
         * @param root Exact bound project. @param generation Previously captured canonical Generation().
         * @return Immutable installed graph lease, empty before installation, or original typed admission/stale failure.
         * @details A retained record owns bytes, not continuing host admission or execution trust. Native users must fence the
         * same generation again before activation and retain their own verified resources through asynchronous retirement.
         */
        [[nodiscard]] Result<std::shared_ptr<const Packages::VerifiedPackageInstallRecord>> InstalledRecord(
            const std::filesystem::path &root, std::uint64_t generation) const;
        /** @brief Cancels admission and drains completed callbacks without waiting or destroying borrowed services.
         * @return Success once every owned callback/session is closed, otherwise typed busy. */
        [[nodiscard]] Result<void> Close();

    private:
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Application::Internal
