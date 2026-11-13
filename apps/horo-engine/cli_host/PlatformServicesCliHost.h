#pragma once

/** @file PlatformServicesCliHost.h @brief Production headless validation and paired cooking over shared application use cases. */
#include "Horo/Application/PlatformServicesProjectCook.h"
#include "Horo/Application/PlatformServicesProjectSources.h"
#include "Horo/Cli/CliDispatcher.h"

namespace Horo::Application::Internal {
    /** @brief Returns inert metadata for explicit Platform Services source validation. @return Headless command descriptor. */
    [[nodiscard]] Cli::CliCommandDescriptor DescribePlatformServicesValidationCommand();
    /** @brief Binds an immutable host-selected policy without discovery or provider activation.
     * @param context Actual host profile, trusted inert contributions and finite validation limits, copied into the adapter.
     * @return Owned adapter and its explicit command capability; source text cannot choose the host policy.
     */
    [[nodiscard]] Cli::CliCommandAdapterRegistration MakePlatformServicesValidationCommandAdapter(
        PlatformServicesProjectValidationContext context);
    /** @brief Returns inert metadata for paired derived cook publication. @return Explicit writes-files command descriptor. */
    [[nodiscard]] Cli::CliCommandDescriptor DescribePlatformServicesCookCommand();
    /** @brief Binds the shared cook publisher to host-selected policy and durable authority.
     * @param context Explicit Cook profile and actual linked trusted contributions.
     * @param files Host-selected durable writer retained through adapter destruction; empty fails without fallback.
     * @return Owned adapter and explicit cook capability. This host has no private mapping source and refuses exact providers.
     */
    [[nodiscard]] Cli::CliCommandAdapterRegistration MakePlatformServicesCookCommandAdapter(
        PlatformServicesProjectValidationContext context, std::shared_ptr<DurableFileSystem> files);
}  // namespace Horo::Application::Internal
