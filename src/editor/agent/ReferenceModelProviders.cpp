#include "Horo/Agent/ReferenceModelProviders.h"

#include "Horo/Security/CredentialStore.h"
#include "ReferenceModelStream.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <curl/curl.h>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <span>
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

        using CredentialBytesResolver = std::function<Security::SecureBytes(std::string_view)>;

        /** @brief Reject empty or control-byte-bearing credentials before constructing an HTTP header. */
        bool ValidBearer(const Security::SecureBytes &credential) {
            return !credential.Empty() && std::ranges::none_of(credential.View(), [](const std::byte byte) {
                const auto code = std::to_integer<unsigned char>(byte);
                return code < 0x20 || code == 0x7f;
            });
        }

        /** @brief Clears application-owned temporary HTTP text on every exit. */
        struct SensitiveHttpText final {
            std::string value;

            ~SensitiveHttpText() {
                Security::SecureZero(std::as_writable_bytes(std::span{value}));
            }
        };

        /** @brief Retains the libcurl list on allocation failure and releases it after the transfer handle. */
        class HttpHeaders final {
        public:
            HttpHeaders() = default;
            HttpHeaders(const HttpHeaders &) = delete;
            HttpHeaders &operator=(const HttpHeaders &) = delete;

            ~HttpHeaders() {
                curl_slist_free_all(list_);
            }

            bool Add(const char *value) {
                if (auto *next = curl_slist_append(list_, value)) {
                    list_ = next;
                    return true;
                }
                return false;
            }

            curl_slist *Get() const noexcept {
                return list_;
            }

        private:
            curl_slist *list_{};
        };

        /** @brief Supplies the origin bearer header through a temporary zeroized text copy. */
        bool AddBearer(HttpHeaders &headers, const std::span<const std::byte> bearer) {
            if (bearer.empty())
                return true;
            SensitiveHttpText authorization{"Authorization: Bearer "};
            authorization.value.append(reinterpret_cast<const char *>(bearer.data()), bearer.size());
            return headers.Add(authorization.value.c_str());
        }

        /** @brief Recognizes a loopback authority with an explicit numeric port before any optional base path. */
        bool IsLoopbackEndpoint(const std::string_view endpoint) {
            constexpr std::array hosts{std::string_view{"http://localhost:"}, std::string_view{"http://127.0.0.1:"},
                                       std::string_view{"http://[::1]:"}};
            const auto host = std::ranges::find_if(hosts, [endpoint](const auto candidate) {
                return endpoint.starts_with(candidate);
            });
            if (host == hosts.end())
                return false;
            const auto authority = endpoint.substr(host->size());
            const auto port = authority.substr(0, authority.find('/'));
            unsigned number{};
            const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
            return parsed.ec == std::errc{} && parsed.ptr == port.data() + port.size() && number > 0 && number <= 65535;
        }

        /** @brief Keeps endpoint admission independent from credentials, query text and local transport policy. */
        bool ValidEndpoint(std::string_view endpoint, Protocol protocol, const bool local) {
            const bool secure = endpoint.starts_with("https://");
            return endpoint.size() >= 9 && (secure || local) && (protocol != Protocol::Ollama || local) &&
                   endpoint.find_first_of("@?#\\") == std::string_view::npos;
        }

        /** @brief Validates the endpoint and timeout before accessing credential material. */
        bool ValidHttpConfig(const ModelProviderConfig &config, Protocol protocol, const bool allowLoopback) {
            return ValidEndpoint(config.endpoint, protocol, allowLoopback && IsLoopbackEndpoint(config.endpoint)) &&
                   config.timeout.count() > 0 && config.timeout.count() <= (std::numeric_limits<long>::max)();
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

        /** @brief Keep local endpoint problems actionable without exposing URLs or remote response text. */
        ModelError TransportError(Protocol protocol, CURLcode result) {
            using enum ModelErrorCode;
            if (protocol == Protocol::Ollama && result == CURLE_COULDNT_CONNECT)
                return {Unavailable, "Local model service is unreachable. Start it or check its configured port.", std::nullopt};
            if (protocol == Protocol::Ollama && result == CURLE_OPERATION_TIMEDOUT)
                return {Unavailable, "Local model request timed out. Wait for the model to load and retry.", std::nullopt};
            return {Transport, "Model HTTP transport failed", std::nullopt};
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
                                  void *user) noexcept {  // NOSONAR: libcurl requires this exact C callback signature.
            auto &state = *static_cast<HttpContext *>(user);
            try {
                if (size != 0 && count > (std::numeric_limits<std::size_t>::max)() / size) {
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
                if (state.pending.size() > kMaximumLineBytes) {
                    state.oversized = true;
                    return 0;
                }
                return bytes;
            } catch (...) {
                // Buffer growth can fail before the guarded consumer is entered; never unwind through libcurl.
                state.invalid = true;
                return 0;
            }
        }

        /** @brief Abort an in-flight transfer when its caller requests cancellation. */
        int ReportProgress(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {  // NOSONAR: libcurl C callback ABI.
            return static_cast<HttpContext *>(user)->stop.stop_requested() ? 1 : 0;
        }

        /** @brief Apply transport bounds and callbacks; URL and TLS policy are set before this call. */
        void ConfigureCurl(CURL *curl,  // NOSONAR: libcurl declares CURL as an opaque C handle (void).
                           HttpContext &context, const ModelProviderConfig &config, const std::string *body) {
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ReportProgress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config.timeout.count()));
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (std::min)(5000L, static_cast<long>(config.timeout.count())));
            if (body != nullptr) {
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->data());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body->size()));
            }
        }

        /** @brief Translate a completed transfer and its final fragment into the neutral outcome. */
        ModelOutcome FinishHttpResponse(const HttpContext &context, Protocol protocol, CURLcode result, long status) {
            using enum ModelErrorCode;
            if (context.stop.stop_requested() || context.cancelled)
                return {ModelError{Cancelled, "Model request cancelled", std::nullopt}};
            if (context.oversized)
                return {ModelError{Protocol, "Model response exceeds limit", std::nullopt}};
            if (context.invalid)
                return {ModelError{Protocol, "Invalid model response", std::nullopt}};
            if (result != CURLE_OK)
                return {TransportError(protocol, result)};
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
                                 std::span<const std::byte> bearer, std::stop_token stop,
                                 const std::function<bool(std::string_view)> &consume) {
            const bool local = IsLoopbackEndpoint(config.endpoint);
            if (!ValidHttpConfig(config, protocol, true) || !consume) {
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
            HttpHeaders headers;
            const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle{curl_easy_init(), &curl_easy_cleanup};
            auto *curl = handle.get();
            if (curl == nullptr)
                return {ModelError{ModelErrorCode::Transport, "Cannot initialize model HTTP client", std::nullopt}};
            // URL and TLS policy are configured together before callbacks or any transfer.
            if (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) != CURLE_OK ||
                curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_3) != CURLE_OK ||
                curl_easy_setopt(curl, CURLOPT_HEADEROPT, CURLHEADER_SEPARATE) != CURLE_OK ||
                (local && curl_easy_setopt(curl, CURLOPT_PROXY, "") != CURLE_OK) || !headers.Add("Accept: application/json") ||
                (body != nullptr && !headers.Add("Content-Type: application/json")) || !AddBearer(headers, bearer) ||
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.Get()) != CURLE_OK) {
                return {ModelError{ModelErrorCode::Transport, "Cannot configure model HTTP transport", std::nullopt}};
            }

            HttpContext context{{}, consume, stop, curl};
            ConfigureCurl(curl, context, config, body);
            const CURLcode result = curl_easy_perform(curl);
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            return FinishHttpResponse(context, protocol, result, status);
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
            std::map<std::string_view, std::string_view> pendingLocalTools;
            for (const auto &message : request.messages) {
                std::string role = "user";
                if (message.role == System)
                    role = "system";
                if (message.role == Assistant)
                    role = "assistant";
                if (message.role == Tool)
                    role = "tool";
                Json item = {{"role", role}, {"content", message.text}};
                if (message.role == Tool) {
                    if (protocol == Protocol::OpenAI) {
                        item["tool_call_id"] = message.toolCallId;
                    } else {
                        const auto tool = pendingLocalTools.find(message.toolCallId);
                        if (tool == pendingLocalTools.end())
                            return nullptr;
                        item["tool_name"] = tool->second;
                        pendingLocalTools.erase(tool);
                    }
                } else if (!pendingLocalTools.empty()) {
                    return nullptr;
                }
                if (!message.toolIntents.empty()) {
                    Json calls = RequestToolCalls(message.toolIntents, protocol);
                    if (calls.is_null())
                        return nullptr;
                    item["tool_calls"] = std::move(calls);
                    if (protocol == Protocol::Ollama)
                        for (const auto &intent : message.toolIntents)
                            if (!pendingLocalTools.try_emplace(intent.callId, intent.name).second)
                                return nullptr;
                }
                messages.push_back(std::move(item));
            }
            return pendingLocalTools.empty() ? std::move(messages) : Json(nullptr);
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
            ReferenceProvider(Protocol protocol, ModelProviderConfig config, CredentialBytesResolver resolver, bool allowLoopback = true)
                : m_protocol(protocol), m_config(std::move(config)), m_resolver(std::move(resolver)), m_allowLoopback(allowLoopback) {
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
                if (cancellation.stop_requested()) {
                    discovery.outcome = {ModelError{ModelErrorCode::Cancelled, "Model request cancelled", std::nullopt}};
                    return discovery;
                }
                if (!ValidHttpConfig(m_config, m_protocol, m_allowLoopback)) {
                    discovery.outcome = {
                        ModelError{ModelErrorCode::InvalidRequest, "Invalid model provider endpoint or timeout", std::nullopt}};
                    return discovery;
                }
                std::string body;
                const auto credential = ResolveCredential();
                if (m_protocol == Protocol::OpenAI && !ValidBearer(credential)) {
                    discovery.outcome = {ModelError{ModelErrorCode::CredentialUnavailable, "Model credential unavailable", std::nullopt}};
                    return discovery;
                }
                discovery.outcome = HttpRequest(m_config, m_protocol, m_protocol == Protocol::Ollama ? "/api/tags" : "/v1/models", nullptr,
                                                credential.View(), cancellation, [&body](std::string_view line) {
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
                if (cancellation.stop_requested())
                    return {ModelError{ModelErrorCode::Cancelled, "Model request cancelled", std::nullopt}};
                if (!ValidHttpConfig(m_config, m_protocol, m_allowLoopback))
                    return {ModelError{ModelErrorCode::InvalidRequest, "Invalid model provider endpoint or timeout", std::nullopt}};
                const Json payload = RequestJson(request, m_protocol);
                if (payload.is_null())
                    return {ModelError{ModelErrorCode::InvalidRequest, "Invalid tool schema or conversation history", std::nullopt}};
                const auto credential = ResolveCredential();
                if (m_protocol == Protocol::OpenAI && !ValidBearer(credential))
                    return {ModelError{ModelErrorCode::CredentialUnavailable, "Model credential unavailable", std::nullopt}};
                const std::string body = payload.dump();
                StreamParser parser(m_protocol, sink, cancellation, m_nextInvocation.fetch_add(1));
                const auto outcome =
                    HttpRequest(m_config, m_protocol, m_protocol == Protocol::Ollama ? "/api/chat" : "/v1/chat/completions", &body,
                                credential.View(), cancellation, [&parser](std::string_view line) {
                    return parser.Line(line);
                });
                if (parser.Cancelled())
                    return {ModelError{ModelErrorCode::Cancelled, "Model request cancelled", std::nullopt}};
                if (parser.Invalid())
                    return {ModelError{ModelErrorCode::Protocol, "Invalid model stream", std::nullopt}};
                if (m_protocol == Protocol::Ollama && outcome.error && outcome.error->httpStatus == 404)
                    return {ModelError{ModelErrorCode::InvalidRequest, "Configured local model was not found", 404}};
                if (!outcome.Succeeded())
                    return outcome;
                return parser.Finish();
            }

        private:
            /** @brief Treat a failed cloud lookup as unavailable without carrying provider diagnostics. */
            Security::SecureBytes ResolveCredential() const {
                if (m_protocol == Protocol::Ollama || m_config.credentialReference.empty() || !m_resolver)
                    return {};
                try {
                    return m_resolver(m_config.credentialReference);
                } catch (...) {  // Credential backends can throw diagnostics containing sensitive material.
                    return {};
                }
            }

            Protocol m_protocol;
            ModelProviderConfig m_config;
            CredentialBytesResolver m_resolver;
            bool m_allowLoopback;
            // The adapter owns call-ID uniqueness across concurrent synchronous Stream calls; hosts join calls before destruction.
            std::atomic<std::uint64_t> m_nextInvocation{1};
        };
    }  // namespace

    /** @copydoc CreateOllamaModelProvider */
    std::unique_ptr<IModelProvider> CreateOllamaModelProvider(ModelProviderConfig config) {
        return std::make_unique<ReferenceProvider>(Protocol::Ollama, std::move(config), CredentialBytesResolver{});
    }

    /** @copydoc ConfirmOllamaModelTools */
    ModelOutcome ConfirmOllamaModelTools(const ModelProviderConfig &config, std::string_view model, std::stop_token cancellation) {
        using enum ModelErrorCode;
        std::string request;
        try {
            if (!model.empty() && model.size() <= 256)
                request = Json{{"model", model}}.dump();
        } catch (const Json::exception &) {
            return {ModelError{InvalidRequest, "Invalid installed model identifier", std::nullopt}};
        }
        if (request.empty())
            return {ModelError{InvalidRequest, "Invalid installed model identifier", std::nullopt}};

        std::string body;
        auto outcome = HttpRequest(config, Protocol::Ollama, "/api/show", &request, {}, cancellation, [&body](std::string_view line) {
            if (line.size() > kMaximumDiscoveryBytes - body.size())
                return false;
            body.append(line);
            return true;
        });
        if (!outcome.Succeeded()) {
            if (outcome.error->httpStatus == 404)
                return {ModelError{InvalidRequest, "Configured local model was not found", 404}};
            return outcome;
        }
        const auto response = Json::parse(body, nullptr, false);
        const auto features = response.find("capabilities");
        if (features != response.end() && features->is_array() && std::ranges::find(*features, "tools") != features->end())
            return {};
        return {ModelError{UnsupportedCapability, "The installed model does not advertise tools", std::nullopt}};
    }

    /** @copydoc CreateOpenAIModelProvider */
    std::unique_ptr<IModelProvider> CreateOpenAIModelProvider(ModelProviderConfig config, ModelCredentialResolver resolver) {
        auto compatible = [resolver = std::move(resolver)](std::string_view reference) -> Security::SecureBytes {
            struct LegacyText {
                std::optional<std::string> value;
                ~LegacyText() {
                    if (value)
                        Security::SecureZero(std::as_writable_bytes(std::span{*value}));
                }
            };
            LegacyText credential{resolver ? resolver(reference) : std::nullopt};
            return credential.value ? Security::SecureBytes{std::as_bytes(std::span{*credential.value})} : Security::SecureBytes{};
        };
        return std::make_unique<ReferenceProvider>(Protocol::OpenAI, std::move(config), std::move(compatible));
    }

    /** @copydoc CreateOpenAIModelProviderWithCredentials */
    std::unique_ptr<IModelProvider> CreateOpenAIModelProviderWithCredentials(ModelProviderConfig config,
                                                                             std::shared_ptr<Security::CredentialProvider> credentials) {
        auto resolve = [credentials = std::move(credentials)](std::string_view text) -> Security::SecureBytes {
            const auto reference = CredentialReference::Parse(text);
            if (!credentials || reference.HasError())
                return {};
            const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            auto result = credentials->Resolve(reference.Value(), now > 0 ? static_cast<std::uint64_t>(now) : 0);
            return result.HasValue() ? std::move(result).Value() : Security::SecureBytes{};
        };
        return std::make_unique<ReferenceProvider>(Protocol::OpenAI, std::move(config), std::move(resolve), false);
    }
}  // namespace Horo::Agent
