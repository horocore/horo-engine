#pragma once

/**
 * @file ModelProvider.h
 * @brief Vendor-neutral editor model inference, discovery, and admission contract.
 */

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Agent {
    /** @brief Independently negotiable model features. */
    enum class ModelFeature : std::uint32_t {
        Streaming = 1U << 0U,
        Tools = 1U << 1U,
        Usage = 1U << 2U,
    };

    /** @brief Compact set of features with no implicit emulation. */
    struct ModelFeatures final {
        std::uint32_t bits{};

        /** @brief Whether this set contains a named feature. */
        [[nodiscard]] bool Has(ModelFeature feature) const noexcept;
        /** @brief Whether this set contains every feature in another set. */
        [[nodiscard]] bool Contains(ModelFeatures required) const noexcept;
    };

    /** @brief Typed author or tool-result role for a conversation message. */
    enum class ModelRole {
        System,
        User,
        Assistant,
        Tool
    };

    /** @brief Complete tool intent emitted after all its streamed fragments are assembled. */
    struct ModelToolIntent final {
        std::string callId;
        std::string name;
        std::string argumentsJson;
    };

    /** @brief One copied message; tool results name the matching call ID and assistant turns retain tool intents. */
    struct ModelMessage final {
        ModelRole role{ModelRole::User};
        std::string text;
        std::string toolCallId;
        std::vector<ModelToolIntent> toolIntents;
    };

    /** @brief Tool schema and its stable name, both provided by admitted host policy. */
    struct ModelTool final {
        std::string name;
        std::string description;
        std::string parametersJson;
    };

    /** @brief One bounded model request; no provider credential or native SDK value is stored here. */
    struct ModelRequest final {
        std::string model;
        std::vector<ModelMessage> messages;
        std::vector<ModelTool> tools;
        ModelFeatures requiredFeatures{static_cast<std::uint32_t>(ModelFeature::Streaming)};
        std::uint32_t maximumOutputTokens{1024};
    };

    /** @brief Provider-reported token counts; absent values are never estimated. */
    struct ModelUsage final {
        std::optional<std::uint64_t> inputTokens;
        std::optional<std::uint64_t> outputTokens;
    };

    /** @brief The meaning of one synchronous stream callback delivery. */
    enum class ModelEventKind {
        TextDelta,
        ToolIntent,
        Usage,
        Completed
    };

    /** @brief One ordered event, valid only for the duration of its callback. */
    struct ModelEvent final {
        ModelEventKind kind{ModelEventKind::TextDelta};
        std::string text;
        ModelToolIntent tool;
        ModelUsage usage;
    };

    /** @brief Stable failure class for admission, transport, protocol, and cancellation. */
    enum class ModelErrorCode {
        InvalidRequest,
        UnsupportedCapability,
        UnknownProvider,
        CredentialUnavailable,
        Unavailable,
        RateLimited,
        Authentication,
        Transport,
        Protocol,
        Cancelled,
    };

    /** @brief Sanitized failure; messages must not contain response bodies or credentials. */
    struct ModelError final {
        ModelErrorCode code{ModelErrorCode::Protocol};
        std::string message;
        std::optional<long> httpStatus;
    };

    /** @brief Exact result of a synchronous stream or discovery operation. */
    struct ModelOutcome final {
        std::optional<ModelError> error;

        /** @brief Whether the operation completed without a provider or callback failure. */
        [[nodiscard]] bool Succeeded() const noexcept {
            return !error.has_value();
        }
    };

    /** @brief Model discovered from the selected provider endpoint. */
    struct ModelDescriptor final {
        std::string id;
        ModelFeatures features;
        std::optional<std::uint32_t> contextTokens;
    };

    /** @brief Network location of admitted inference data. */
    enum class ModelResidency {
        OnDevice,
        External
    };

    /** @brief Cost category for host policy and UI; no price estimate is implied. */
    enum class ModelCostClass {
        LocalCompute,
        Metered
    };

    /** @brief Provider identity, availability, and models at one discovery point. */
    struct ProviderDiscovery final {
        std::string id;
        ModelFeatures features;
        std::vector<ModelDescriptor> models;
        ModelOutcome outcome;
        ModelResidency residency{ModelResidency::OnDevice};
        ModelCostClass costClass{ModelCostClass::LocalCompute};
    };

    /** @brief Host-owned provider configuration with an opaque credential lookup key. */
    struct ModelProviderConfig final {
        std::string endpoint;
        std::string credentialReference;
        std::chrono::milliseconds timeout{30'000};
        ModelFeatures enabledFeatures{
            static_cast<std::uint32_t>(ModelFeature::Streaming) |
            static_cast<std::uint32_t>(ModelFeature::Usage)}; /**< Host-confirmed features; tools require opt-in. */
    };

    /** @brief Synchronous observer; false requests cancellation before further delivery. */
    using ModelEventSink = std::function<bool(const ModelEvent &)>;

    /**
     * @brief Provider-neutral streaming and discovery seam owned by the editor agent.
     * @details Calls run synchronously on their caller threads. The host retains the provider until all calls return and
     * coordinates a shared credential resolver when calling concurrently; cancelling a token interrupts active I/O.
     */
    class IModelProvider {
    public:
        virtual ~IModelProvider() = default;

        /** @brief Discover available models through the configured real endpoint. */
        [[nodiscard]] virtual ProviderDiscovery Discover(std::stop_token cancellation) = 0;

        /**
         * @brief Stream one admitted request in caller thread, with a single terminal Completed event on success.
         * @param request Copied conversation and required features; tool execution stays with the caller.
         * @param sink Synchronous ordered observer; false cancels the operation.
         * @param cancellation Cooperative token checked during I/O and event delivery.
         * @return Sanitized outcome. Failure never emits Completed.
         */
        [[nodiscard]] virtual ModelOutcome Stream(const ModelRequest &request, const ModelEventSink &sink,
                                                  std::stop_token cancellation) = 0;
    };

    /** @brief Validate request shape and explicit feature negotiation before any provider I/O. */
    [[nodiscard]] ModelOutcome AdmitModelRequest(const ModelRequest &request, ModelFeatures supported);

    /** @brief Explicit composition registry; entries do not activate during registration. */
    class ModelProviderRegistry final {
    public:
        using Factory = std::function<std::unique_ptr<IModelProvider>(const ModelProviderConfig &)>;

        /** @brief Register one stable provider ID and factory; rejects duplicates or empty inputs. */
        [[nodiscard]] bool Register(std::string id, Factory factory);
        /** @brief List registered IDs in registration order without constructing adapters. */
        [[nodiscard]] std::vector<std::string> ProviderIds() const;
        /** @brief Construct the selected adapter with host-provided configuration. */
        [[nodiscard]] std::unique_ptr<IModelProvider> Create(std::string_view id, const ModelProviderConfig &config) const;

    private:
        struct Entry final {
            std::string id;
            Factory factory;
        };

        std::vector<Entry> m_entries;
    };
}  // namespace Horo::Agent
