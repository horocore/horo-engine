#pragma once

/**
 * @file ReferenceModelProviders.h
 * @brief Host-selected local Ollama and cloud OpenAI-compatible model adapters.
 */

#include "Horo/Agent/ModelProvider.h"

namespace Horo::Security {
    class CredentialProvider;
}

namespace Horo::Agent {
    /** @brief Resolve an opaque credential reference at dispatch time; secrets are never persisted in model values. */
    using ModelCredentialResolver = std::function<std::optional<std::string>(std::string_view)>;

    /**
     * @brief Construct the real Ollama HTTP adapter; the host supplies its local endpoint.
     * @param config Endpoint and request timeout; credential reference is unused.
     * @return Adapter ready for explicit discovery or inference.
     */
    [[nodiscard]] std::unique_ptr<IModelProvider> CreateOllamaModelProvider(ModelProviderConfig config);

    /** @brief Checks whether an exact installed Ollama model reports tool support before chat dispatch.
     * @param config Local endpoint and time limit. @param model Exact installed model identifier.
     * @param cancellation Worker cancellation. @return Success only when the local endpoint confirms tool support. */
    [[nodiscard]] ModelOutcome ConfirmOllamaModelTools(const ModelProviderConfig &config, std::string_view model,
                                                       std::stop_token cancellation);

    /**
     * @brief Construct the real OpenAI-compatible HTTP adapter.
     * @param config Cloud HTTPS endpoint, opaque credential reference, and timeout.
     * @param resolver Host-owned secret lookup invoked only for an actual HTTP operation.
     * @return Adapter ready for explicit discovery or inference.
     */
    [[nodiscard]] std::unique_ptr<IModelProvider> CreateOpenAIModelProvider(ModelProviderConfig config, ModelCredentialResolver resolver);

    /** @brief Construct an HTTPS adapter with host-owned credential lookup.
     * @param config HTTPS endpoint and serialized CredentialReference.
     * @param credentials Shared provider whose expiry clock uses steady-clock milliseconds.
     * @return Adapter resolving move-only credential bytes per operation. Unapproved schemes and malformed references fail before lookup.
     * @details The host coordinates concurrent credential calls and obtains transmission consent before dispatch.
     */
    [[nodiscard]] std::unique_ptr<IModelProvider> CreateOpenAIModelProviderWithCredentials(
        ModelProviderConfig config, std::shared_ptr<Security::CredentialProvider> credentials);
}  // namespace Horo::Agent
