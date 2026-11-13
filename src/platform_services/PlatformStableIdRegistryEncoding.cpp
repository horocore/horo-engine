#include "Horo/PlatformServices/PlatformStableIdRegistry.h"

#include <nlohmann/json.hpp>

namespace Horo::PlatformServices {
    /** @copydoc PlatformStableIdRegistry::CanonicalDocument */
    Result<std::string> PlatformStableIdRegistry::CanonicalDocument() const {
        if (projectId_.empty())
            return Result<std::string>::Failure(MakeError(StableIdErrors::InvalidLedger));
        using Json = nlohmann::ordered_json;
        auto entries = Json::array();
        for (const auto &entry : entries_)
            entries.push_back({{"kind", static_cast<std::uint8_t>(entry.kind)},
                               {"canonicalKey", entry.canonicalKey},
                               {"id", FormatPlatformServiceStableId(entry.storedId)},
                               {"aliases", entry.aliases},
                               {"state", entry.state == PlatformStableIdState::Active ? "active" : "tombstoned"},
                               {"removalProvenance", entry.removalProvenance}});
        auto encoded = Json{{"schemaVersion", PlatformStableIdRegistrySchemaVersion},
                            {"projectId", projectId_},
                            {"algorithm", "horo.platform-services.stable-id.v1"},
                            {"entries", std::move(entries)}}
                           .dump(2) +
                       "\n";
        if (encoded.size() > PlatformStableIdRegistryMaximumDocumentBytes)
            return Result<std::string>::Failure(MakeError(StableIdErrors::InvalidLedger));
        return Result<std::string>::Success(std::move(encoded));
    }
}  // namespace Horo::PlatformServices
