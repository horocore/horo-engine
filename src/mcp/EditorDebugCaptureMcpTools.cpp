#include "EditorDebugCaptureMcpTools.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Mcp/McpAuthorization.h"
#include "Horo/Mcp/McpErrors.h"
#include "Horo/Mcp/McpSession.h"

#include <algorithm>
#include <charconv>
#include <limits>
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
        enum class CaptureKind {
            Telemetry,
            Image
        };
        constexpr std::size_t ImageChunkBytes = 2048;

        struct CaptureOwner {
            std::uint64_t id{};
            Mcp::McpSessionHandle session;
            std::weak_ptr<const Mcp::McpAuthority> principal;
            EditorViewportImageRegion region;

            bool Matches(std::uint64_t requested, const Mcp::McpRequestContext &context) const {
                return id == requested && session == context.session && principal.lock() == context.authority;
            }
        };

        auto RetainedAuthority(const Mcp::McpRequestContext &context) {
            return [principal = std::weak_ptr<const Mcp::McpAuthority>{context.authority}] {
                const auto active = principal.lock();
                return active && !active->IsStopRequested();
            };
        }

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

        Json ImageInteger(std::size_t minimum, std::size_t maximum) {
            return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
        }

        Json ImageRegionProperties() {
            const auto coordinate = ImageInteger(0, std::numeric_limits<std::uint32_t>::max());
            const auto dimension = ImageInteger(1, EditorViewportImageRegion::MaximumDimension);
            return {{"x", coordinate}, {"y", coordinate}, {"width", dimension}, {"height", dimension}};
        }

        constexpr auto MaximumImageBytes =
            EditorViewportImageRegion::MaximumDimension * EditorViewportImageRegion::MaximumDimension * EditorViewportImageRegion::Channels;

        Json ImageArtifactSchema() {
            auto properties = ImageRegionProperties();
            properties.update({{"captureId", CaptureIdSchema()},
                               {"phase", {{"type", "string"}, {"enum", {"pending", "ready", "cancelled", "resized", "failed", "expired"}}}},
                               {"offset", ImageInteger(0, MaximumImageBytes - 1)},
                               {"nextOffset", ImageInteger(1, MaximumImageBytes - 1)},
                               {"totalBytes", ImageInteger(EditorViewportImageRegion::Channels, MaximumImageBytes)},
                               {"encoding", {{"type", "string"}, {"enum", {"rgba8-hex"}}}},
                               {"data", {{"type", "string"}, {"maxLength", ImageChunkBytes * 2}}}});
            return ObjectSchema(std::move(properties), {"captureId", "phase"});
        }

        std::string_view ImagePhaseName(EditorViewportImagePhase phase) {
            switch (phase) {
                case EditorViewportImagePhase::Pending:
                    return "pending";
                case EditorViewportImagePhase::Ready:
                    return "ready";
                case EditorViewportImagePhase::Cancelled:
                    return "cancelled";
                case EditorViewportImagePhase::Resized:
                    return "resized";
                case EditorViewportImagePhase::Failed:
                    return "failed";
                case EditorViewportImagePhase::Expired:
                    return "expired";
            }
            return {};
        }

        std::string EncodeImageChunk(std::span<const std::byte> pixels) {
            constexpr std::string_view digits = "0123456789abcdef";
            std::string hex(pixels.size() * 2, '0');
            std::size_t index = 0;
            for (const auto pixel : pixels) {
                const auto value = std::to_integer<unsigned>(pixel);
                hex[index++] = digits[value >> 4];
                hex[index++] = digits[value & 0x0f];
            }
            return hex;
        }

        Output ReadImage(const EditorDebugCaptureEndpoint &endpoint, const CaptureOwner &owner, std::size_t offset,
                         const Mcp::McpRequestContext &context) {
            const auto image = endpoint.readImage(owner.id);
            if (!image)
                return Output::Failure(MakeError(Mcp::McpErrors::OperationUnavailable));
            Json result{{"captureId", std::to_string(owner.id)}, {"phase", ImagePhaseName(image->phase)}};
            if (image->phase != EditorViewportImagePhase::Ready)
                return Output::Success(std::move(result));
            const auto &artifact = image->artifact;
            if (!artifact || artifact->region != owner.region || artifact->rgba.size() != artifact->region.Bytes() ||
                offset >= artifact->rgba.size())
                return Output::Failure(MakeError(Mcp::McpErrors::OperationUnavailable));
            if (!context.BindResultLease({image->publication, image->expiresAt}))
                return Output::Failure(MakeError(Mcp::McpErrors::OperationUnavailable));

            const auto bytes = artifact->rgba.size();
            const auto length = std::min(ImageChunkBytes, bytes - offset);
            const auto &region = artifact->region;
            result.update({{"x", region.x},
                           {"y", region.y},
                           {"width", region.width},
                           {"height", region.height},
                           {"offset", offset},
                           {"totalBytes", bytes},
                           {"encoding", "rgba8-hex"},
                           {"data", EncodeImageChunk(std::span{artifact->rgba}.subspan(offset, length))}});
            if (offset + length < bytes)
                result["nextOffset"] = offset + length;
            return Output::Success(std::move(result));
        }

        class CaptureTool final : public Mcp::IMcpToolAdapter {
        public:
            CaptureTool(Action action, CaptureKind kind, std::weak_ptr<const EditorDebugCaptureEndpoint> endpoint,
                        std::shared_ptr<CaptureOwner> owner)
                : action_(action), kind_(kind), endpoint_(std::move(endpoint)), owner_(std::move(owner)) {}

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
                return AccessCapture(*endpoint, arguments, context);
            }

        private:
            Output AccessCapture(const EditorDebugCaptureEndpoint &endpoint, const Json &arguments, const Mcp::McpRequestContext &context) {
                const auto id = ReadCaptureId(arguments);
                if (!id || !owner_->Matches(*id, context))
                    return Output::Failure(MakeError(Mcp::McpErrors::OperationUnavailable));
                if (action_ == Action::Cancel)
                    return Output::Success({{"cancelled", kind_ == CaptureKind::Image ? endpoint.cancelImage(*id) : endpoint.cancel(*id)}});
                if (kind_ == CaptureKind::Image)
                    return ReadImage(endpoint, *owner_, arguments.value("offset", std::size_t{0}), context);
                const auto artifact = endpoint.read(*id);
                if (!artifact)
                    return Output::Failure(MakeError(Mcp::McpErrors::OperationUnavailable));
                return Output::Success(WriteArtifact(*artifact));
            }

            Output Start(const EditorDebugCaptureEndpoint &endpoint, const Json &arguments, const Mcp::McpRequestContext &context) {
                EditorViewportImageRegion region{};
                const auto result = [&] {
                    if (kind_ == CaptureKind::Telemetry)
                        return endpoint.start({arguments.at("target").get<std::uint64_t>(), arguments.at("samples").get<std::uint32_t>(),
                                               arguments.at("intervalMs").get<std::uint32_t>()},
                                              RetainedAuthority(context));
                    region = {arguments.at("x").get<std::uint32_t>(), arguments.at("y").get<std::uint32_t>(),
                              arguments.at("width").get<std::uint32_t>(), arguments.at("height").get<std::uint32_t>()};
                    if (!region.Fits(std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max()))
                        return Result<std::uint64_t>::Failure(MakeError(Mcp::McpErrors::OperationUnavailable));
                    return endpoint.startImage(region, RetainedAuthority(context));
                }();
                if (result.HasError())
                    return Output::Failure(result.ErrorValue());
                *owner_ = {result.Value(), context.session, context.authority, region};
                return Output::Success({{"captureId", std::to_string(result.Value())}});
            }

            Action action_;
            CaptureKind kind_;
            std::weak_ptr<const EditorDebugCaptureEndpoint> endpoint_;
            std::shared_ptr<CaptureOwner> owner_;
        };

        Mcp::McpToolRegistration Register(Action action, std::string id, std::string description, Json input, Json output,
                                          const std::shared_ptr<const EditorDebugCaptureEndpoint> &endpoint,
                                          const std::shared_ptr<CaptureOwner> &owner, CaptureKind kind = CaptureKind::Telemetry) {
            return {.descriptor =
                        {.id = {std::move(id)},
                         .description = std::move(description),
                         .inputSchema = std::move(input),
                         .outputSchema = std::move(output),
                         .effect = action == Action::Read ? Mcp::McpToolEffect::Query : Mcp::McpToolEffect::PresentationSideEffect,
                         .requiredCapabilities = {std::string{EditorDebugCaptureGrant}},
                         .bounds = {.maximumInputBytes = 512, .maximumResultBytes = 8192, .maximumDepth = 6, .maximumNodes = 256}},
                    .adapter = std::make_shared<CaptureTool>(action, kind, endpoint, owner),
                    .owner = Mcp::McpOwnerContext::Editor};
        }

        bool CanBindTelemetry(const EditorDebugCaptureEndpoint &endpoint) {
            return endpoint.owner == std::this_thread::get_id() && !endpoint.project.empty() && endpoint.project.size() <= 256 &&
                   IsValidUtf8ScalarSequence(endpoint.project) && endpoint.authorizationRevision && endpoint.start && endpoint.read &&
                   endpoint.cancel;
        }
    }  // namespace

    /** @copydoc MakeEditorDebugCaptureMcpTools */
    Result<std::vector<Mcp::McpToolRegistration>> MakeEditorDebugCaptureMcpTools(std::weak_ptr<const EditorDebugCaptureEndpoint> endpoint) {
        using Entries = Result<std::vector<Mcp::McpToolRegistration>>;
        const auto bound = endpoint.lock();
        if (!bound || !CanBindTelemetry(*bound))
            return Entries::Failure(MakeError(Mcp::McpErrors::ConfigurationInvalid));
        const auto owner = std::make_shared<CaptureOwner>();
        const auto id = ObjectSchema({{"captureId", CaptureIdSchema()}}, {"captureId"});
        const auto cancelled = ObjectSchema({{"cancelled", {{"type", "boolean"}}}}, {"cancelled"});
        std::vector registrations{
            Register(Action::Start, "debug.capture_telemetry", "Capture a bounded movement trace for one visible Play object.",
                     StartSchema(), id, bound, owner),
            Register(Action::Read, "debug.get_capture", "Read progress and redacted samples from an exact debug capture.", id,
                     ArtifactSchema(), bound, owner),
            Register(Action::Cancel, "debug.cancel_capture", "Cancel the exact active debug capture.", id, cancelled, bound, owner),
        };
        if (bound->startImage && bound->readImage && bound->cancelImage) {
            const auto image = std::make_shared<CaptureOwner>();
            registrations.push_back(Register(Action::Start, "viewport.capture_image", "Capture a bounded crop of the authored viewport.",
                                             ObjectSchema(ImageRegionProperties(), {"x", "y", "width", "height"}), id, bound, image,
                                             CaptureKind::Image));
            registrations.push_back(
                Register(Action::Read, "debug.get_image", "Read status or up to 2048 RGBA8 bytes at a byte offset.",
                         ObjectSchema({{"captureId", CaptureIdSchema()}, {"offset", ImageInteger(0, MaximumImageBytes - 1)}},
                                      {"captureId"}),
                         ImageArtifactSchema(), bound, image, CaptureKind::Image));
            registrations.push_back(Register(Action::Cancel, "debug.cancel_image", "Cancel the exact active viewport image capture.", id,
                                             cancelled, bound, image, CaptureKind::Image));
        }
        return Entries::Success(std::move(registrations));
    }
}  // namespace Horo::Editor
