#include "Horo/Agent/ReferenceModelProviders.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <curl/curl.h>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

namespace Horo::Agent {
    namespace {
        using Json = nlohmann::json;
        constexpr ModelFeatures kProtocolFeatures{static_cast<std::uint32_t>(ModelFeature::Streaming) |
                                                  static_cast<std::uint32_t>(ModelFeature::Tools) |
                                                  static_cast<std::uint32_t>(ModelFeature::Usage)};
        constexpr std::size_t kMaximumLineBytes = 256U << 10U;
        constexpr std::size_t kMaximumDiscoveryBytes = 1U << 20U;

        enum class Protocol {
            Ollama,
            OpenAI
        };

        /** @brief Destroy the ephemeral credential bytes after each discovery or inference dispatch. */
        struct SensitiveCredential final {
            std::optional<std::string> value;

            ~SensitiveCredential() {
                if (value)
                    std::fill(value->begin(), value->end(), '\0');
            }

            [[nodiscard]] bool Invalid() const {
                return value && (value->empty() || std::ranges::any_of(*value, [](unsigned char c) {
                    return c < 0x20 || c == 0x7f;
                }));
            }
        };

        /** @brief Keep URL validation simple and reject implicit redirect or credential-bearing endpoints. */
        bool ValidEndpoint(const std::string &endpoint, Protocol protocol) {
            const bool secure = endpoint.starts_with("https://");
            const bool local = endpoint.starts_with("http://localhost:") || endpoint.starts_with("http://127.0.0.1:") ||
                               endpoint.starts_with("http://[::1]:");
            if (endpoint.size() < 9 || !(secure || local) || (protocol == Protocol::Ollama && !local) ||
                endpoint.find('@') != std::string::npos || endpoint.find('?') != std::string::npos ||
                endpoint.find('#') != std::string::npos) {
                return false;
            }
            return true;
        }

        /** @brief Map an HTTP status without retaining remote response text. */
        ModelError HttpError(long status) {
            ModelErrorCode code = ModelErrorCode::Unavailable;
            if (status == 401 || status == 403)
                code = ModelErrorCode::Authentication;
            if (status == 429)
                code = ModelErrorCode::RateLimited;
            if (status == 400 || status == 422)
                code = ModelErrorCode::InvalidRequest;
            return {code, "Model provider returned HTTP " + std::to_string(status), status};
        }

        /** @brief Copy a numeric usage field only when the provider actually reported it. */
        std::optional<std::uint64_t> UsageField(const Json &object, std::string_view key) {
            if (!object.is_object())
                return std::nullopt;
            auto field = object.find(std::string(key));
            if (field == object.end() || !field->is_number_unsigned())
                return std::nullopt;
            return field->get<std::uint64_t>();
        }

        /** @brief Forward one event while preserving sink cancellation as an explicit outcome. */
        bool Deliver(const ModelEventSink &sink, const ModelEvent &event, std::stop_token stop) {
            return !stop.stop_requested() && sink(event);
        }

