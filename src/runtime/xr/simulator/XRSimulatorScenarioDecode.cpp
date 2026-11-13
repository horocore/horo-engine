#include "Horo/XR/XRSimulatorScenario.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

namespace Horo::XR {
    namespace {
        using Json = nlohmann::json;

        /** @brief Tooling-only bounded DOM ingestion. No native values or unbounded diagnostic strings are retained. */
        class BoundedSax final : public nlohmann::json_sax<Json> {
        public:
            Json root;

            bool null() override {
                return Scalar(nullptr);
            }

            bool boolean(bool value) override {
                return Scalar(value);
            }

            bool number_integer(number_integer_t value) override {
                return Scalar(value);
            }

            bool number_unsigned(number_unsigned_t value) override {
                return Scalar(value);
            }

            bool number_float(number_float_t value, const string_t &text) override {
                return std::isfinite(value) && text.size() <= XRSimulatorHardLimits::MaximumTextBytes && Scalar(value);
            }

            bool string(string_t &) override {
                return false;
            }  // Schema has no free-form string values.

            bool binary(binary_t &) override {
                return false;
            }

            bool start_object(std::size_t) override {
                return Start(true);
            }

            bool start_array(std::size_t) override {
                return Start(false);
            }

            bool end_object() override {
                return End(true);
            }

            bool end_array() override {
                return End(false);
            }

            bool key(string_t &value) override {
                if (!Work() || depth_ == 0 || value.size() > XRSimulatorHardLimits::MaximumTextBytes)
                    return false;
                auto &frame = frames_[depth_ - 1];
                if (!frame.object || frame.pending || frame.value.contains(value))
                    return false;
                frame.key = std::move(value);
                frame.pending = true;
                return true;
            }

            bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception &) override {
                return false;
            }

        private:
            struct Frame final {
                Json value;
                std::string key;
                bool object{};
                bool pending{};
            };

            std::array<Frame, XRSimulatorHardLimits::MaximumDepth> frames_;
            std::size_t depth_{};
            std::size_t work_{};
            bool rooted_{};

            bool Work() noexcept {
                return ++work_ <= XRSimulatorHardLimits::MaximumNodes;
            }

            template <class T> bool Scalar(T value) {
                return Work() && Add(Json(value));
            }

            bool Start(const bool object) {
                if (!Work() || depth_ == frames_.size())
                    return false;
                auto &frame = frames_[depth_++];
                frame.value = object ? Json::object() : Json::array();
                frame.object = object;
                frame.pending = false;
                return true;
            }

            bool End(const bool object) {
                if (depth_ == 0 || frames_[depth_ - 1].object != object || frames_[depth_ - 1].pending)
                    return false;
                auto value = std::move(frames_[--depth_].value);
                return Add(std::move(value));
            }

            bool Add(Json value) {
                if (depth_ == 0) {
                    if (rooted_)
                        return false;
                    root = std::move(value);
                    rooted_ = true;
                    return true;
                }
                auto &frame = frames_[depth_ - 1];
                if (!frame.object) {
                    frame.value.push_back(std::move(value));
                    return true;
                }
                if (!frame.pending)
                    return false;
                frame.value.emplace(std::move(frame.key), std::move(value));
                frame.pending = false;
                return true;
            }
        };

        bool Fields(const Json &value, const std::initializer_list<std::string_view> fields) {
            if (!value.is_object() || value.size() != fields.size())
                return false;
            return std::ranges::all_of(fields, [&](const auto key) {
                return value.contains(std::string(key));
            });
        }

        bool Boolean(const Json &value, bool &output) {
            if (!value.is_boolean())
                return false;
            output = value.get<bool>();
            return true;
        }

        bool Unsigned(const Json &value, std::uint64_t &output) {
            if (value.is_number_unsigned()) {
                output = value.get<std::uint64_t>();
                return true;
            }
            if (!value.is_number_integer() || value.get<std::int64_t>() < 0)
                return false;
            output = value.get<std::uint64_t>();
            return true;
        }

        template <class T> bool Enum(const Json &value, T &output, const T end) {
            std::uint64_t integer{};
            if (!Unsigned(value, integer) || integer >= static_cast<std::uint64_t>(end))
                return false;
            output = static_cast<T>(integer);
            return true;
        }

