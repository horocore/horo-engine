#include "Horo/Agent/ModelProvider.h"

#include <algorithm>
#include <utility>

namespace Horo::Agent {
    namespace {
        using enum ModelErrorCode;

        /** @brief Validate bounded conversation content and tool-result identity. */
        ModelOutcome AdmitMessages(const ModelRequest &request, ModelFeatures supported) {
            constexpr std::size_t kMaximumTools = 64;
            constexpr std::size_t kMaximumTextBytes = 1U << 20U;
            std::size_t textBytes = 0;
            for (const auto &message : request.messages) {
                if (message.text.size() > kMaximumTextBytes - textBytes || message.toolIntents.size() > kMaximumTools)
                    return {ModelError{InvalidRequest, "Model conversation exceeds limits", std::nullopt}};
                textBytes += message.text.size();
                if (message.role == ModelRole::Tool ? message.toolCallId.empty() : !message.toolCallId.empty())
                    return {ModelError{InvalidRequest, "Invalid tool result identity", std::nullopt}};
                if (!message.toolIntents.empty() && (message.role != ModelRole::Assistant || !supported.Has(ModelFeature::Tools)))
                    return {ModelError{UnsupportedCapability, "Assistant tool history requires tool support", std::nullopt}};
                for (const auto &intent : message.toolIntents) {
                    if (intent.callId.empty() || intent.callId.size() > 256 || intent.name.empty() || intent.name.size() > 256 ||
                        intent.argumentsJson.empty() || intent.argumentsJson.size() > (64U << 10U))
                        return {ModelError{InvalidRequest, "Incomplete assistant tool intent", std::nullopt}};
                }
            }
            return {};
        }

        /** @brief Reject invalid or oversized host-provided tool schemas before dispatch. */
        ModelOutcome AdmitTools(const ModelRequest &request) {
            for (const auto &tool : request.tools) {
                if (tool.name.empty() || tool.name.size() > 256 || tool.description.size() > 4096 || tool.parametersJson.empty() ||
                    tool.parametersJson.size() > (64U << 10U))
                    return {ModelError{InvalidRequest, "Tool name and parameter schema are required", std::nullopt}};
            }
            return {};
        }
    }  // namespace

    /** @copydoc ModelFeatures::Has */
    bool ModelFeatures::Has(ModelFeature feature) const noexcept {
        return (bits & static_cast<std::uint32_t>(feature)) != 0;
    }

    /** @copydoc ModelFeatures::Contains */
    bool ModelFeatures::Contains(ModelFeatures required) const noexcept {
        return (bits & required.bits) == required.bits;
    }

    /** @copydoc AdmitModelRequest */
    ModelOutcome AdmitModelRequest(const ModelRequest &request, ModelFeatures supported) {
        using enum ModelErrorCode;
        constexpr std::size_t kMaximumMessages = 128;
        constexpr std::size_t kMaximumTools = 64;
        if (!supported.Contains(request.requiredFeatures) || (!request.tools.empty() && !supported.Has(ModelFeature::Tools))) {
            return {ModelError{UnsupportedCapability, "Selected model does not support required features", std::nullopt}};
        }
        if (request.model.empty() || request.model.size() > 256 || request.messages.empty() || request.messages.size() > kMaximumMessages ||
            request.tools.size() > kMaximumTools || request.maximumOutputTokens == 0 || request.maximumOutputTokens > 131'072) {
            return {ModelError{InvalidRequest, "Model request exceeds shape or size limits", std::nullopt}};
        }
        if (const auto messages = AdmitMessages(request, supported); !messages.Succeeded())
            return messages;
        return AdmitTools(request);
    }

    /** @copydoc ModelProviderRegistry::Register */
    bool ModelProviderRegistry::Register(std::string id, Factory factory) {
        if (id.empty() || !factory || std::ranges::any_of(m_entries, [&](const Entry &entry) {
            return entry.id == id;
        })) {
            return false;
        }
        m_entries.emplace_back(std::move(id), std::move(factory));
        return true;
    }

    /** @copydoc ModelProviderRegistry::ProviderIds */
    std::vector<std::string> ModelProviderRegistry::ProviderIds() const {
        std::vector<std::string> ids;
        ids.reserve(m_entries.size());
        for (const auto &entry : m_entries) {
            ids.emplace_back(entry.id);
        }
        return ids;
    }

    /** @copydoc ModelProviderRegistry::Create */
    std::unique_ptr<IModelProvider> ModelProviderRegistry::Create(std::string_view id, const ModelProviderConfig &config) const {
        const auto entry = std::ranges::find_if(m_entries, [&](const Entry &candidate) {
            return candidate.id == id;
        });
        return entry == m_entries.end() ? nullptr : entry->factory(config);
    }
}  // namespace Horo::Agent
