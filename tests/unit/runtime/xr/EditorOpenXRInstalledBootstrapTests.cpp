#include "EditorOpenXRBootstrap.h"
#include "support/OpenXRInstalledProductTestFixture.h"

#include <array>
#include <catch2/generators/catch_generators.hpp>
#include <type_traits>

static_assert(!std::is_default_constructible_v<Horo::Editor::EditorOpenXRBootstrap::ConstructionKey>);
static_assert(!std::is_aggregate_v<Horo::Editor::EditorOpenXRBootstrap::ConstructionKey>);

namespace Horo::Editor::Tests {
    namespace {
        using Horo::Tests::OpenXRProduct::InstalledProductFixture;
        using Horo::Tests::OpenXRProduct::RequireValue;

        Result<std::unique_ptr<EditorOpenXRBootstrap>> StartInstalled(InstalledProductFixture &fixture, std::vector<std::string> &values) {
            std::vector<char *> arguments;
            for (auto &value : values)
                arguments.push_back(value.data());
            return EditorOpenXRBootstrap::Create(arguments, fixture.Root(), std::filesystem::canonical(fixture.storage.Path()) / "cache",
                                                 Runtime::SceneObjectId{1});
        }

        /** @brief Provides distinct signed movement and teleport actions with a controller aim pose. */
        nlohmann::json ReducedMotionTeleportDocument() {
            auto root = nlohmann::json::parse(InstalledProductFixture::BindingDocument());
            const auto role = static_cast<unsigned>(XR::XRTrackedDeviceRole::LeftController);
            const auto digital = static_cast<unsigned>(Input::ActionValueType::Digital);
            const auto axis = static_cast<unsigned>(Input::ActionValueType::Axis2D);
            auto move = root["actions"].front();
            move["id"] = "locomotion.move";
            move["label"] = "Move";
            move["type"] = axis;
            move["required"] = false;
            root["actions"].push_back(move);
            root["profiles"].push_back({{"id", 2}, {"path", "/interaction_profiles/oculus/touch_controller"}});
            root["controls"].push_back(
                {{"profile", 2}, {"id", 2}, {"role", role}, {"type", digital}, {"path", "/user/hand/left/input/x/click"}});
            root["controls"].push_back(
                {{"profile", 2}, {"id", 3}, {"role", role}, {"type", axis}, {"path", "/user/hand/left/input/thumbstick"}});
            root["suggestions"].push_back({{"action", "select"}, {"profile", 2}, {"control", 2}});
            root["suggestions"].push_back({{"action", "locomotion.move"}, {"profile", 2}, {"control", 3}});
            for (const auto profile : {1, 2}) {
                root["poses"].push_back({{"profile", profile},
                                         {"set", 1},
                                         {"role", role},
                                         {"kind", static_cast<unsigned>(XR::XRTrackedPoseKind::Aim)},
                                         {"path", "/user/hand/left/input/aim/pose"},
                                         {"label", "Aim"}});
            }
            root["locomotion"] = {{"actor", 17},
                                  {"moveAction", "locomotion.move"},
                                  {"speedMetersPerSecond", 2.5},
                                  {"deadZone", 0.15},
                                  {"reducedMotionTeleport", {{"action", "select"}, {"maximumDistanceMeters", 6}}}};
            return root;
        }
    }  // namespace

    TEST_CASE("Executable XR bootstrap reopens only the verified offline package under independent approved roots",
              "[xr][host][bootstrap][install]") {
        InstalledProductFixture fixture{HORO_TEST_OPENXR_LOADER};
        auto arguments = fixture.BootstrapArguments();
        arguments.emplace_back("--xr-host-world-canvas-minimum-span-pixels=48");
        fixture.RetireInstaller();
        const auto cache = std::filesystem::canonical(fixture.storage.Path()) / "cache";
        REQUIRE_FALSE(std::filesystem::exists(cache / "native"));
        REQUIRE_FALSE(std::filesystem::exists(cache / "xr-native"));
        auto bootstrap = RequireValue(StartInstalled(fixture, arguments));
        REQUIRE(bootstrap);
        CHECK(std::filesystem::is_directory(cache / "native"));
        CHECK(std::filesystem::is_directory(cache / "xr-native"));
        const auto &services = bootstrap->Services();
        REQUIRE(services.packages);
        REQUIRE(services.bindings);
        CHECK(services.bindings->schema.actions.size() == 1);
        CHECK(services.trackingRoot == Runtime::SceneObjectId{1});
        const auto &canvasSpan = services.configuration.invocation.at(SettingKey{"xr.world_canvas.minimum_span_pixels"});
        CHECK(std::get<std::int64_t>(canvasSpan.value) == 48);
        REQUIRE(services.jobs);
        REQUIRE(services.validateAdmission);
        CHECK(services.validateAdmission().HasValue());
        auto retained = services.packages;
        const auto generation = retained->Generation();
        const auto record = retained->InstalledRecord(fixture.Root(), generation);
        REQUIRE(record.HasValue());
        REQUIRE(record.Value());
        CHECK(record.Value()->Graph()->packages.size() == 1);
        bootstrap.reset();
        // A copied package capability is closed before its borrowed executable jobs/files disappear.
        CHECK(retained->InstalledRecord(fixture.Root(), generation).HasError());
    }