        bool Float(const Json &value, float &output) {
            if (!value.is_number())
                return false;
            output = value.get<float>();
            return std::isfinite(output);
        }

        template <std::size_t Count> bool Floats(const Json &value, std::array<float, Count> &output) {
            if (!value.is_array() || value.size() != Count)
                return false;
            for (std::size_t index = 0; index < Count; ++index)
                if (!Float(value[index], output[index]))
                    return false;
            return true;
        }

        bool Pose(const Json &value, XRSimulatorPose &pose) {
            if (!Fields(value, {"p", "q", "pv", "ov"}))
                return false;
            std::array<float, 3> position{};
            std::array<float, 4> orientation{};
            if (!Floats(value.at("p"), position) || !Floats(value.at("q"), orientation))
                return false;
            pose.position = {position[0], position[1], position[2]};
            pose.orientation = {orientation[0], orientation[1], orientation[2], orientation[3]};
            return Boolean(value.at("pv"), pose.positionValid) && Boolean(value.at("ov"), pose.orientationValid);
        }

        bool Source(const Json &value, XRSimulatorSource &source) {
            return Fields(value, {"role", "kind", "state", "pose"}) && Enum(value.at("role"), source.role, XRTrackedDeviceRole::Count) &&
                   Enum(value.at("kind"), source.kind, XRTrackedPoseKind::Count) &&
                   Enum(value.at("state"), source.state, XRTrackedDeviceState::Count) && Pose(value.at("pose"), source.pose);
        }

        bool Action(const Json &value, XRSimulatorAction &action) {
            if (!Fields(value, {"binding", "role", "kind", "active", "x", "y"}))
                return false;
            std::uint64_t binding{};
            if (!Unsigned(value.at("binding"), binding) || binding >= XRSimulatorHardLimits::MaximumActions)
                return false;
            action.bindingIndex = static_cast<std::uint32_t>(binding);
            return Enum(value.at("role"), action.role, XRTrackedDeviceRole::Count) &&
                   Enum(value.at("kind"), action.kind, XRTrackedActionKind::Count) && Boolean(value.at("active"), action.active) &&
                   Float(value.at("x"), action.x) && Float(value.at("y"), action.y);
        }

        bool Joint(const Json &value, XRSimulatorJoint &joint) {
            if (!Fields(value, {"hand", "joint", "pose", "radius"}))
                return false;
            if (!Enum(value.at("hand"), joint.hand, XRHandedness::Count) || !Enum(value.at("joint"), joint.joint, XRHandJoint::Count))
                return false;
            if (!value.at("radius").is_null()) {
                float radius{};
                if (!Float(value.at("radius"), radius))
                    return false;
                joint.radiusMeters = radius;
            }
            return Pose(value.at("pose"), joint.pose);
        }

        template <class T, std::size_t Capacity, class Function>
        bool Array(const Json &value, std::array<T, Capacity> &output, std::uint32_t &count, Function decode) {
            if (!value.is_array() || value.size() > Capacity)
                return false;
            for (std::size_t index = 0; index < value.size(); ++index)
                if (!decode(value[index], output[index]))
                    return false;
            count = static_cast<std::uint32_t>(value.size());
            return true;
        }

        bool StepHeader(const Json &value, XRSimulatorStep &step) {
            if (!Fields(value, {"event", "frame", "render", "failure", "delta", "views", "sources", "actions", "joints"}))
                return false;
            if (!value.at("event").is_null()) {
                std::uint64_t event{};
                if (!Unsigned(value.at("event"), event) || event > static_cast<std::uint64_t>(XRSessionEvent::Ready))
                    return false;
                step.event = static_cast<XRSessionEvent>(event);
            }
            return Boolean(value.at("frame"), step.executeFrame) && Boolean(value.at("render"), step.shouldRender) &&
                   Enum(value.at("failure"), step.failure, XRFakeFailure::Count) && Unsigned(value.at("delta"), step.deltaNanoseconds);
        }

        bool Step(const Json &value, XRSimulatorStep &step) {
            return StepHeader(value, step) && Array(value.at("views"), step.views, step.viewCount, Pose) &&
                   Array(value.at("sources"), step.sources, step.sourceCount, Source) &&
                   Array(value.at("actions"), step.actions, step.actionCount, Action) &&
                   Array(value.at("joints"), step.joints, step.jointCount, Joint);
        }

