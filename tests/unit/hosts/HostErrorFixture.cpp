#include "HostErrorFixture.h"

#include <iostream>
#include <nlohmann/json.hpp>

/** @brief Emits real C++ host envelopes for the Python exception parity integration test. */
int main() {
    const auto translator = Horo::Hosts::ErrorTranslator::Create(HostErrorFixture::Registry(), HostErrorFixture::Mappings());
    if (!translator)
        return 1;
    nlohmann::json fixtures = nlohmann::json::array();
    for (std::size_t index = 0; index < HostErrorFixture::Descriptors.size(); ++index) {
        const auto translated = translator->Translate(HostErrorFixture::Failure(index));
        if (!translated)
            return 1;
        const auto gui = Horo::Hosts::TranslateGuiError(*translated, Horo::Hosts::GuiErrorSurface::Workflow);
        const auto cli = Horo::Hosts::TranslateCliError(*translated);
        fixtures.push_back({{"cli", nlohmann::json::parse(cli.json)},
                            {"gui", nlohmann::json::parse(gui.detail.Json())},
                            {"mcp", nlohmann::json::parse(Horo::Hosts::TranslateMcpError(*translated))}});
    }
    std::cout << fixtures.dump() << '\n';
    return 0;
}
