#pragma once

#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Hosts/ErrorTranslation.h"

#include <array>
#include <stdexcept>

namespace HostErrorFixture {
    using namespace Horo;
    using namespace Horo::Hosts;

    inline const std::array<ExitCategory, 8> Categories{ExitCategory::Usage,     ExitCategory::Validation, ExitCategory::Capability,
                                                        ExitCategory::Operation, ExitCategory::Permission, ExitCategory::Cancelled,
                                                        ExitCategory::Timeout,   ExitCategory::Invariant};

    /** @brief Test-owned application identities; hosts never manufacture these identities. */
    inline const std::array<ErrorCodeDescriptor, 8> Descriptors = [] {
        std::array<ErrorCodeDescriptor, 8> descriptors;
        constexpr std::array names{"usage", "validation", "capability", "operation", "permission", "cancelled", "timeout", "invariant"};
        for (std::size_t index = 0; index < names.size(); ++index)
            descriptors[index] = {ErrorDomainId{"project.translation"},
                                  ErrorCode{std::string{"project.translation."} + names[index]},
                                  index == 7 ? ErrorSeverity::Critical : ErrorSeverity::Error,
                                  "Registered safe summary",
                                  "Registered remediation",
                                  index == 6,
                                  index == 1};
        return descriptors;
    }();

    inline const std::array<ErrorCodeDescriptor, 2>
        DiagnosticDescriptors{ErrorCodeDescriptor{ErrorDomainId{"project.translation"}, ErrorCode{"project.translation.finding"},
                                                  ErrorSeverity::Warning, "Registered diagnostic summary", "Inspect the finding"},
                              ErrorCodeDescriptor{ErrorDomainId{"project.translation"}, ErrorCode{"project.translation.other"},
                                                  ErrorSeverity::Info, "Registered informational summary", "Inspect the note"}};

    /** @brief Builds the exact immutable application registry used by C++ and Python parity tests. */
    inline ErrorCodeRegistry Registry() {
        ModuleDescriptor module{.id = ModuleId{"project.translation"}, .version = {1, 0, 0}};
        ModuleErrorDomainDescriptor domain{.id = ErrorDomainId{"project.translation"}};
        for (const auto &descriptor : Descriptors)
            domain.descriptors.push_back(&descriptor);
        for (const auto &descriptor : DiagnosticDescriptors)
            domain.descriptors.push_back(&descriptor);
        module.errorDomains.push_back(std::move(domain));
        auto registry = BuildErrorCodeRegistry(std::span{&module, 1});
        if (registry.HasError())
            throw std::runtime_error("Invalid host translation test registry");
        return std::move(registry).Value();
    }

    /** @brief Declares one explicit cross-host presentation category per test-owned application identity. */
    inline std::array<ErrorMapping, 8> Mappings() {
        std::array<ErrorMapping, 8> mappings;
        for (std::size_t index = 0; index < mappings.size(); ++index)
            mappings[index] = {Descriptors[index].domain, Descriptors[index].code, Categories[index]};
        return mappings;
    }

    /** @brief Constructs nested diagnostics with private operation text to exercise disclosure parity. */
    inline Error Failure(const std::size_t index) {
        Error error = MakeError(Descriptors[index], "Private credential and /home/private/project");
        error.diagnostics.push_back({DiagnosticCode{"project.translation.finding"},
                                     DiagnosticSeverity::Warning,
                                     "Private diagnostic text",
                                     {"private/project.json", 4, 7},
                                     "entities[0].id"});
        error.diagnostics.push_back({DiagnosticCode{"project.translation.other"}, DiagnosticSeverity::Note, "Second finding", {}, {}});
        return WithCause(std::move(error), MakeError(Descriptors[3], "Private cause detail"));
    }
}  // namespace HostErrorFixture