        bool CapturePolicy(const Json &value) {
            for (const auto key : {"purpose", "retention", "export"}) {
                std::uint64_t policy{};
                if (!Unsigned(value.at(key), policy) || policy != 0)
                    return false;
            }
            return true;
        }

        bool Capture(const Json &value, std::optional<XRSimulatorCaptureManifest> &output) {
            if (value.is_null())
                return true;
            if (!Fields(value, {"purpose", "retention", "export", "maxSteps", "maxBytes", "maxDuration", "elapsed"}))
                return false;
            if (!CapturePolicy(value))
                return false;
            XRSimulatorCaptureManifest capture;
            std::uint64_t steps{};
            if (!Unsigned(value.at("maxSteps"), steps) || steps > XRSimulatorHardLimits::MaximumSteps)
                return false;
            capture.maximumSteps = static_cast<std::uint32_t>(steps);
            if (!Unsigned(value.at("maxBytes"), capture.maximumEncodedBytes) ||
                !Unsigned(value.at("maxDuration"), capture.maximumDurationNanoseconds) ||
                !Unsigned(value.at("elapsed"), capture.elapsedNanoseconds))
                return false;
            output = capture;
            return true;
        }

        bool Fingerprint(const Json &bindings, XRSimulatorBindingFingerprint &output) {
            if (!bindings.is_array() || bindings.size() != output.size())
                return false;
            for (std::size_t index = 0; index < bindings.size(); ++index) {
                std::uint64_t byte{};
                if (!Unsigned(bindings[index], byte) || byte > 255)
                    return false;
                output[index] = static_cast<std::byte>(byte);
            }
            return true;
        }

        bool DescriptorHeader(const Json &value, XRSimulatorScenarioDescriptor &descriptor) {
            std::uint64_t dropped{};
            if (!Enum(value.at("provenance"), descriptor.provenance, XRSimulatorProvenance::Count) ||
                !Enum(value.at("initial"), descriptor.initialState, XRSimulatorInitialState::Count) ||
                !Boolean(value.at("complete"), descriptor.complete))
                return false;
            if (!Unsigned(value.at("dropped"), dropped) || dropped > std::numeric_limits<std::uint32_t>::max())
                return false;
            descriptor.droppedSteps = static_cast<std::uint32_t>(dropped);
            return Fingerprint(value.at("bindings"), descriptor.bindings) && Capture(value.at("capture"), descriptor.capture);
        }

        Result<XRSimulatorScenario> Invalid() {
            return Result<XRSimulatorScenario>::Failure(MakeError(XRSimulatorErrors::ArtifactInvalid));
        }

        Result<XRSimulatorScenario> Decode(const Json &value) {
            if (!Fields(value, {"version", "provenance", "initial", "complete", "dropped", "bindings", "capture", "steps"}))
                return Invalid();
            XRSimulatorScenarioDescriptor descriptor;
            std::uint64_t version{};
            if (!Unsigned(value.at("version"), version))
                return Invalid();
            if (version != XRSimulatorScenarioVersion)
                return Result<XRSimulatorScenario>::Failure(MakeError(XRSimulatorErrors::VersionUnsupported));
            if (!DescriptorHeader(value, descriptor))
                return Invalid();
            const auto &input = value.at("steps");
            if (!input.is_array() || input.empty() || input.size() > XRSimulatorHardLimits::MaximumSteps)
                return Invalid();
            std::vector<XRSimulatorStep> steps(input.size());
            for (std::size_t index = 0; index < input.size(); ++index)
                if (!Step(input[index], steps[index]))
                    return Invalid();
            descriptor.steps = steps;
            return XRSimulatorScenario::Create(descriptor);
        }
    }  // namespace

    /** @copydoc ParseXRSimulatorScenario */
    Result<XRSimulatorScenario> ParseXRSimulatorScenario(const std::span<const std::byte> bytes) {
        if (bytes.empty() || bytes.size() > XRSimulatorHardLimits::MaximumEncodedBytes)
            return Invalid();
        BoundedSax parser;
        if (const auto *first = reinterpret_cast<const char *>(bytes.data()); !Json::sax_parse(first, first + bytes.size(), &parser))
            return Invalid();
        return Decode(parser.root);
    }
}  // namespace Horo::XR