    TEST_CASE("Executable XR bootstrap rejects files and redirects in its owned native cache parents",
              "[xr][host][bootstrap][install][security]") {
        InstalledProductFixture fixture{HORO_TEST_OPENXR_LOADER};
        auto arguments = fixture.BootstrapArguments();
        fixture.RetireInstaller();
        const auto cache = std::filesystem::canonical(fixture.storage.Path()) / "cache";
        RequireValue(fixture.files.CreateDirectoryPath(cache, DirectoryCreationMode::Single));
        const auto child = cache / GENERATE("native", "xr-native");
        const auto redirected = std::filesystem::canonical(fixture.storage.Path()) / "redirect-target";
        SECTION("regular file") {
            std::ofstream output{child, std::ios::binary};
            output << "preserve";
            output.close();
            REQUIRE(StartInstalled(fixture, arguments).HasError());
            std::ifstream input{child, std::ios::binary};
            CHECK(std::string(std::istreambuf_iterator<char>{input}, {}) == "preserve");
        }
        SECTION("directory redirect") {
            RequireValue(fixture.files.CreateDirectoryPath(redirected, DirectoryCreationMode::Single));
            std::filesystem::create_directory_symlink(redirected, child);
            REQUIRE(StartInstalled(fixture, arguments).HasError());
            CHECK(std::filesystem::is_symlink(child));
            CHECK(std::filesystem::is_empty(redirected));
        }
    }

    TEST_CASE("Executable signed XR bootstrap rejects independent root and catalog tampering before native discovery",
              "[xr][host][bootstrap][security]") {
        InstalledProductFixture fixture{HORO_TEST_OPENXR_LOADER};
        auto arguments = fixture.BootstrapArguments();
        fixture.RetireInstaller();
        SECTION("changed catalog bytes have no verified authority") {
            std::ofstream output{arguments[4], std::ios::binary | std::ios::app};
            output << ' ';
        }
        SECTION("operator revoked the independent signer") {
            std::ifstream input{arguments[2], std::ios::binary};
            auto roots = nlohmann::json::parse(input);
            roots["publishers"][0]["revoked"] = true;
            std::ofstream output{arguments[2], std::ios::binary | std::ios::trunc};
            output << roots.dump();
        }
        SECTION("signature path points at unsigned catalog") {
            arguments[6] = arguments[4];
        }
        CHECK(StartInstalled(fixture, arguments).HasError());
    }

    TEST_CASE("Installed XR bindings preserve signed UTF8 labels and reject malformed verified-package bytes",
              "[xr][host][bootstrap][install][security]") {
        auto bytes = InstalledProductFixture::BindingDocument();
        constexpr std::string_view label = "Seçim ✓";
        const auto offset = bytes.find("Select");
        REQUIRE(offset != std::string::npos);
        bytes.replace(offset, std::string_view{"Select"}.size(), label);
        bool malformed{};
        SECTION("valid localized label") {}
        SECTION("invalid UTF8 in a verified installed binding") {
            const auto nonAscii = bytes.find("ç");
            REQUIRE(nonAscii != std::string::npos);
            bytes[nonAscii] = static_cast<char>(0xff);
            malformed = true;
        }
        SECTION("truncated verified installed JSON") {
            bytes.pop_back();
            malformed = true;
        }
        InstalledProductFixture fixture{HORO_TEST_OPENXR_LOADER, bytes};
        const auto candidate = fixture.Candidate();
        const auto parsed =
            Application::Internal::ReadInstalledOpenXRBindings(candidate.component,
                                                               RequireValue(Packages::PackagePath::Parse("runtime/xr/bindings.json")));
        if (malformed) {
            CHECK(parsed.HasError());
        } else {
            REQUIRE(parsed.HasValue());
            REQUIRE(parsed.Value()->actionLabels.size() == 1);
            CHECK(parsed.Value()->actionLabels.front().text == label);
        }
    }

