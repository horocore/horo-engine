#pragma once

/**
 * @file ReferenceModelProviders.h
 * @brief Host-selected local Ollama and cloud OpenAI-compatible model adapters.
 */

#include "Horo/Agent/ModelProvider.h"

namespace Horo::Agent {
    /** @brief Resolve an opaque credential reference at dispatch time; secrets are never persisted in model values. */
    using ModelCredentialResolver = std::function<std::optional<std::string>(std::string_view)>;

    /**
     * @brief Construct the real Ollama HTTP adapter; the host supplies its local endpoint.
     * @param config Endpoint and request timeout; credential reference is unused.
     * @return Adapter ready for explicit discovery or inference.
     */
    [[nodiscard]] std::unique_ptr<IModelProvider> CreateOllamaModelProvider(ModelProviderConfig config);

    /**
     * @brief Construct the real OpenAI-compatible HTTP adapter.
     * @param config Cloud HTTPS endpoint, opaque credential reference, and timeout.
     * @param resolver Host-owned secret lookup invoked only for an actual HTTP operation.
     * @return Adapter ready for explicit discovery or inference.
     */
    [[nodiscard]] std::unique_ptr<IModelProvider> CreateOpenAIModelProvider(ModelProviderConfig config, ModelCredentialResolver resolver);
}  // namespace Horo::Agent
