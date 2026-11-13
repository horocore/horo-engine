#include "XRSimulatorCodecInternal.h"

#include <charconv>
#include <cstring>
#include <limits>

namespace Horo::XR {
    namespace {
        class Writer final {
        public:
            std::span<std::byte> output;
            std::size_t bytes{};
            std::size_t nodes{};
            bool valid{true};

            explicit Writer(const std::span<std::byte> target = {}) noexcept : output(target) {}

            void Text(const std::string_view text) noexcept {
                if (text.size() > XRSimulatorHardLimits::MaximumEncodedBytes - bytes) {
                    valid = false;
                    return;
                }
                if (!output.empty()) {
                    if (text.size() > output.size() - bytes) {
                        valid = false;
                        return;
                    }
                    std::memcpy(output.data() + bytes, text.data(), text.size());
                }
                bytes += text.size();
            }

            void Node() noexcept {
                ++nodes;
                valid &= nodes <= XRSimulatorHardLimits::MaximumNodes;
            }

            void Key(const std::string_view key) noexcept {
                Node();
                Text("\"");
                Text(key);
                Text("\":");
            }

            void Bool(const bool value) noexcept {
                Node();
                Text(value ? "true" : "false");
            }

            void Null() noexcept {
                Node();
                Text("null");
            }

            void Number(const std::uint64_t value) noexcept {
                Node();
                std::array<char, 32> buffer{};
                const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
                valid &= result.ec == std::errc{};
                Text({buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())});
            }

