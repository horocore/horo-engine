#include "NpcDialogueHttpsProvider.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

namespace Horo::App {
    namespace {
        using namespace AI;
        using Clock = std::chrono::steady_clock;
        constexpr char ContentType[] = "Content-Type: application/vnd.horo.npc-dialogue.v1";
        constexpr char Accept[] = "Accept: application/vnd.horo.npc-dialogue.v1";
        constexpr std::string_view MediaType = "application/vnd.horo.npc-dialogue.v1";
        using Cue = Result<NpcDialogueCueId>;

        /** @brief Validates the exact endpoint with the same URL parser used by the HTTPS transport. */
        bool ValidEndpoint(const std::string &url) {
            if (!url.starts_with("https://") || url.size() > 2048 || url.find_first_of("?#") != std::string::npos ||
                url.find('\0') != std::string::npos)
                return false;
            const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed{curl_url(), &curl_url_cleanup};
            return parsed && curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), CURLU_DISALLOW_USER) == CURLUE_OK;
        }

        /** @brief Encodes only positive authored IDs; the bounded payload has no generic context fields. */
        std::string Encode(std::span<const NpcDialogueFact> facts) {
            std::string body;
            for (const auto &fact : facts) {
                body.append(std::to_string(fact.field.Value())).append(",");
                body.append(std::to_string(fact.value.Value())).append("\n");
            }
            return body;
        }

        /** @brief Accepts only a moderated cue token with a complete canonical decimal encoding. */
        Cue Decode(std::string_view body) {
            constexpr std::string_view prefix = "allow\n";
            if (!body.starts_with(prefix) || !body.ends_with('\n'))
                return Cue::Failure(MakeError(AIErrors::TaskContextInvalid));
            body.remove_prefix(prefix.size());
            if (body.size() < 2 || body.front() == '0')
                return Cue::Failure(MakeError(AIErrors::TaskContextInvalid));
            body.remove_suffix(1);
            std::uint64_t cue{};
            const auto parsed = std::from_chars(body.data(), body.data() + body.size(), cue);
            if (parsed.ec != std::errc{} || parsed.ptr != body.data() + body.size())
                return Cue::Failure(MakeError(AIErrors::TaskContextInvalid));
            return NpcDialogueCueId::Create(cue);
        }

        /** @brief Holds a reply and cancellation state for one synchronous I/O-worker transfer. */
        struct Transfer final {
            std::array<char, 32> buffer{};
            std::size_t received{};
            Clock::time_point deadline;
            CancellationToken cancellation;

            [[nodiscard]] bool Stopped() const noexcept {
                return cancellation.IsCancellationRequested() || Clock::now() >= deadline;
            }
        };

        /** @brief Aborts on excess response bytes before copying. */
        std::size_t Receive(char *bytes, std::size_t size, std::size_t count, void *user) noexcept {
            auto &transfer = *static_cast<Transfer *>(user);
            if (size == 0 || count > (transfer.buffer.size() - transfer.received) / size)
                return 0;
            const auto length = size * count;
            std::memcpy(transfer.buffer.data() + transfer.received, bytes, length);
            transfer.received += length;
            return length;
        }

        /** @brief Bridges the worker's cancellation and deadline into libcurl. */
        int Progress(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
            return static_cast<const Transfer *>(user)->Stopped() ? 1 : 0;
        }

        /** @brief Pins transfer policy to the approved HTTPS endpoint and trust store. */
        bool ConfigureEndpoint(CURL *curl, const NpcDialogueHttpsConfig &config) {
            return curl_easy_setopt(curl, CURLOPT_URL, config.endpoint.c_str()) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https") == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_PROXY, "") == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2) == CURLE_OK &&
                   (config.caBundle.empty() || curl_easy_setopt(curl, CURLOPT_CAINFO, config.caBundle.c_str()) == CURLE_OK);
        }

        /** @brief Admits only the protocol POST and its pre-encoded payload. */
        bool ConfigurePost(CURL *curl, const std::string &body, curl_slist *headers) {
            return curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str()) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size())) == CURLE_OK;
        }

        /** @brief Installs bounded response processing and cancellation. */
        bool ConfigureTransfer(CURL *curl, Transfer &transfer, long timeout) {
            return curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Receive) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer) == CURLE_OK;
        }

        /** @brief Checks status and content type before interpreting remote bytes. */
        bool ValidResponse(CURL *curl) {
            long status{};
            char *type{};
            return curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status == 200 &&
                   curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &type) == CURLE_OK && type && type == MediaType;
        }

        /** @brief Performs one post and interprets only a successful, current reply. */
        Cue ReadReply(CURL *curl, const Transfer &transfer) {
            if (transfer.Stopped() || curl_easy_perform(curl) != CURLE_OK || transfer.Stopped())
                return Cue::Failure(MakeError(AIErrors::CapabilityUnavailable));
            if (!ValidResponse(curl))
                return Cue::Failure(MakeError(AIErrors::TaskContextInvalid));
            return Decode({transfer.buffer.data(), transfer.received});
        }

        /** @brief Owns curl resources through the complete exchange. */
        Cue Exchange(const NpcDialogueHttpsConfig &config, Transfer &transfer, const std::string &body, long timeout) {
            const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers{curl_slist_append(nullptr, ContentType),
                                                                                    &curl_slist_free_all};
            const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{curl_easy_init(), &curl_easy_cleanup};
            if (!headers || !curl || !curl_slist_append(headers.get(), Accept) || !ConfigureEndpoint(curl.get(), config) ||
                !ConfigurePost(curl.get(), body, headers.get()) || !ConfigureTransfer(curl.get(), transfer, timeout))
                return Cue::Failure(MakeError(AIErrors::CapabilityUnavailable));
            return ReadReply(curl.get(), transfer);
        }

        /** @brief Performs only the admitted transport and keeps server text out of diagnostics. */
        class Provider final : public INpcDialogueProvider {
        public:
            explicit Provider(NpcDialogueHttpsConfig config) : config_(std::move(config)) {}

            Result<NpcDialogueCueId> RequestCue(std::span<const NpcDialogueFact> facts, Clock::time_point deadline,
                                               const CancellationToken &cancellation) const override {
                const auto now = Clock::now();
                if (facts.size() > NpcDialogueTurn::MaximumFacts || now >= deadline || cancellation.IsCancellationRequested())
                    return Cue::Failure(MakeError(AIErrors::TaskContextInvalid));
                const auto body = Encode(facts);
                Transfer transfer{.deadline = deadline, .cancellation = cancellation};
                const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
                const auto timeout = std::clamp<decltype(milliseconds)>(milliseconds, 1, (std::numeric_limits<long>::max)());
                return Exchange(config_, transfer, body, static_cast<long>(timeout));
            }

        private:
            NpcDialogueHttpsConfig config_;
        };
    }  // namespace

    /** @copydoc MakeNpcDialogueHttpsProvider */
    Result<AI::NpcDialogueProviderBinding> MakeNpcDialogueHttpsProvider(NpcDialogueHttpsConfig config) {
        using Binding = Result<AI::NpcDialogueProviderBinding>;
        static const bool CurlReady = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
        if (!CurlReady || !config.route.provider.IsValid() || !config.route.destination.IsValid() ||
            config.route.version != AI::NpcDialogueWireVersion ||
            config.generation == 0 || !ValidEndpoint(config.endpoint) || config.caBundle.find('\0') != std::string::npos)
            return Binding::Failure(MakeError(AI::AIErrors::TaskContextInvalid));
        try {
            const auto route = config.route;
            const auto generation = config.generation;
            auto provider = std::make_shared<Provider>(std::move(config));
            return Binding::Success({route, generation, provider, provider});
        } catch (const std::bad_alloc &) {
            return Binding::Failure(MakeError(AI::AIErrors::TaskCapacityExceeded));
        }
    }
}  // namespace Horo::App
