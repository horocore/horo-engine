#pragma once

/** @file CliHostErrorTranslation.h @brief Private exact error presentation composition for the headless host. */
#include "Horo/Hosts/ErrorTranslation.h"

namespace Horo::Application::Internal {
    /** @brief Composes real producer metadata and exact exit mappings without activating application services.
     * @return Owned strict translator, or no value if its explicit catalog is invalid.
     */
    [[nodiscard]] std::optional<Hosts::ErrorTranslator> MakeCliHostErrorTranslator();
}  // namespace Horo::Application::Internal
