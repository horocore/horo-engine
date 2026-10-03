#include "Horo/Agent/ReferenceModelProviders.h"

#include "ReferenceModelStream.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <curl/curl.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>
#include <utility>

namespace Horo::Agent {
    namespace {
        using Json = nlohmann::json;
        constexpr ModelFeatures kProtocolFeatures{static_cast<std::uint32_t>(ModelFeature::Streaming) |
                                                  static_cast<std::uint32_t>(ModelFeature::Tools) |
                                                  static_cast<std::uint32_t>(ModelFeature::Usage)};
        using Detail::kMaximumLineBytes;
        using Detail::Protocol;
        using Detail::StreamParser;
        constexpr std::size_t kMaximumDiscoveryBytes = 1U << 20U;

        /** @brief Destroy the ephemeral credential bytes after each discovery or inference dispatch. */
        struct SensitiveCredential final {
            std::optional<std::string> value;

            explicit SensitiveCredential(std::optional<std::string> secret) : value(std::move(secret)) {}

            SensitiveCredential(const SensitiveCredential &) = delete;
            SensitiveCredential &operator=(const SensitiveCredential &) = delete;
            SensitiveCredential(SensitiveCredential &&) = delete;
            SensitiveCredential &operator=(SensitiveCredential &&) = delete;

            ~SensitiveCredential() {
                if (value)
                    std::ranges::fill(*value, '\0');
            }

            [[nodiscard]] bool Invalid() const {
                return value && (value->empty() || std::ranges::any_of(*value, [](unsigned char c) {
                    return c < 0x20 || c == 0x7f;
                }));
            }
        };

        /** @brief Keep URL validation simple and reject implicit redirect or credential-bearing endpoints. */
        bool ValidEndpoint(std::string_view endpoint, Protocol protocol) {
            const bool secure = endpoint.starts_with("https://");
            const bool local = endpoint.starts_with("http://localhost:") || endpoint.starts_with("http://127.0.0.1:") ||
                               endpoint.starts_with("http://[::1]:");
            return endpoint.size() >= 9 && (secure || local) && (protocol != Protocol::Ollama || local) &&
                   endpoint.find('@') == std::string_view::npos && endpoint.find('?') == std::string_view::npos &&
                   endpoint.find('#') == std::string_view::npos;
        }

        /** @brief Map an HTTP status without retaining remote response text. */
        ModelError HttpError(long status) {
            using enum ModelErrorCode;
            ModelErrorCode code = Unavailable;
            if (status == 401 || status == 403)
                code = Authentication;
            if (status == 429)
                code = RateLimited;
            if (status == 400 || status == 422)
                code = InvalidRequest;
            std::ostringstream message;
            message << "Model provider returned HTTP " << status;
            return {code, message.str(), status};
        }

        /** @brief Own the bounded response buffer and cancellation state for one HTTP transfer. */
        struct HttpContext final {
            std::string pending;
            const std::function<bool(std::string_view)> &consume;
            std::stop_token stop;
            CURL *curl{};  // NOSONAR: libcurl declares CURL as an opaque C handle (void).
            bool cancelled{};
            bool oversized{};
            bool invalid{};
        };

