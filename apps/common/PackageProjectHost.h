#pragma once
/** @file PackageProjectHost.h @brief Explicit asynchronous package project-binding and retirement owner. */
#include "PackageProjectSession.h"

namespace Horo::Application::Internal {
    /** @brief Explicit borrowed application services; all remain alive until host Close completes. */
    struct PackageProjectHostServices final {
        JobSystem &jobs;
        DurableFileSystem &files;
        const Clock &clock;
        const WallClock &wallClock;
        const Packages::IPackageExtensionTrust &trust;
    };

    /** @brief Fixed-capacity owner of one live/retiring project and one bounded metadata preparation.
     * @details The executable pumps this owner even without a modal or MCP call. Complete metadata samples
     * linearize policy generations. An observed change/error revokes old admission before any further activation.
     * Expired metadata permits only non-activating drainage until revalidation. Metadata documents are read only on owned I/O work. First
     * owner-lane composition retains existing canonical path admission.
     */
    class PackageProjectHost final {
    public:
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
        /** @brief Cancels admission and drains completed callbacks without waiting or destroying borrowed services.
         * @return Success once every owned callback/session is closed, otherwise typed busy. */
        [[nodiscard]] Result<void> Close();

    private:
        struct Impl;
        explicit PackageProjectHost(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Application::Internal