            void Float(const float value) noexcept {
                Node();
                std::array<char, 64> buffer{};
                const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value == 0 ? 0.0F : value,
                                                  std::chars_format::general, std::numeric_limits<float>::max_digits10);
                valid &= result.ec == std::errc{};
                Text({buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())});
            }

            template <class T, class Function> void Array(const std::span<const T> values, Function write) noexcept {
                Node();
                Text("[");
                for (std::size_t index = 0; index < values.size(); ++index) {
                    if (index != 0)
                        Text(",");
                    write(values[index]);
                }
                Text("]");
            }

            void Pose(const XRSimulatorPose &pose) noexcept {
                Node();
                Text("{");
                Key("p");
                const std::array position{pose.position.x, pose.position.y, pose.position.z};
                Array<float>(position, [this](float value) {
                    Float(value);
                });
                Text(",");
                Key("q");
                const std::array orientation{pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
                Array<float>(orientation, [this](float value) {
                    Float(value);
                });
                Text(",");
                Key("pv");
                Bool(pose.positionValid);
                Text(",");
                Key("ov");
                Bool(pose.orientationValid);
                Text("}");
            }

            void Source(const XRSimulatorSource &source) noexcept {
                Node();
                Text("{");
                Key("role");
                Number(static_cast<std::uint64_t>(source.role));
                Text(",");
                Key("kind");
                Number(static_cast<std::uint64_t>(source.kind));
                Text(",");
                Key("state");
                Number(static_cast<std::uint64_t>(source.state));
                Text(",");
                Key("pose");
                Pose(source.pose);
                Text("}");
            }

            void Action(const XRSimulatorAction &action) noexcept {
                Node();
                Text("{");
                Key("binding");
                Number(action.bindingIndex);
                Text(",");
                Key("role");
                Number(static_cast<std::uint64_t>(action.role));
                Text(",");
                Key("kind");
                Number(static_cast<std::uint64_t>(action.kind));
                Text(",");
                Key("active");
                Bool(action.active);
                Text(",");
                Key("x");
                Float(action.x);
                Text(",");
                Key("y");
                Float(action.y);
                Text("}");
            }

            void Joint(const XRSimulatorJoint &joint) noexcept {
                Node();
                Text("{");
                Key("hand");
                Number(static_cast<std::uint64_t>(joint.hand));
                Text(",");
                Key("joint");
                Number(static_cast<std::uint64_t>(joint.joint));
                Text(",");
                Key("pose");
                Pose(joint.pose);
                Text(",");
                Key("radius");
                if (joint.radiusMeters.has_value())
                    Float(*joint.radiusMeters);
                else
                    Null();
                Text("}");
            }

            void Step(const XRSimulatorStep &step) noexcept {
                Node();
                Text("{");
                Key("event");
                if (step.event)
                    Number(static_cast<std::uint64_t>(*step.event));
                else
                    Null();
                Text(",");
                Key("frame");
                Bool(step.executeFrame);
                Text(",");
                Key("render");
                Bool(step.shouldRender);
                Text(",");
                Key("failure");
                Number(static_cast<std::uint64_t>(step.failure));
                Text(",");
                Key("delta");
                Number(step.deltaNanoseconds);
                Text(",");
                Key("views");
                Array<XRSimulatorPose>(std::span{step.views}.first(step.viewCount), [this](const auto &value) {
                    Pose(value);
                });
                Text(",");
                Key("sources");
                Array<XRSimulatorSource>(std::span{step.sources}.first(step.sourceCount), [this](const auto &value) {
                    Source(value);
                });
                Text(",");
                Key("actions");
                Array<XRSimulatorAction>(std::span{step.actions}.first(step.actionCount), [this](const auto &value) {
                    Action(value);
                });
                Text(",");
                Key("joints");
                Array<XRSimulatorJoint>(std::span{step.joints}.first(step.jointCount), [this](const auto &value) {
                    Joint(value);
                });
                Text("}");
            }

            void Capture(const std::optional<XRSimulatorCaptureManifest> &capture) noexcept {
                if (!capture) {
                    Null();
                    return;
                }
                Node();
                Text("{");
                Key("purpose");
                Number(XRSimulatorCaptureManifest::SimulatorDevelopmentPurpose);
                Text(",");
                Key("retention");
                Number(XRSimulatorCaptureManifest::MemoryUntilExplicitExport);
                Text(",");
                Key("export");
                Number(XRSimulatorCaptureManifest::SyntheticControlsOnlyExport);
                Text(",");
                Key("maxSteps");
                Number(capture->maximumSteps);
                Text(",");
                Key("maxBytes");
                Number(capture->maximumEncodedBytes);
                Text(",");
                Key("maxDuration");
                Number(capture->maximumDurationNanoseconds);
                Text(",");
                Key("elapsed");
                Number(capture->elapsedNanoseconds);
                Text("}");
            }

            void Scenario(const XRSimulatorScenario &scenario) noexcept {
                Node();
                Text("{");
                Key("version");
                Number(XRSimulatorScenarioVersion);
                Text(",");
                Key("provenance");
                Number(static_cast<std::uint64_t>(scenario.Provenance()));
                Text(",");
                Key("initial");
                Number(static_cast<std::uint64_t>(scenario.InitialState()));
                Text(",");
                Key("complete");
                Bool(scenario.Complete());
                Text(",");
                Key("dropped");
                Number(scenario.DroppedSteps());
                Text(",");
                Key("bindings");
                Array<std::byte>(scenario.BindingFingerprint(), [this](const auto value) {
                    Number(std::to_integer<std::uint8_t>(value));
                });
                Text(",");
                Key("capture");
                Capture(scenario.CaptureManifest());
                Text(",");
                Key("steps");
                Array<XRSimulatorStep>(scenario.Steps(), [this](const auto &step) {
                    Step(step);
                });
                Text("}");
            }
        };
    }  // namespace

    SimulatorInternal::EncodedMeasure SimulatorInternal::Measure(const XRSimulatorStep &step) noexcept {
        Writer writer;
        writer.Step(step);
        return {writer.bytes, writer.nodes, writer.valid};
    }

    /** @copydoc EncodeXRSimulatorScenario */
    Result<std::vector<std::byte>> EncodeXRSimulatorScenario(const XRSimulatorScenario &scenario) {
        std::vector<std::byte> output(XRSimulatorHardLimits::MaximumEncodedBytes);
        Writer writer{output};
        writer.Scenario(scenario);
        if (!writer.valid)
            return Result<std::vector<std::byte>>::Failure(MakeError(XRErrors::CapacityExceeded));
        output.resize(writer.bytes);
        return Result<std::vector<std::byte>>::Success(std::move(output));
    }
}  // namespace Horo::XR
