#include "Horo/Release/UpdateDiscoveryErrors.h"
#include "Horo/Release/UpdateTransferErrors.h"
#include "HoroEditor/app/ConfiguredEditorUpdateManifestSource.h"

#include <catch2/catch_test_macros.hpp>
#include <utility>

using namespace Horo;
using namespace Horo::Editor;

namespace {
    class RecordingHttpClient final : public IEditorUpdateManifestHttpClient {
    public:
        std::string requestedUrl;
        unsigned requests{};
        std::string body{"signed-document"};

        Result<std::string> Get(const std::string &url, const EditorUpdateManifestHttpPolicy &, CancellationToken) override {
            ++requests;
            requestedUrl = url;
            return Result<std::string>::Success(body);
        }
    };

    [[nodiscard]] EditorUpdateManifestEndpoint StableEndpoint() {
        return {{EditorUpdateChannelKind::Stable, {}}, "stable", "https://updates.example.test/stable/manifest.json"};
    }
}  // namespace

TEST_CASE("Installed editor source binds each selected channel to its host endpoint", "[editor][update]") {
    RecordingHttpClient http;
    ConfiguredEditorUpdateManifestSource source{{StableEndpoint(),
                                                 {{EditorUpdateChannelKind::Enterprise, "corp"},
                                                  "enterprise",
                                                  "https://mirror.example.test/manifest.json"}},
                                                {},
                                                http};
    auto stable = source.Fetch({}, {});
    REQUIRE(stable.HasValue());
    CHECK(stable.Value().expectedChannel == "stable");
    CHECK(stable.Value().signedDocument == "signed-document");
    CHECK(http.requestedUrl == "https://updates.example.test/stable/manifest.json");

    auto enterprise = source.Fetch({EditorUpdateChannelKind::Enterprise, "corp"}, {});
    REQUIRE(enterprise.HasValue());
    CHECK(enterprise.Value().expectedChannel == "enterprise");
    CHECK(http.requestedUrl == "https://mirror.example.test/manifest.json");
    CHECK(http.requests == 2U);
}

TEST_CASE("Installed editor source rejects absent, duplicate and unsafe endpoint policy before transport", "[editor][update]") {
    RecordingHttpClient http;
    ConfiguredEditorUpdateManifestSource missing{{StableEndpoint()}, {}, http};
    auto absent = missing.Fetch({EditorUpdateChannelKind::Preview, {}}, {});
    REQUIRE(absent.HasError());
    CHECK(absent.ErrorValue().code.Value() == Release::UpdateDiscoveryErrors::InvalidPolicy.code.Value());

    ConfiguredEditorUpdateManifestSource duplicate{{StableEndpoint(), StableEndpoint()}, {}, http};
    auto repeated = duplicate.Fetch({}, {});
    REQUIRE(repeated.HasError());
    CHECK(repeated.ErrorValue().code.Value() == Release::UpdateDiscoveryErrors::InvalidPolicy.code.Value());

    const EditorUpdateManifestEndpoint preview{{EditorUpdateChannelKind::Preview, {}},
                                               "preview",
                                               "https://updates.example.test/preview/manifest.json"};
    ConfiguredEditorUpdateManifestSource duplicateOther{{StableEndpoint(), preview, preview}, {}, http};
    CHECK(duplicateOther.Fetch({}, {}).HasError());

    std::vector<EditorUpdateManifestEndpoint> tooMany(17U, StableEndpoint());
    ConfiguredEditorUpdateManifestSource oversizedPolicy{std::move(tooMany), {}, http};
    CHECK(oversizedPolicy.Fetch({}, {}).HasError());

    auto endpoint = StableEndpoint();
    endpoint.url = "https://user@updates.example.test/manifest.json";
    ConfiguredEditorUpdateManifestSource unsafe{{endpoint}, {}, http};
    auto rejected = unsafe.Fetch({}, {});
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == Release::UpdateDiscoveryErrors::InvalidPolicy.code.Value());

    endpoint = StableEndpoint();
    endpoint.signedChannel = "nightly";
    ConfiguredEditorUpdateManifestSource mislabeled{{endpoint}, {}, http};
    CHECK(mislabeled.Fetch({}, {}).HasError());
    CHECK(http.requests == 0U);
}

TEST_CASE("Installed editor source rejects offline HTTPS mapping, oversized metadata and cancellation", "[editor][update]") {
    RecordingHttpClient http;
    ConfiguredEditorUpdateManifestSource
        offline{{{{EditorUpdateChannelKind::Offline, "media"}, "offline", "https://updates.example.test/manifest.json"}}, {}, http};
    CHECK(offline.Fetch({EditorUpdateChannelKind::Offline, "media"}, {}).HasError());

    http.body.resize(128U * 1024U + 1U, 'a');
    ConfiguredEditorUpdateManifestSource source{{StableEndpoint()}, {}, http};
    auto oversized = source.Fetch({}, {});
    REQUIRE(oversized.HasError());
    CHECK(oversized.ErrorValue().code.Value() == Release::UpdateTransferErrors::InvalidResponse.code.Value());

    CancellationSource cancellation;
    cancellation.RequestCancellation();
    auto cancelled = source.Fetch({}, cancellation.Token());
    REQUIRE(cancelled.HasError());
    CHECK(cancelled.ErrorValue().code.Value() == Release::UpdateTransferErrors::Cancelled.code.Value());
    CHECK(http.requests == 1U);
}

TEST_CASE("Installed editor HTTPS transport rejects malformed authority and invalid timeout without connecting", "[editor][update]") {
    CurlEditorUpdateManifestHttpClient http;
    const std::string url = "https://updates.example.test/manifest.json";
    auto invalidTimeout = http.Get(url, {.connectTimeoutSeconds = 0U}, {});
    REQUIRE(invalidTimeout.HasError());
    CHECK(invalidTimeout.ErrorValue().code.Value() == Release::UpdateDiscoveryErrors::InvalidPolicy.code.Value());
    auto credentials = http.Get("https://user@updates.example.test/manifest.json", {}, {});
    REQUIRE(credentials.HasError());
    CHECK(credentials.ErrorValue().code.Value() == Release::UpdateDiscoveryErrors::InvalidPolicy.code.Value());

    CancellationSource cancellation;
    cancellation.RequestCancellation();
    auto cancelled = http.Get(url, {}, cancellation.Token());
    REQUIRE(cancelled.HasError());
    CHECK(cancelled.ErrorValue().code.Value() == Release::UpdateTransferErrors::Cancelled.code.Value());
}