    TEST_CASE("Installed XR World UI pointer requires an authored aim pose for the selected controller",
              "[xr][host][bootstrap][install][world-ui]") {
        using XR::XRTrackedDeviceRole;
        using XR::XRTrackedPoseKind;
        auto root = nlohmann::json::parse(InstalledProductFixture::BindingDocument());
        const auto &action = root["actions"].front();
        const auto role = action["role"].get<XRTrackedDeviceRole>();
        REQUIRE(role == XRTrackedDeviceRole::LeftController);
        root["worldUiPointer"] = {{"action", action["id"]}, {"maximumDistanceMeters", 6.0F}};
        root["poses"].push_back({{"profile", 1},
                                 {"set", action["set"]},
                                 {"role", role},
                                 {"kind", XRTrackedPoseKind::Aim},
                                 {"path", "/user/hand/left/input/aim/pose"},
                                 {"label", "Aim"}});
        bool valid = true;
        SECTION("matching aim pose") {}
        SECTION("absent aim pose") {
            root["poses"].clear();
            valid = false;
        }
        SECTION("grip pose on the selected controller") {
            root["poses"][0]["kind"] = XRTrackedPoseKind::Grip;
            root["poses"][0]["path"] = "/user/hand/left/input/grip/pose";
            valid = false;
        }
        SECTION("aim pose on the opposite controller") {
            root["poses"][0]["role"] = XRTrackedDeviceRole::RightController;
            root["poses"][0]["path"] = "/user/hand/right/input/aim/pose";
            valid = false;
        }
        InstalledProductFixture fixture{HORO_TEST_OPENXR_LOADER, root.dump()};
        const auto candidate = fixture.Candidate();
        const auto parsed =
            Application::Internal::ReadInstalledOpenXRBindings(candidate.component,
                                                               RequireValue(Packages::PackagePath::Parse("runtime/xr/bindings.json")));
        REQUIRE(parsed.HasValue() == valid);
        if (valid) {
            REQUIRE(parsed.Value()->worldUiPointer.has_value());
            CHECK(parsed.Value()->worldUiPointer->role == role);
        }
    }

    TEST_CASE("Installed XR bindings retain only bounded explicitly signed baseline haptic metadata",
              "[xr][host][bootstrap][install][haptics]") {
        using Horo::XR::XRTrackedDeviceRole;
        auto root = nlohmann::json::parse(InstalledProductFixture::BindingDocument());
        auto route = nlohmann::json{{"profile", 1},
                                    {"set", 1},
                                    {"role", static_cast<unsigned>(XRTrackedDeviceRole::LeftController)},
                                    {"path", "/user/hand/left/output/haptic"},
                                    {"label", "Vibration"}};
        bool malformed{};
        SECTION("complete declaration") {}
        SECTION("cannot assign a player in native binding files") {
            route["player"] = 0;
            malformed = true;
        }
        SECTION("roles are limited to controllers") {
            route["role"] = static_cast<unsigned>(XRTrackedDeviceRole::Head);
            malformed = true;
        }
        root["haptics"] = nlohmann::json::array({route});
        InstalledProductFixture fixture{HORO_TEST_OPENXR_LOADER, root.dump()};
        const auto candidate = fixture.Candidate();
        const auto parsed =
            Application::Internal::ReadInstalledOpenXRBindings(candidate.component,
                                                               RequireValue(Packages::PackagePath::Parse("runtime/xr/bindings.json")));
        if (malformed) {
            CHECK(parsed.HasError());
        } else {
            REQUIRE(parsed.HasValue());
            REQUIRE(parsed.Value()->haptics.size() == 1);
            const auto &output = parsed.Value()->haptics.front();
            CHECK(output.role == XRTrackedDeviceRole::LeftController);
            CHECK(output.path == "/user/hand/left/output/haptic");
        }
    }

