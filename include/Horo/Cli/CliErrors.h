#pragma once

/**
 * @file CliErrors.h
 * @brief Stable typed failures emitted by CLI descriptor and registry validation.
 */

#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/ModuleDescriptor.h"

namespace Horo::Cli::CliErrors {
    /** @brief A descriptor or host policy contains malformed metadata. */
    extern const ErrorCodeDescriptor DescriptorInvalid;
    /** @brief Descriptor metadata exceeds a host-declared admission limit. */
    extern const ErrorCodeDescriptor RegistryCapacityExceeded;
    /** @brief More than one descriptor declares the same hierarchical command path. */
    extern const ErrorCodeDescriptor CommandPathDuplicate;
    /** @brief Option long names or short aliases collide within one command. */
    extern const ErrorCodeDescriptor OptionNameDuplicate;
    /** @brief An option declaration combines incompatible schema fields. */
    extern const ErrorCodeDescriptor OptionSchemaIncompatible;
    /** @brief An output declaration is unversioned or lacks required encodings. */
    extern const ErrorCodeDescriptor OutputSchemaIncompatible;
    /** @brief A descriptor requires a capability not granted by the composition root. */
    extern const ErrorCodeDescriptor CapabilityUnauthorized;
    /** @brief A descriptor is unavailable on the active executable host. */
    extern const ErrorCodeDescriptor HostUnsupported;
    /** @brief A descriptor targets a contract generation unsupported by the host. */
    extern const ErrorCodeDescriptor ContractVersionIncompatible;
    /** @brief Parser input or resource-limit policy is malformed. */
    extern const ErrorCodeDescriptor ParserPolicyInvalid;
    /** @brief No registered command matches the requested hierarchical path. */
    extern const ErrorCodeDescriptor CommandUnknown;
    /** @brief One or more command-line values failed bounded syntax validation. */
    extern const ErrorCodeDescriptor ParseFailed;
    /** @brief Explicit stdin selection is unsupported or conflicts with the command contract. */
    extern const ErrorCodeDescriptor InputModeUnsupported;
    /** @brief Explicit argv or stdin data exceeds a declared parser bound. */
    extern const ErrorCodeDescriptor InputCapacityExceeded;
    /** @brief Required interactive input is unavailable and its deterministic alternative was omitted. */
    extern const ErrorCodeDescriptor InteractiveInputUnavailable;
    /** @brief Dispatcher registration metadata is malformed or inconsistent with the accepted registry. */
    extern const ErrorCodeDescriptor DispatchRegistrationInvalid;
    /** @brief No executable adapter is bound to the validated command path. */
    extern const ErrorCodeDescriptor CommandUnavailable;
    /** @brief Invocation authority does not admit the command's declared side effects. */
    extern const ErrorCodeDescriptor SideEffectUnauthorized;
    /** @brief Invocation correlation, timeout, or execution bounds are malformed. */
    extern const ErrorCodeDescriptor ExecutionContextInvalid;
    /** @brief Cooperative cancellation was requested before a terminal result was produced. */
    extern const ErrorCodeDescriptor ExecutionCancelled;
    /** @brief The command exceeded its declared execution deadline. */
    extern const ErrorCodeDescriptor ExecutionTimedOut;
    /** @brief Adapter progress or result data exceeded a dispatcher-owned bound. */
    extern const ErrorCodeDescriptor ExecutionCapacityExceeded;
    /** @brief Unexpected process-host failure before a normal operation result is available. */
    extern const ErrorCodeDescriptor HostFailure;
    /** @brief Returns inert CLI-owned error metadata for explicit host composition. @return Canonical descriptor contribution. */
    [[nodiscard]] ModuleErrorDomainDescriptor ErrorDomain();
}  // namespace Horo::Cli::CliErrors
