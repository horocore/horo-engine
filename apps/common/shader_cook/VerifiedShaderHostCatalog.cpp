#include "VerifiedShaderHostCatalog.h"

#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>

namespace Horo::Application::Internal {
    namespace {
        using Json = nlohmann::json;

        struct InvalidCatalog final {};

        /** @brief Rejects excess nesting and duplicate keys while parsing the already authenticated bounded snapshot. */
        struct CatalogKeys final {
            std::array<std::set<std::string, std::less<>>, 4> keys;

            bool operator()(const int depth, const Json::parse_event_t event, const Json &value) {
                if (depth < 0 || static_cast<std::size_t>(depth) >= keys.size())
                    throw InvalidCatalog{};
                const auto index = static_cast<std::size_t>(depth);
                if (event == Json::parse_event_t::object_start) {
                    if (index + 1 >= keys.size())
                        throw InvalidCatalog{};
                    keys[index + 1].clear();
                }
                if (event == Json::parse_event_t::key && !keys[index].insert(value.get<std::string>()).second)
                    throw InvalidCatalog{};
                return true;
            }
        };

        /** @brief Requires the exact schema shape rather than ignoring unknown security-relevant fields. */
        bool Fields(const Json &value, const std::initializer_list<std::string_view> fields) {
            return value.is_object() && value.size() == fields.size() && std::ranges::all_of(fields, [&](const auto key) {
                return value.contains(key);
            });
        }

        /** @brief Bounds exact identity text before copying it into an immutable approval. */
        bool Text(const Json &value) {
            return value.is_string() && !value.get_ref<const std::string &>().empty() && value.get_ref<const std::string &>().size() <= 128;
        }

        /** @brief Decodes only existing tool roles; names never become paths or executable arguments. */
        std::optional<Render::ShaderCompilerTool> Role(const std::string_view name) {
            using enum Render::ShaderCompilerTool;
            if (name == "dxc")
                return Dxc;
            if (name == "spirv-val")
                return SpirvTools;
            if (name == "spirv-cross")
                return SpirvCross;
            if (name == "apple-metal")
                return AppleMetal;
            if (name == "dxil-validator")
                return DxilValidator;
            return {};
        }

        /** @brief Copies a complete closed tool table only after authentication and complete tuple validation. */
        std::vector<ShaderHostToolApproval> DecodeToolApprovals(const Json &document, const std::string_view actualHost) {
            if (!Fields(document, {"version", "host", "tools"}) || !document["version"].is_number_unsigned() ||
                document["version"].get<std::uint64_t>() != 1 || !Text(document["host"]) ||
                document["host"].get_ref<const std::string &>() != actualHost || !document["tools"].is_array() ||
                document["tools"].empty() || document["tools"].size() > 8)
                throw InvalidCatalog{};
            std::vector<ShaderHostToolApproval> tools;
            tools.reserve(document["tools"].size());
            for (const auto &tool : document["tools"]) {
                if (!Fields(tool, {"role", "release", "archive_sha256", "executable_sha256"}) || !Text(tool["role"]) ||
                    !Text(tool["release"]) || !Text(tool["archive_sha256"]) || !Text(tool["executable_sha256"]))
                    throw InvalidCatalog{};
                const auto role = Role(tool["role"].get_ref<const std::string &>());
                const auto archive = ParseSha256(tool["archive_sha256"].get_ref<const std::string &>());
                const auto executable = ParseSha256(tool["executable_sha256"].get_ref<const std::string &>());
                if (!role || archive.HasError() || executable.HasError() || archive.Value() == Sha256Digest{} ||
                    executable.Value() == Sha256Digest{} || std::ranges::any_of(tools, [&](const auto &entry) {
                    return entry.identity.tool == *role;
                }))
                    throw InvalidCatalog{};
                tools.push_back({{*role, tool["release"].get<std::string>(), archive.Value()}, executable.Value()});
            }
            std::ranges::sort(tools, {}, [](const auto &entry) {
                return entry.identity.tool;
            });
            return tools;
        }
    }  // namespace

    /** @copydoc VerifiedShaderHostCatalog::Create */
    Result<std::shared_ptr<const VerifiedShaderHostCatalog>> VerifiedShaderHostCatalog::Create(
        const std::span<const std::byte> bytes, const Security::DetachedSignatureEnvelope &envelope,
        const Security::ArtifactVerifier &verifier, const std::string_view actualHost) {
        using Creation = Result<std::shared_ptr<const VerifiedShaderHostCatalog>>;
        if (bytes.empty() || bytes.size() > 65536 || actualHost.empty() || actualHost.size() > 128)
            return Creation::Failure(MakeError(Render::ShaderCompilerPipelineErrors::ToolchainConfigurationInvalid));
        const auto authenticated = verifier.Verify(bytes, envelope);
        if (authenticated.HasError())
            return Creation::Failure(authenticated.ErrorValue());
        try {
            CatalogKeys unique;
            const auto *first = std::bit_cast<const char *>(bytes.data());
            const auto document = Json::parse(first, first + bytes.size(), std::ref(unique));
            auto tools = DecodeToolApprovals(document, actualHost);
            return Creation::Success(std::shared_ptr<const VerifiedShaderHostCatalog>{
                new VerifiedShaderHostCatalog(authenticated.Value(), std::string{actualHost}, std::move(tools))});
        } catch (const InvalidCatalog &) {
            return Creation::Failure(MakeError(Render::ShaderCompilerPipelineErrors::ToolchainConfigurationInvalid));
        } catch (const Json::exception &) {
            return Creation::Failure(MakeError(Render::ShaderCompilerPipelineErrors::ToolchainConfigurationInvalid));
        } catch (const std::bad_alloc &) {
            return Creation::Failure(MakeError(Render::ShaderCompilerPipelineErrors::AllocationFailed));
        }
    }

    /** @copydoc VerifiedShaderHostCatalog::VerifiedShaderHostCatalog */
    VerifiedShaderHostCatalog::VerifiedShaderHostCatalog(Security::VerifiedArtifactEvidence evidence, std::string host,
                                                         std::vector<ShaderHostToolApproval> tools)
        : evidence_(std::move(evidence)), host_(std::move(host)), tools_(std::move(tools)) {}

    /** @copydoc VerifiedShaderHostCatalog::Approves */
    bool VerifiedShaderHostCatalog::Approves(const std::string_view hostPlatform, const Render::ShaderCompilerToolIdentity &identity,
                                             const Sha256Digest &executableDigest) const noexcept {
        return hostPlatform == host_ && std::ranges::any_of(tools_, [&](const auto &tool) {
            return tool.identity.tool == identity.tool && tool.identity.release == identity.release &&
                   tool.identity.buildDigest == identity.buildDigest && tool.executableDigest == executableDigest;
        });
    }

    /** @copydoc VerifiedShaderHostCatalog::Tools */
    std::span<const ShaderHostToolApproval> VerifiedShaderHostCatalog::Tools() const noexcept {
        return tools_;
    }
}  // namespace Horo::Application::Internal