    TEST_CASE("Installed XR locomotion selects one authored actor and typed actions", "[xr][host][bootstrap][install][locomotion]") {
        auto root = nlohmann::json::parse(InstalledProductFixture::BindingDocument());
        auto move = root["actions"].front();
        move["id"] = "locomotion.move";
        move["type"] = static_cast<unsigned>(Input::ActionValueType::Axis2D);
        auto turn = move;
        turn["id"] = "locomotion.turn";
        turn["type"] = static_cast<unsigned>(Input::ActionValueType::Axis1D);
        root["actions"].push_back(move);
        root["actions"].push_back(turn);
        auto selected =
            nlohmann::json{{"actor", 17},
                           {"moveAction", "locomotion.move"},
                           {"speedMetersPerSecond", 2.5},
                           {"deadZone", 0.15},
                           {"snapTurn", {{"action", "locomotion.turn"}, {"angleRadians", 0.5}, {"engage", 0.65}, {"release", 0.25}}}};
        bool malformed{};
        bool armSwing{};
        bool roomScale{};
        bool teleport{};
        bool declareAim{true};
        bool declareBothGrips{true};
        const nlohmann::json physical{{"gain", 1.25}, {"maximumTrackingGapMilliseconds", 120}};
        SECTION("exact authored actor and routes") {}
        SECTION("arm swing selects paired tracking instead of a stick") {
            armSwing = true;
            selected.erase("moveAction");
            selected.erase("deadZone");
            selected["armSwing"] = physical;
        }
        SECTION("teleport requires a digital action with an authored aim pose") {
            teleport = true;
            selected.erase("moveAction");
            selected.erase("deadZone");
            selected["teleport"] = {{"action", "select"}, {"maximumDistanceMeters", 6}};
        }
        SECTION("teleport rejects a binding without aim evidence") {
            selected.erase("moveAction");
            selected.erase("deadZone");
            selected["teleport"] = {{"action", "select"}, {"maximumDistanceMeters", 6}};
            declareAim = false;
            malformed = true;
        }
        SECTION("room scale selects headset-driven Character motion") {
            roomScale = true;
            selected.erase("moveAction");
            selected.erase("deadZone");
            selected["roomScale"] = {{"maximumSampleDistanceMeters", 0.4}, {"maximumTrackingGapMilliseconds", 120}};
        }
        SECTION("room scale rejects an unbounded jump") {
            selected.erase("moveAction");
            selected.erase("deadZone");
            selected["roomScale"] = {{"maximumSampleDistanceMeters", 0.0}, {"maximumTrackingGapMilliseconds", 120}};
            malformed = true;
        }
        SECTION("room scale cannot be combined with a stick") {
            selected["roomScale"] = {{"maximumSampleDistanceMeters", 0.4}, {"maximumTrackingGapMilliseconds", 120}};
            malformed = true;
        }
        SECTION("arm swing requires both declared grip poses") {
            selected.erase("moveAction");
            selected.erase("deadZone");
            selected["armSwing"] = physical;
            declareBothGrips = false;
            malformed = true;
        }
        SECTION("an arm swing route cannot specify a second translation mode") {
            selected["armSwing"] = physical;
            malformed = true;
        }
        SECTION("arm swing requires a bounded fresh tracking gap") {
            selected.erase("moveAction");
            selected.erase("deadZone");
            selected["armSwing"] = physical;
            selected["armSwing"]["maximumTrackingGapMilliseconds"] = 1'001;
            malformed = true;
        }
        SECTION("a digital action cannot act as a movement vector") {
            selected["moveAction"] = root["actions"].front()["id"];
            malformed = true;
        }
        SECTION("a duplicate authored action has no authority") {
            root["actions"].push_back(move);
            malformed = true;
        }
        SECTION("the installation cannot assign a player") {
            selected["player"] = 1;
            malformed = true;
        }
        SECTION("turn release must be below engage") {
            selected["snapTurn"]["release"] = selected["snapTurn"]["engage"];
            malformed = true;
        }
        if (selected.contains("teleport") && declareAim) {
            root["poses"].push_back({{"profile", 1},
                                     {"set", 1},
                                     {"role", static_cast<unsigned>(XR::XRTrackedDeviceRole::LeftController)},
                                     {"kind", static_cast<unsigned>(XR::XRTrackedPoseKind::Aim)},
                                     {"path", "/user/hand/left/input/aim/pose"},
                                     {"label", "Aim"}});
        }
        if (selected.contains("armSwing")) {
            auto grip = nlohmann::json{{"profile", 1},
                                       {"set", 1},
                                       {"role", static_cast<unsigned>(XR::XRTrackedDeviceRole::LeftController)},
                                       {"kind", static_cast<unsigned>(XR::XRTrackedPoseKind::Grip)},
                                       {"path", "/user/hand/left/input/grip/pose"},
                                       {"label", "Grip"}};
            root["poses"].push_back(grip);
            if (declareBothGrips) {
                grip["role"] = static_cast<unsigned>(XR::XRTrackedDeviceRole::RightController);
                grip["path"] = "/user/hand/right/input/grip/pose";
                root["poses"].push_back(std::move(grip));
            }
        }
        root["locomotion"] = std::move(selected);
        InstalledProductFixture fixture{HORO_TEST_OPENXR_LOADER, root.dump()};
        const auto candidate = fixture.Candidate();
        const auto loaded =
            Application::Internal::ReadInstalledOpenXRBindings(candidate.component,
                                                               RequireValue(Packages::PackagePath::Parse("runtime/xr/bindings.json")));
        if (malformed) {
            CHECK(loaded.HasError());
            return;
        }
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value()->locomotion);
        const auto &route = *loaded.Value()->locomotion;
        CHECK(route.actor == 17);
        if (armSwing) {
            const auto *swing = std::get_if<Application::Internal::OpenXRArmSwingBinding>(&route.translation);
            REQUIRE(swing);
            CHECK(swing->gain == 1.25F);
            CHECK(swing->maximumTrackingGapNanoseconds == 120'000'000);
        } else if (teleport) {
            const auto *routeTeleport = std::get_if<Application::Internal::OpenXRTeleportBinding>(&route.translation);
            REQUIRE(routeTeleport);
            CHECK(routeTeleport->action == Input::ActionId{"select"});
            CHECK(routeTeleport->maximumDistanceMeters == 6.0F);
        } else if (roomScale) {
            const auto *room = std::get_if<Application::Internal::OpenXRRoomScaleBinding>(&route.translation);
            REQUIRE(room);
            CHECK(room->maximumSampleDistanceMeters == 0.4F);
            CHECK(room->maximumTrackingGapNanoseconds == 120'000'000);
        } else {
            const auto *stick = std::get_if<Application::Internal::OpenXRStickMoveBinding>(&route.translation);
            REQUIRE(stick);
            CHECK(stick->action == Input::ActionId{"locomotion.move"});
            CHECK(stick->deadZone == 0.15F);
        }
        CHECK(route.speedMetersPerSecond == 2.5F);
        REQUIRE(route.turn);
        CHECK(route.turn->action == Input::ActionId{"locomotion.turn"});
    }

    TEST_CASE("Signed XR locomotion validates the reduced-motion teleport alternative", "[xr][host][bootstrap][install][comfort]") {
        auto root = ReducedMotionTeleportDocument();
        auto &route = root["locomotion"];
        bool valid = true;
        SECTION("A signed digital action and aim are admitted with stick movement") {
            // Keep the complete movement and teleport declaration.
        }
        SECTION("The alternative cannot select an analog action") {
            route["reducedMotionTeleport"]["action"] = "locomotion.move";
            valid = false;
        }
        SECTION("The alternative requires an authored controller aim") {
            root["poses"].clear();
            valid = false;
        }
        SECTION("The alternative cannot exceed the approved ray distance") {
            route["reducedMotionTeleport"]["maximumDistanceMeters"] = 100.1;
            valid = false;
        }
        SECTION("Physical room-scale correction cannot be replaced by the alternative") {
            route.erase("moveAction");
            route.erase("deadZone");
            route["roomScale"] = {{"maximumSampleDistanceMeters", 0.4}, {"maximumTrackingGapMilliseconds", 120}};
            valid = false;
        }
        InstalledProductFixture fixture{HORO_TEST_OPENXR_LOADER, root.dump()};
        const auto candidate = fixture.Candidate();
        const auto loaded =
            Application::Internal::ReadInstalledOpenXRBindings(candidate.component,
                                                               RequireValue(Packages::PackagePath::Parse("runtime/xr/bindings.json")));
        REQUIRE(loaded.HasValue() == valid);
        if (valid) {
            const Input::InputContextId gameplay{"gameplay"};
            const Input::ActionDescriptor select{Input::ActionId{"select"}, Input::ActionValueType::Digital, gameplay, true, {}};
            const Input::ActionDescriptor move{Input::ActionId{"locomotion.move"}, Input::ActionValueType::Axis2D, gameplay, false, {}};
            const std::array registered{select, move};
            REQUIRE(loaded.Value()->ValidateRegisteredActions(registered).HasValue());
            REQUIRE(loaded.Value()->locomotion);
            const auto &selection = *loaded.Value()->locomotion;
            CHECK(selection.SelectedTeleport({}) == nullptr);
            const auto *alternative = selection.SelectedTeleport({.reducedMotion = true, .teleportWhenReduced = true});
            REQUIRE(alternative);
            CHECK(alternative->action == Input::ActionId{"select"});
            CHECK(alternative->maximumDistanceMeters == 6.0F);
        }
    }
}  // namespace Horo::Editor::Tests
