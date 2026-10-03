#include "Horo/Application/NavigationBakeDiagnostics.h"
#include "Horo/Application/NavigationBakeService.h"

int main() {
    Horo::Application::NavigationBakeDiagnosticsConfig config;
    const auto rejected = Horo::Application::NavigationBakeDiagnostics::Create(config);
    const Horo::Application::NavigationBakeDiagnosticRecord record;
    const Horo::Application::NavigationDiagnosticTarget target;
    const Horo::Application::NavigationBakeSourceAuthority authority;
    const auto invalidSource = authority.UpdateCurrent({}, {});
    return rejected.HasError() && invalidSource.HasError() && !target.asset.IsValid() && record.result == Horo::BuildOutputResult::None ? 0
                                                                                                                                        : 1;
}