        /** @brief Perform one real bounded HTTP request with a per-line or buffered response consumer. */
        ModelOutcome HttpRequest(const ModelProviderConfig &config, Protocol protocol, std::string_view path, const std::string *body,
                                 const std::string *bearer, std::stop_token stop, const std::function<bool(std::string_view)> &consume) {
            if (!ValidEndpoint(config.endpoint, protocol) || config.timeout.count() <= 0 ||
                config.timeout.count() > std::numeric_limits<long>::max() || !consume) {
                return {ModelError{ModelErrorCode::InvalidRequest, "Invalid model provider endpoint or timeout", std::nullopt}};
            }
            if (stop.stop_requested())
                return {ModelError{ModelErrorCode::Cancelled, "Model request cancelled", std::nullopt}};

            static const CURLcode curlInit = curl_global_init(CURL_GLOBAL_DEFAULT);
            if (curlInit != CURLE_OK)
                return {ModelError{ModelErrorCode::Transport, "Cannot initialize model HTTP runtime", std::nullopt}};
            CURL *curl = curl_easy_init();
            if (curl == nullptr)
                return {ModelError{ModelErrorCode::Transport, "Cannot initialize model HTTP client", std::nullopt}};
            curl_slist *headers = nullptr;
            headers = curl_slist_append(headers, "Accept: application/json");
            if (body != nullptr)
                headers = curl_slist_append(headers, "Content-Type: application/json");
            std::string authorization;
            if (bearer != nullptr) {
                authorization = "Authorization: Bearer " + *bearer;
                headers = curl_slist_append(headers, authorization.c_str());
            }

            struct Context final {
                std::string pending;
                const std::function<bool(std::string_view)> &consume;
                std::stop_token stop;
                CURL *curl{};
                bool cancelled{};
                bool oversized{};
                bool invalid{};
            } context{{}, consume, stop, curl};

            const auto write = +[](char *data, std::size_t size, std::size_t count, void *user) -> std::size_t {
                auto &state = *static_cast<Context *>(user);
                if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
                    state.oversized = true;
                    return 0;
                }
                const std::size_t bytes = size * count;
                if (state.stop.stop_requested()) {
                    state.cancelled = true;
                    return 0;
                }
                long responseStatus = 0;
                curl_easy_getinfo(state.curl, CURLINFO_RESPONSE_CODE, &responseStatus);
                if (responseStatus < 200 || responseStatus >= 300)
                    return bytes;
                if (bytes > kMaximumDiscoveryBytes || state.pending.size() > kMaximumDiscoveryBytes - bytes) {
                    state.oversized = true;
                    return 0;
                }
                state.pending.append(data, bytes);
                std::size_t newline;
                while ((newline = state.pending.find('\n')) != std::string::npos) {
                    bool accepted = false;
                    try {
                        accepted = newline <= kMaximumLineBytes && state.consume(std::string_view(state.pending).substr(0, newline));
                    } catch (...) {
                        state.invalid = true;
                    }
                    if (!accepted) {
                        state.oversized = newline > kMaximumLineBytes;
                        return 0;
                    }
                    state.pending.erase(0, newline + 1);
                }
                return bytes;
            };
            const auto progress = +[](void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                return static_cast<Context *>(user)->stop.stop_requested() ? 1 : 0;
            };

