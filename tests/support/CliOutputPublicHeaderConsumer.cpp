#include "Horo/Cli/CliErrors.h"
#include "Horo/Cli/CliOutputPresenter.h"
#include "Horo/Foundation/Diagnostics/DiagnosticBundle.h"

#include <sstream>

int main() {
    const auto errors = Horo::Cli::CliErrors::ErrorDomain();
    const auto bundles = Horo::Diagnostics::DiagnosticBundleErrorDomain();
    const std::vector<Horo::ModuleDescriptor> modules{{.id = {"horo.cli"}, .version = {1, 0, 0}, .errorDomains = {errors}},
                                                      {.id = {"horo.foundation"}, .version = {1, 0, 0}, .errorDomains = {bundles}}};
    auto registry = Horo::BuildErrorCodeRegistry(modules);
    if (registry.HasError())
        return 1;
    const Horo::Hosts::ErrorMapping mapping{Horo::Cli::CliErrors::HostFailure.domain, Horo::Cli::CliErrors::HostFailure.code,
                                            Horo::Hosts::ExitCategory::Invariant};
    auto translator = Horo::Hosts::ErrorTranslator::Create(std::move(registry).Value(), std::span{&mapping, 1});
    if (!translator)
        return 2;
    std::ostringstream output, diagnostics;
    Horo::Cli::CliOutputPresenter presenter({.path = {{"host", "inspect"}},
                                             .output = {.id = "horo.cli.host-result",
                                                        .version = 1,
                                                        .formats = Horo::Cli::CliOutputFormat::Human | Horo::Cli::CliOutputFormat::Json}},
                                            {1}, std::move(*translator), output, diagnostics, Horo::Cli::CliProgressOutputMode::Json,
                                            false);
    return presenter.HostFailure().exitCode == 1 && output.str().find("cli.host_failure") != std::string::npos ? 0 : 3;
}