        /** @brief Feed complete response lines to the parser without retaining unbounded payloads. */
        std::size_t WriteResponse(char *data,  // NOSONAR: libcurl's C callback requires a mutable char pointer.
                                  std::size_t size, std::size_t count,
                                  void *user) {  // NOSONAR: libcurl requires this exact C callback signature.
            auto &state = *static_cast<HttpContext *>(user);
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
                } catch (...) {  // NOSONAR: no user callback exception may unwind across libcurl's C frame.
                    state.invalid = true;
                }
                if (!accepted) {
                    state.oversized = newline > kMaximumLineBytes;
                    return 0;
                }
                state.pending.erase(0, newline + 1);
            }
            return bytes;
        }

        /** @brief Abort an in-flight transfer when its caller requests cancellation. */
        int ReportProgress(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {  // NOSONAR: libcurl C callback ABI.
            return static_cast<HttpContext *>(user)->stop.stop_requested() ? 1 : 0;
        }

        /** @brief Apply transport bounds and callbacks; URL and TLS policy are set before this call. */
        void ConfigureCurl(CURL *curl,  // NOSONAR: libcurl declares CURL as an opaque C handle (void).
                           HttpContext &context, const ModelProviderConfig &config, const std::string *body, curl_slist *headers) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ReportProgress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config.timeout.count()));
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, std::min(5000L, static_cast<long>(config.timeout.count())));
            if (body != nullptr) {
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->data());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body->size()));
            }
        }

        /** @brief Translate a completed transfer and its final fragment into the neutral outcome. */
        ModelOutcome FinishHttpResponse(const HttpContext &context, CURLcode result, long status) {
            using enum ModelErrorCode;
            if (context.stop.stop_requested() || context.cancelled)
                return {ModelError{Cancelled, "Model request cancelled", std::nullopt}};
            if (context.oversized)
                return {ModelError{Protocol, "Model response exceeds limit", std::nullopt}};
            if (context.invalid)
                return {ModelError{Protocol, "Invalid model response", std::nullopt}};
            if (result != CURLE_OK)
                return {ModelError{Transport, "Model HTTP transport failed", std::nullopt}};
            if (status < 200 || status >= 300)
                return {HttpError(status)};
            if (!context.pending.empty()) {
                try {
                    if (!context.consume(context.pending))
                        return {ModelError{Protocol, "Invalid final model response fragment", std::nullopt}};
                } catch (...) {  // NOSONAR: non-standard sink exceptions are a protocol failure too.
                    return {ModelError{Protocol, "Invalid final model response fragment", std::nullopt}};
                }
            }
            return {};
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

            std::string url = config.endpoint;
            if (url.ends_with('/'))
                url.pop_back();
            url += path;

            if (static const CURLcode curlInit = curl_global_init(CURL_GLOBAL_DEFAULT); curlInit != CURLE_OK)
                return {ModelError{ModelErrorCode::Transport, "Cannot initialize model HTTP runtime", std::nullopt}};
            auto *curl = curl_easy_init();
            if (curl == nullptr)
                return {ModelError{ModelErrorCode::Transport, "Cannot initialize model HTTP client", std::nullopt}};
            // URL and TLS policy are configured together before callbacks or any transfer.
            if (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) != CURLE_OK ||
                curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_3) != CURLE_OK) {
                curl_easy_cleanup(curl);
                return {ModelError{ModelErrorCode::Transport, "Cannot configure model HTTP transport", std::nullopt}};
            }
            curl_slist *headers = nullptr;
            headers = curl_slist_append(headers, "Accept: application/json");
            if (body != nullptr)
                headers = curl_slist_append(headers, "Content-Type: application/json");
            std::string authorization;
            if (bearer != nullptr) {
                authorization = "Authorization: Bearer " + *bearer;
                headers = curl_slist_append(headers, authorization.c_str());
            }

            HttpContext context{{}, consume, stop, curl};
            ConfigureCurl(curl, context, config, body, headers);
            const CURLcode result = curl_easy_perform(curl);
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            std::ranges::fill(authorization, '\0');
            return FinishHttpResponse(context, result, status);
        }

        /** @brief Serialize one assistant turn's intents without sending vendor types across the public boundary. */
        Json RequestToolCalls(const std::vector<ModelToolIntent> &intents, Protocol protocol) {
            Json calls = Json::array();
            for (const auto &intent : intents) {
                const auto arguments = Json::parse(intent.argumentsJson, nullptr, false);
                if (!arguments.is_object())
                    return nullptr;
                Json call = {{"id", intent.callId}, {"type", "function"}};
                call["function"] = {{"name", intent.name},
                                    {"arguments", protocol == Protocol::OpenAI ? Json(intent.argumentsJson) : arguments}};
                calls.push_back(std::move(call));
            }
            return calls;
        }

        /** @brief Serialize neutral conversation history into the selected provider's wire shape. */
        Json RequestMessages(const ModelRequest &request, Protocol protocol) {
            using enum ModelRole;
            Json messages = Json::array();
            for (const auto &message : request.messages) {
                std::string role = "user";
                if (message.role == System)
                    role = "system";
                if (message.role == Assistant)
                    role = "assistant";
                if (message.role == Tool)
                    role = "tool";
                Json item = {{"role", role}, {"content", message.text}};
                if (message.role == Tool && protocol == Protocol::OpenAI)
                    item["tool_call_id"] = message.toolCallId;
                if (!message.toolIntents.empty()) {
                    Json calls = RequestToolCalls(message.toolIntents, protocol);
                    if (calls.is_null())
                        return nullptr;
                    item["tool_calls"] = std::move(calls);
                }
                messages.push_back(std::move(item));
            }
            return messages;
        }

        /** @brief Serialize host-admitted JSON schemas for provider-native tool declarations. */
        Json RequestTools(const ModelRequest &request) {
            Json tools = Json::array();
            for (const auto &tool : request.tools) {
                const auto schema = Json::parse(tool.parametersJson, nullptr, false);
                if (schema.is_discarded() || !schema.is_object())
                    return nullptr;
                tools.push_back(
                    {{"type", "function"}, {"function", {{"name", tool.name}, {"description", tool.description}, {"parameters", schema}}}});
            }
            return tools;
        }

        /** @brief Build a provider-native request from neutral typed messages and tools. */
        Json RequestJson(const ModelRequest &request, Protocol protocol) {
            Json messages = RequestMessages(request, protocol);
            if (messages.is_null())
                return nullptr;
            Json payload = {{"model", request.model}, {"messages", std::move(messages)}, {"stream", true}};
            if (protocol == Protocol::OpenAI) {
                payload["max_completion_tokens"] = request.maximumOutputTokens;
                payload["stream_options"] = {{"include_usage", true}};
            } else {
                payload["options"] = {{"num_predict", request.maximumOutputTokens}};
            }
            if (!request.tools.empty()) {
                Json tools = RequestTools(request);
                if (tools.is_null())
                    return nullptr;
                payload["tools"] = std::move(tools);
            }
            return payload;
        }

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
                discovery.outcome =
                    HttpRequest(m_config, m_protocol, m_protocol == Protocol::Ollama ? "/api/tags" : "/v1/models", nullptr,
                                credential.value ? &*credential.value : nullptr, cancellation, [&body](std::string_view line) {
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
                    discovery.models.emplace_back(item[nameKey].get<std::string>(), m_config.enabledFeatures, std::nullopt);
                }
                return discovery;
            }

            /** @copydoc IModelProvider::Stream */
            ModelOutcome Stream(const ModelRequest &request, const ModelEventSink &sink, std::stop_token cancellation) override {
                if (auto admitted = AdmitModelRequest(request, m_config.enabledFeatures); !admitted.Succeeded())
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
                StreamParser parser(m_protocol, sink, cancellation, m_nextInvocation.fetch_add(1));
                const auto outcome =
                    HttpRequest(m_config, m_protocol, m_protocol == Protocol::Ollama ? "/api/chat" : "/v1/chat/completions", &body,
                                credential.value ? &*credential.value : nullptr, cancellation, [&parser](std::string_view line) {
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