            std::string url = config.endpoint;
            if (url.ends_with('/'))
                url.pop_back();
            url += path;
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config.timeout.count()));
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, std::min(5000L, static_cast<long>(config.timeout.count())));
            if (url.starts_with("https://"))
                curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
            if (body != nullptr) {
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->data());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body->size()));
            }
            const CURLcode result = curl_easy_perform(curl);
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            std::fill(authorization.begin(), authorization.end(), '\0');
            if (stop.stop_requested() || context.cancelled) {
                return {ModelError{ModelErrorCode::Cancelled, "Model request cancelled", std::nullopt}};
            }
            if (context.oversized)
                return {ModelError{ModelErrorCode::Protocol, "Model response exceeds limit", std::nullopt}};
            if (context.invalid)
                return {ModelError{ModelErrorCode::Protocol, "Invalid model response", std::nullopt}};
            if (result != CURLE_OK)
                return {ModelError{ModelErrorCode::Transport, "Model HTTP transport failed", std::nullopt}};
            if (status < 200 || status >= 300)
                return {HttpError(status)};
            if (!context.pending.empty()) {
                try {
                    if (!consume(context.pending))
                        return {ModelError{ModelErrorCode::Protocol, "Invalid final model response fragment", std::nullopt}};
                } catch (...) {
                    return {ModelError{ModelErrorCode::Protocol, "Invalid final model response fragment", std::nullopt}};
                }
            }
            return {};
        }

        /** @brief Build a provider-native request from neutral typed messages and tools. */
        Json RequestJson(const ModelRequest &request, Protocol protocol) {
            Json messages = Json::array();
            for (const auto &message : request.messages) {
                std::string role = "user";
                if (message.role == ModelRole::System)
                    role = "system";
                if (message.role == ModelRole::Assistant)
                    role = "assistant";
                if (message.role == ModelRole::Tool)
                    role = "tool";
                Json item = {{"role", role}, {"content", message.text}};
                if (message.role == ModelRole::Tool && protocol == Protocol::OpenAI)
                    item["tool_call_id"] = message.toolCallId;
                if (!message.toolIntents.empty()) {
                    Json calls = Json::array();
                    for (const auto &intent : message.toolIntents) {
                        const auto arguments = Json::parse(intent.argumentsJson, nullptr, false);
                        if (!arguments.is_object())
                            return nullptr;
                        Json call = {{"id", intent.callId}, {"type", "function"}};
                        call["function"] = {{"name", intent.name},
                                            {"arguments", protocol == Protocol::OpenAI ? Json(intent.argumentsJson) : arguments}};
                        calls.push_back(std::move(call));
                    }
                    item["tool_calls"] = std::move(calls);
                }
                messages.push_back(std::move(item));
            }
            Json payload = {{"model", request.model}, {"messages", std::move(messages)}, {"stream", true}};
            if (protocol == Protocol::OpenAI) {
                payload["max_completion_tokens"] = request.maximumOutputTokens;
                payload["stream_options"] = {{"include_usage", true}};
            } else {
                payload["options"] = {{"num_predict", request.maximumOutputTokens}};
            }
            if (!request.tools.empty()) {
                Json tools = Json::array();
                for (const auto &tool : request.tools) {
                    const auto schema = Json::parse(tool.parametersJson, nullptr, false);
                    if (schema.is_discarded() || !schema.is_object())
                        return nullptr;
                    tools.push_back({{"type", "function"},
                                     {"function", {{"name", tool.name}, {"description", tool.description}, {"parameters", schema}}}});
                }
                payload["tools"] = std::move(tools);
            }
            return payload;
        }

        /** @brief Streaming parser state for both real provider wire formats. */
        class StreamParser final {
        public:
            StreamParser(Protocol protocol, const ModelEventSink &sink, std::stop_token stop, std::uint64_t invocation)
                : m_protocol(protocol), m_sink(sink), m_stop(stop), m_invocation(invocation) {}

            bool Line(std::string_view line) {
                if (line.empty() || line == "\r")
                    return true;
                if (m_protocol == Protocol::OpenAI) {
                    if (!line.starts_with("data: "))
                        return true;
                    line.remove_prefix(6);
                    if (line == "[DONE]" || line == "[DONE]\r") {
                        m_done = true;
                        return true;
                    }
                }
                const Json frame = Json::parse(line, nullptr, false);
                if (frame.is_discarded() || !frame.is_object()) {
                    m_error = true;
                    return false;
                }
                return m_protocol == Protocol::Ollama ? OllamaFrame(frame) : OpenAIFrame(frame);
            }

            [[nodiscard]] ModelOutcome Finish() {
                if (m_stop.stop_requested() || m_cancelled)
                    return {ModelError{ModelErrorCode::Cancelled, "Model request cancelled", std::nullopt}};
                if (m_error || !m_done)
                    return {ModelError{ModelErrorCode::Protocol, "Incomplete or invalid model stream", std::nullopt}};
                for (const auto &[index, tool] : m_tools) {
                    (void)index;
                    const auto arguments = Json::parse(tool.argumentsJson, nullptr, false);
                    if (tool.callId.empty() || tool.name.empty() || !arguments.is_object()) {
                        return {ModelError{ModelErrorCode::Protocol, "Incomplete tool intent", std::nullopt}};
                    }
                    if (!Emit({ModelEventKind::ToolIntent, {}, tool, {}}))
                        break;
                }
                if (m_cancelled)
                    return {ModelError{ModelErrorCode::Cancelled, "Model request cancelled", std::nullopt}};
                if (m_usage.inputTokens || m_usage.outputTokens) {
                    if (!Emit({ModelEventKind::Usage, {}, {}, m_usage}))
                        return {ModelError{ModelErrorCode::Cancelled, "Model request cancelled", std::nullopt}};
                }
                if (!Emit({ModelEventKind::Completed, {}, {}, {}}))
                    return {ModelError{ModelErrorCode::Cancelled, "Model request cancelled", std::nullopt}};
                return {};
            }

            [[nodiscard]] bool Invalid() const noexcept {
                return m_error;
            }

            [[nodiscard]] bool Cancelled() const noexcept {
                return m_cancelled;
            }

        private:
            bool Emit(const ModelEvent &event) {
                if (event.kind == ModelEventKind::TextDelta) {
                    if (event.text.size() > kMaximumOutputBytes - m_outputBytes) {
                        m_error = true;
                        return false;
                    }
                    m_outputBytes += event.text.size();
                }
                if (!Deliver(m_sink, event, m_stop)) {
                    m_cancelled = true;
                    return false;
                }
                return true;
            }

            bool OllamaFrame(const Json &frame) {
                if (frame.contains("error")) {
                    m_error = true;
                    return false;
                }
                if (frame.contains("message") && frame["message"].is_object()) {
                    const auto &message = frame["message"];
                    if (message.contains("content") && message["content"].is_string()) {
                        const std::string text = message["content"].get<std::string>();
                        if (!text.empty() && !Emit({ModelEventKind::TextDelta, text, {}, {}}))
                            return false;
                    }
                    if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
                        for (const auto &call : message["tool_calls"]) {
                            if (!call.is_object() || !call.contains("function") || !call["function"].is_object()) {
                                m_error = true;
                                return false;
                            }
                            const auto &function = call["function"];
                            if (!function.value("name", Json()).is_string() || !function.contains("arguments")) {
                                m_error = true;
                                return false;
                            }
                            if (call.contains("id") && !call["id"].is_string()) {
                                m_error = true;
                                return false;
                            }
                            std::string callId = call.value("id", std::string{});
                            if (callId.empty()) {
                                callId = "ollama-" + std::to_string(m_invocation) + "-" + std::to_string(m_tools.size());
                            }
                            ModelToolIntent tool{std::move(callId), function["name"].get<std::string>(),
                                                 function["arguments"].is_string() ? function["arguments"].get<std::string>()
                                                                                   : function["arguments"].dump()};
                            if (tool.argumentsJson.size() > kMaximumLineBytes || m_tools.size() >= 64) {
                                m_error = true;
                                return false;
                            }
                            m_tools.emplace(m_tools.size(), std::move(tool));
                        }
                    }
                }
                if (frame.contains("done") && !frame["done"].is_boolean()) {
                    m_error = true;
                    return false;
                }
                if (frame.value("done", false)) {
                    m_done = true;
                    m_usage.inputTokens = UsageField(frame, "prompt_eval_count");
                    m_usage.outputTokens = UsageField(frame, "eval_count");
                }
                return true;
            }

            bool OpenAIFrame(const Json &frame) {
                if (frame.contains("error")) {
                    m_error = true;
                    return false;
                }
                if (frame.contains("usage") && frame["usage"].is_object()) {
                    m_usage.inputTokens = UsageField(frame["usage"], "prompt_tokens");
                    m_usage.outputTokens = UsageField(frame["usage"], "completion_tokens");
                }
                if (!frame.contains("choices") || !frame["choices"].is_array())
                    return true;
                for (const auto &choice : frame["choices"]) {
                    if (!choice.is_object() || !choice.contains("delta") || !choice["delta"].is_object())
                        continue;
                    const auto &delta = choice["delta"];
                    if (delta.contains("content") && delta["content"].is_string()) {
                        const std::string text = delta["content"].get<std::string>();
                        if (!text.empty() && !Emit({ModelEventKind::TextDelta, text, {}, {}}))
                            return false;
                    }
                    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                        for (const auto &fragment : delta["tool_calls"]) {
                            if (!fragment.is_object() || !fragment.value("index", Json()).is_number_unsigned()) {
                                m_error = true;
                                return false;
                            }
                            const auto index = fragment["index"].get<std::size_t>();
                            if (index >= 64) {
                                m_error = true;
                                return false;
                            }
                            auto &tool = m_tools[index];
                            if (m_tools.size() > 64) {
                                m_error = true;
                                return false;
                            }
                            if (fragment.contains("id") && fragment["id"].is_string())
                                tool.callId = fragment["id"].get<std::string>();
                            if (fragment.contains("function") && fragment["function"].is_object()) {
                                const auto &function = fragment["function"];
                                if (function.contains("name") && function["name"].is_string())
                                    tool.name += function["name"].get<std::string>();
                                if (function.contains("arguments") && function["arguments"].is_string())
                                    tool.argumentsJson += function["arguments"].get<std::string>();
                                if (tool.name.size() > 256 || tool.argumentsJson.size() > kMaximumLineBytes) {
                                    m_error = true;
                                    return false;
                                }
                            }
                        }
                    }
                }
                return true;
            }

            Protocol m_protocol;
            static constexpr std::size_t kMaximumOutputBytes = 8U << 20U;
            const ModelEventSink &m_sink;
            std::stop_token m_stop;
            std::uint64_t m_invocation{};
            std::map<std::size_t, ModelToolIntent> m_tools;
            ModelUsage m_usage;
            bool m_done{};
            bool m_error{};
            bool m_cancelled{};
            std::size_t m_outputBytes{};
        };

        /** @brief One concrete HTTP adapter instance; protocol wire types stay target-private. */
        class ReferenceProvider final : public IModelProvider {
        public:
            ReferenceProvider(Protocol protocol, ModelProviderConfig config, ModelCredentialResolver resolver)
                : m_protocol(protocol), m_config(std::move(config)), m_resolver(std::move(resolver)) {
                m_config.enabledFeatures.bits &= kProtocolFeatures.bits;
            }

            /** @copydoc IModelProvider::Discover */
            ProviderDiscovery Discover(std::stop_token cancellation) override {
                ProviderDiscovery discovery{m_protocol == Protocol::Ollama ? "ollama" : "openai",
                                            m_config.enabledFeatures,
                                            {},
                                            {},
                                            m_protocol == Protocol::Ollama ? ModelResidency::OnDevice : ModelResidency::External,
                                            m_protocol == Protocol::Ollama ? ModelCostClass::LocalCompute : ModelCostClass::Metered};
                std::string body;
                SensitiveCredential credential{ResolveCredential()};
                if (credential.Invalid()) {
                    discovery.outcome = {ModelError{ModelErrorCode::CredentialUnavailable, "Model credential unavailable", std::nullopt}};
                    return discovery;
                }
                discovery.outcome = HttpRequest(m_config, m_protocol, m_protocol == Protocol::Ollama ? "/api/tags" : "/v1/models", nullptr,
                                                credential.value ? &*credential.value : nullptr, cancellation, [&](std::string_view line) {
                    body.append(line);
                    return body.size() <= kMaximumDiscoveryBytes;
                });
                if (!discovery.outcome.Succeeded())
                    return discovery;
                const Json response = Json::parse(body, nullptr, false);
                const auto key = m_protocol == Protocol::Ollama ? "models" : "data";
                if (!response.is_object() || !response.contains(key) || !response[key].is_array()) {
                    discovery.outcome = {ModelError{ModelErrorCode::Protocol, "Invalid model discovery response", std::nullopt}};
                    return discovery;
                }
                for (const auto &item : response[key]) {
                    if (!item.is_object())
                        continue;
                    const auto nameKey = m_protocol == Protocol::Ollama ? "name" : "id";
                    if (!item.contains(nameKey) || !item[nameKey].is_string())
                        continue;
                    discovery.models.push_back({item[nameKey].get<std::string>(), m_config.enabledFeatures, std::nullopt});
                }
                return discovery;
            }

            /** @copydoc IModelProvider::Stream */
            ModelOutcome Stream(const ModelRequest &request, const ModelEventSink &sink, std::stop_token cancellation) override {
                auto admitted = AdmitModelRequest(request, m_config.enabledFeatures);
                if (!admitted.Succeeded())
                    return admitted;
                if (!sink)
                    return {ModelError{ModelErrorCode::InvalidRequest, "Model event sink is required", std::nullopt}};
                const Json payload = RequestJson(request, m_protocol);
                if (payload.is_null())
                    return {ModelError{ModelErrorCode::InvalidRequest, "Invalid tool parameter schema", std::nullopt}};
                SensitiveCredential credential{ResolveCredential()};
                if (credential.Invalid())
                    return {ModelError{ModelErrorCode::CredentialUnavailable, "Model credential unavailable", std::nullopt}};
                const std::string body = payload.dump();
                StreamParser parser(m_protocol, sink, cancellation, m_nextInvocation.fetch_add(1, std::memory_order_relaxed));
                const auto outcome =
                    HttpRequest(m_config, m_protocol, m_protocol == Protocol::Ollama ? "/api/chat" : "/v1/chat/completions", &body,
                                credential.value ? &*credential.value : nullptr, cancellation, [&](std::string_view line) {
                    return parser.Line(line);
                });
                if (parser.Cancelled())
                    return {ModelError{ModelErrorCode::Cancelled, "Model request cancelled", std::nullopt}};
                if (parser.Invalid())
                    return {ModelError{ModelErrorCode::Protocol, "Invalid model stream", std::nullopt}};
                if (!outcome.Succeeded())
                    return outcome;
                return parser.Finish();
            }

        private:
            std::optional<std::string> ResolveCredential() const {
                if (m_protocol == Protocol::Ollama)
                    return std::nullopt;
                if (m_config.credentialReference.empty() || !m_resolver)
                    return std::string{};
                auto credential = m_resolver(m_config.credentialReference);
                return credential.value_or(std::string{});
            }

            Protocol m_protocol;
            ModelProviderConfig m_config;
            ModelCredentialResolver m_resolver;
            // The adapter owns call-ID uniqueness across concurrent synchronous Stream calls; hosts join calls before destruction.
            std::atomic<std::uint64_t> m_nextInvocation{1};
        };
    }  // namespace

    /** @copydoc CreateOllamaModelProvider */
    std::unique_ptr<IModelProvider> CreateOllamaModelProvider(ModelProviderConfig config) {
        return std::make_unique<ReferenceProvider>(Protocol::Ollama, std::move(config), ModelCredentialResolver{});
    }

    /** @copydoc CreateOpenAIModelProvider */
    std::unique_ptr<IModelProvider> CreateOpenAIModelProvider(ModelProviderConfig config, ModelCredentialResolver resolver) {
        return std::make_unique<ReferenceProvider>(Protocol::OpenAI, std::move(config), std::move(resolver));
    }
}  // namespace Horo::Agent
