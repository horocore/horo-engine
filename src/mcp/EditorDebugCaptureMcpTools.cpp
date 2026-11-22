#include "EditorDebugCaptureMcpTools.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Mcp/McpAuthorization.h"
#include "Horo/Mcp/McpErrors.h"
#include "Horo/Mcp/McpSession.h"

#include <charconv>
#include <span>

namespace Horo::Editor {
    namespace {
        using Json = nlohmann::json;
        using Output = Result<Json>;
        enum class Action {
            Start,
            Read,
            Cancel
        };

        struct CaptureOwner {
            std::uint64_t id{};
            Mcp::McpSessionHandle session;
            std::weak_ptr<const Mcp::McpAuthority> principal;
        };

        Json ObjectSchema(Json properties, Json required) {
            return {{"type", "object"},
                    {"properties", std::move(properties)},
                    {"required", std::move(required)},
                    {"additionalProperties", false}};
        }

        Json CaptureIdSchema() {
            return {{"type", "string"}, {"minLength", 1}, {"maxLength", 20}};
        }

        Json StartSchema() {
            using Limits = EditorDebugCaptureRequest;
            return ObjectSchema({{"target", {{"type", "integer"}, {"minimum", 1}}},
                                 {"samples", {{"type", "integer"}, {"minimum", 1}, {"maximum", Limits::MaximumSamples}}},
                                 {"intervalMs",
                                  {{"type", "integer"}, {"minimum", Limits::MinimumIntervalMs}, {"maximum", Limits::MaximumIntervalMs}}}},
                                {"target", "samples", "intervalMs"});
        }

        Json ArtifactSchema() {
            const Json point{{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}};
            const auto sample = ObjectSchema({{"elapsedMs", {{"type", "integer"}, {"minimum", 0}}}, {"worldPosition", point}},
                                             {"elapsedMs", "worldPosition"});
            return ObjectSchema({{"captureId", CaptureIdSchema()},
                                 {"target", {{"type", "integer"}, {"minimum", 1}}},
                                 {"phase", {{"type", "string"}, {"enum", {"sampling", "ready", "cancelled", "scene_changed"}}}},
                                 {"requestedSamples",
                                  {{"type", "integer"}, {"minimum", 1}, {"maximum", EditorDebugCaptureRequest::MaximumSamples}}},
                                 {"samples",
                                  {{"type", "array"}, {"maxItems", EditorDebugCaptureRequest::MaximumSamples}, {"items", sample}}}},
                                {"captureId", "target", "phase", "requestedSamples", "samples"});
        }

        std::string_view PhaseName(const EditorDebugCapturePhase phase) {
            switch (phase) {
                case EditorDebugCapturePhase::Sampling:
                    return "sampling";
                case EditorDebugCapturePhase::Ready:
                    return "ready";
                case EditorDebugCapturePhase::Cancelled:
                    return "cancelled";
                case EditorDebugCapturePhase::SceneChanged:
                    return "scene_changed";
            }
            return {};
        }

        Json WriteArtifact(const EditorDebugCaptureArtifact &artifact) {
            Json samples = Json::array();
            for (const auto &sample : std::span{artifact.samples}.first(artifact.count))
                samples.push_back({{"elapsedMs", sample.elapsedMs}, {"worldPosition", sample.worldPosition}});
            return {{"captureId", std::to_string(artifact.id)},
                    {"target", artifact.request.target},
                    {"phase", PhaseName(artifact.phase)},
                    {"requestedSamples", artifact.request.samples},
                    {"samples", std::move(samples)}};
        }

        std::optional<std::uint64_t> ReadCaptureId(const Json &arguments) {
            const auto &text = arguments.at("captureId").get_ref<const std::string &>();
            std::uint64_t id{};
            const auto end = text.data() + text.size();
            const auto parsed = std::from_chars(text.data(), end, id);
            return parsed.ec == std::errc{} && parsed.ptr == end && id != 0 ? std::optional{id} : std::nullopt;
        }

        class CaptureTool final : public Mcp::IMcpToolAdapter {
        public:
            CaptureTool(Action action, std::weak_ptr<const EditorDebugCaptureEndpoint> endpoint, std::shared_ptr<CaptureOwner> owner)
                : action_(action), endpoint_(std::move(endpoint)), owner_(std::move(owner)) {}

