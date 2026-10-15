#include "SceneSourceInternal.h"

#include <array>
#include <limits>

namespace Horo::SceneSource::Detail {
    namespace {
        /** @brief Reject missing, wrongly typed or zero stable identities before scalar reads. */
        template <std::size_t Size> bool StableFields(const Json &value, const std::array<const char *, Size> &fields) {
            if (!value.is_object())
                return false;
            for (const char *field : fields)
                if (!value.contains(field) || !value[field].is_number_unsigned() || !value[field].get<std::uint64_t>())
                    return false;
            return true;
        }
    }  // namespace

    /** @copydoc SceneGeneratedOwnershipJson */
    WireJson SceneGeneratedOwnershipJson(const SceneGeneratedOwnership &v) {
        const auto &s = v.source;
        return {{"source",
                 {{"asset", s.asset.ToString()},
                  {"identity", s.identity},
                  {"revision", s.revision},
                  {"execution", s.execution},
                  {"node", s.node},
                  {"pin", s.pin},
                  {"sample", s.sample},
                  {"ordinal", s.ordinal}}},
                {"owner", v.owner},
                {"scope", v.scope},
                {"lineage", v.lineage},
                {"set", v.set},
                {"output", v.output},
                {"revision", v.revision},
                {"ownershipGeneration", v.ownershipGeneration},
                {"content", FormatSha256(v.content)}};
    }

    /** @copydoc ParseSceneGeneratedOwnership */
    Result<std::optional<SceneGeneratedOwnership>> ParseSceneGeneratedOwnership(const Json &object) {
        using Return = Result<std::optional<SceneGeneratedOwnership>>;
        if (!object.contains("generated"))
            return Return::Success(std::nullopt);
        const auto invalid = [] {
            return Return::Failure(PersistenceError(SceneInvalid, "Scene generated ownership is malformed."));
        };
        const auto &v = object["generated"];
        constexpr std::array ownerFields{"owner", "scope", "lineage", "set", "output", "revision", "ownershipGeneration"};
        if (!StableFields(v, ownerFields) || !v.contains("source") || !v.contains("content") || !v["content"].is_string())
            return invalid();
        const auto &s = v["source"];
        constexpr std::array sourceFields{"identity", "revision", "execution", "node", "pin", "sample"};
        if (!StableFields(s, sourceFields) || !s.contains("asset") || !s["asset"].is_string() || !s.contains("ordinal") ||
            !s["ordinal"].is_number_unsigned() || s["ordinal"].get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
            return invalid();
        const auto asset = Assets::AssetId::Parse(s["asset"].get<std::string>());
        const auto content = ParseSha256(v["content"].get<std::string>());
        if (asset.HasError() || content.HasError())
            return invalid();
        SceneGeneratedOwnership result{{asset.Value(), s["identity"].get<std::uint64_t>(), s["revision"].get<std::uint64_t>(),
                                        s["execution"].get<std::uint64_t>(), s["node"].get<std::uint64_t>(), s["pin"].get<std::uint64_t>(),
                                        s["sample"].get<std::uint64_t>(), s["ordinal"].get<std::uint32_t>()},
                                       v["owner"].get<std::uint64_t>(),
                                       v["scope"].get<std::uint64_t>(),
                                       v["lineage"].get<std::uint64_t>(),
                                       v["set"].get<std::uint64_t>(),
                                       v["output"].get<std::uint64_t>(),
                                       v["revision"].get<std::uint64_t>(),
                                       v["ownershipGeneration"].get<std::uint64_t>(),
                                       content.Value()};
        if (!IsValidSceneGeneratedOwnership(result))
            return invalid();
        return Return::Success(std::move(result));
    }
}  // namespace Horo::SceneSource::Detail