            Output Invoke(const Json &arguments, const Mcp::McpRequestContext &context) override {
                const auto endpoint = endpoint_.lock();
                if (!endpoint || endpoint->owner != std::this_thread::get_id())
                    return Output::Failure(MakeError(Mcp::McpErrors::OwnerUnavailable));
                if (context.projectIdentity != endpoint->project || context.authorizationRevision != endpoint->authorizationRevision)
                    return Output::Failure(MakeError(Mcp::McpErrors::AuthorizationDenied));
                if (context.IsStopRequested())
                    return Output::Failure(MakeError(Mcp::McpErrors::RequestCancelled));
                if (action_ == Action::Start)
                    return Start(*endpoint, arguments, context);
                const auto id = ReadCaptureId(arguments);
                if (!id || owner_->id != *id || owner_->session != context.session || owner_->principal.lock() != context.authority)
                    return Output::Failure(MakeError(Mcp::McpErrors::OperationUnavailable));
                if (action_ == Action::Cancel)
                    return Output::Success({{"cancelled", endpoint->cancel(*id)}});
                const auto artifact = endpoint->read(*id);
                if (!artifact)
                    return Output::Failure(MakeError(Mcp::McpErrors::OperationUnavailable));
                return Output::Success(WriteArtifact(*artifact));
            }

        private:
            Output Start(const EditorDebugCaptureEndpoint &endpoint, const Json &arguments, const Mcp::McpRequestContext &context) {
                const auto result =
                    endpoint.start({arguments.at("target").get<std::uint64_t>(), arguments.at("samples").get<std::uint32_t>(),
                                    arguments.at("intervalMs").get<std::uint32_t>()},
                                   [principal = std::weak_ptr<const Mcp::McpAuthority>{context.authority}] {
                    const auto active = principal.lock();
                    return active && !active->IsStopRequested();
                });
                if (result.HasError())
                    return Output::Failure(result.ErrorValue());
                *owner_ = {result.Value(), context.session, context.authority};
                return Output::Success({{"captureId", std::to_string(result.Value())}});
            }

            Action action_;
            std::weak_ptr<const EditorDebugCaptureEndpoint> endpoint_;
            std::shared_ptr<CaptureOwner> owner_;
        };

        Mcp::McpToolRegistration Register(Action action, std::string id, std::string description, Json input, Json output,
                                          const std::shared_ptr<const EditorDebugCaptureEndpoint> &endpoint,
                                          const std::shared_ptr<CaptureOwner> &owner) {
            return {.descriptor =
                        {.id = {std::move(id)},
                         .description = std::move(description),
                         .inputSchema = std::move(input),
                         .outputSchema = std::move(output),
                         .effect = action == Action::Read ? Mcp::McpToolEffect::Query : Mcp::McpToolEffect::PresentationSideEffect,
                         .requiredCapabilities = {std::string{EditorDebugCaptureGrant}},
                         .bounds = {.maximumInputBytes = 512, .maximumResultBytes = 8192, .maximumDepth = 6, .maximumNodes = 256}},
                    .adapter = std::make_shared<CaptureTool>(action, endpoint, owner),
                    .owner = Mcp::McpOwnerContext::Editor};
        }
    }  // namespace

    /** @copydoc MakeEditorDebugCaptureMcpTools */
    Result<std::vector<Mcp::McpToolRegistration>> MakeEditorDebugCaptureMcpTools(std::weak_ptr<const EditorDebugCaptureEndpoint> endpoint) {
        using Entries = Result<std::vector<Mcp::McpToolRegistration>>;
        const auto bound = endpoint.lock();
        if (!bound || bound->owner != std::this_thread::get_id() || bound->project.empty() || bound->project.size() > 256 ||
            !IsValidUtf8ScalarSequence(bound->project) || !bound->authorizationRevision || !bound->start || !bound->read || !bound->cancel)
            return Entries::Failure(MakeError(Mcp::McpErrors::ConfigurationInvalid));
        const auto owner = std::make_shared<CaptureOwner>();
        const auto id = ObjectSchema({{"captureId", CaptureIdSchema()}}, {"captureId"});
        return Entries::Success({
            Register(Action::Start, "debug.capture_telemetry", "Capture a bounded movement trace for one visible Play object.",
                     StartSchema(), id, bound, owner),
            Register(Action::Read, "debug.get_capture", "Read progress and redacted samples from an exact debug capture.", id,
                     ArtifactSchema(), bound, owner),
            Register(Action::Cancel, "debug.cancel_capture", "Cancel the exact active debug capture.", id,
                     ObjectSchema({{"cancelled", {{"type", "boolean"}}}}, {"cancelled"}), bound, owner),
        });
    }
}  // namespace Horo::Editor
