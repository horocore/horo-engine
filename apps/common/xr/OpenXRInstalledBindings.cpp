#include "OpenXRInstalledBindings.h"

#include "JsonUtils.h"

#include <limits>
#include <ranges>
#include <span>

namespace Horo::Application::Internal {
    namespace {
        using Foundation::Json;

        /** @brief The installer vocabulary cannot manufacture Input/native admission. */
        Error InvalidBindings() {
            return MakeError(XR::XRErrors::OperationInvalid,
                             "Installed XR physical declarations are malformed or exceed bounded schema-one limits.");
        }

        /** @brief Checks bounded strings before any owning copy or native spelling conversion. */
        bool Text(const Json &value, const std::size_t maximum) {
            if (!value.is_string())
                return false;
            const auto &text = value.get_ref<const std::string &>();
            return !text.empty() && text.size() <= maximum && text.find('\0') == std::string::npos;
        }

        /** @brief Rejects narrowing/zero and closed-enum overflow before value extraction. */
        bool Number(const Json &value, const std::uint64_t maximum, const bool allowZero = false) {
            return value.is_number_unsigned() && (allowZero || value.get<std::uint64_t>() != 0) && value.get<std::uint64_t>() <= maximum;
        }

        /** @brief Fixed schema-one collection bounds, validated before allocating any entry. */
        bool Shape(const Json &root) {
            if (!Foundation::HasAllowedFields(root, {"schemaVersion", "genericFallback", "profiles", "sets", "actions", "controls",
                                                     "suggestions", "poses"}) ||
                !root.at("schemaVersion").is_number_unsigned() || root.at("schemaVersion") != 1 ||
                !Number(root.at("genericFallback"), std::numeric_limits<std::uint32_t>::max(), true))
                return false;
            const std::array counts{std::pair{"profiles", XR::XRActionBindingLimits::MaximumProfiles},
                                    std::pair{"sets", XR::XRActionBindingLimits::MaximumSets},
                                    std::pair{"actions", XR::XRActionBindingLimits::MaximumActions},
                                    std::pair{"controls", XR::XRActionBindingLimits::MaximumBindings},
                                    std::pair{"suggestions", XR::XRActionBindingLimits::MaximumBindings},
                                    std::pair{"poses", std::size_t{4}}};
            return std::ranges::all_of(counts, [&root](const auto &count) {
                const auto &[name, maximum] = count;
                return root.at(name).is_array() && root.at(name).size() <= maximum;
            });
        }

        /** @brief Native identity payloads stay bounded while semantic strings remain exact Input keys. */
        bool Identity(const Json &value) {
            return Number(value, std::numeric_limits<std::uint32_t>::max());
        }

        bool Role(const Json &value) {
            using enum Horo::XR::XRTrackedDeviceRole;
            return Number(value, static_cast<std::uint64_t>(RightController), true) &&
                   (value == static_cast<std::uint64_t>(LeftController) || value == static_cast<std::uint64_t>(RightController));
        }

        bool Type(const Json &value) {
            return Number(value, static_cast<std::uint64_t>(Input::ActionValueType::Axis2D), true);
        }

        /** @brief Copies action/set presentation and exact registered-action intent, never calling Input registration. */
        Result<void> Actions(const Json &root, OpenXRApplicationBindings &output) {
            for (const auto &entry : root.at("sets")) {
                if (!Foundation::HasAllowedFields(entry, {"id", "label"}) || !Identity(entry.at("id")) ||
                    !Text(entry.at("label"), XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1))
                    return Result<void>::Failure(InvalidBindings());
                output.setLabels.push_back({{entry.at("id").get<std::uint32_t>()}, entry.at("label").get<std::string>()});
            }
            for (const auto &entry : root.at("actions")) {
                if (!Foundation::HasAllowedFields(entry, {"id", "context", "type", "set", "role", "required", "label"}) ||
                    !Text(entry.at("id"), XR::XRActionBindingLimits::MaximumNameBytes) ||
                    !Text(entry.at("context"), XR::XRActionBindingLimits::MaximumNameBytes) || !Type(entry.at("type")) ||
                    !Identity(entry.at("set")) || !Role(entry.at("role")) || !entry.at("required").is_boolean() ||
                    !Text(entry.at("label"), XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1))
                    return Result<void>::Failure(InvalidBindings());
                const Input::ActionId id{entry.at("id").get<std::string>()};
                output.schema.actions.push_back({id,
                                                 Input::InputContextId{entry.at("context").get<std::string>()},
                                                 static_cast<Input::ActionValueType>(entry.at("type").get<std::uint64_t>()),
                                                 {entry.at("set").get<std::uint32_t>()},
                                                 static_cast<XR::XRTrackedDeviceRole>(entry.at("role").get<std::uint64_t>()),
                                                 entry.at("required").get<bool>()});
                output.actionLabels.emplace_back(id, entry.at("label").get<std::string>());
            }
            return Result<void>::Success();
        }

        /** @brief Copies only bounded profile/control spellings; exact native path policy remains in XROpenXR. */
        Result<void> Controls(const Json &root, OpenXRApplicationBindings &output) {
            for (const auto &entry : root.at("profiles")) {
                if (!Foundation::HasAllowedFields(entry, {"id", "path"}) || !Identity(entry.at("id")) ||
                    !Text(entry.at("path"), XR_MAX_PATH_LENGTH - 1))
                    return Result<void>::Failure(InvalidBindings());
                output.profiles.push_back({{entry.at("id").get<std::uint32_t>()}, entry.at("path").get<std::string>()});
            }
            for (const auto &entry : root.at("controls")) {
                if (!Foundation::HasAllowedFields(entry, {"profile", "id", "role", "type", "path"}) || !Identity(entry.at("profile")) ||
                    !Identity(entry.at("id")) || !Role(entry.at("role")) || !Type(entry.at("type")) ||
                    !Text(entry.at("path"), XR_MAX_PATH_LENGTH - 1))
                    return Result<void>::Failure(InvalidBindings());
                output.controls.push_back({{{entry.at("profile").get<std::uint32_t>()},
                                            {entry.at("id").get<std::uint32_t>()},
                                            static_cast<XR::XRTrackedDeviceRole>(entry.at("role").get<std::uint64_t>()),
                                            static_cast<Input::ActionValueType>(entry.at("type").get<std::uint64_t>())},
                                           entry.at("path").get<std::string>()});
            }
            for (const auto &entry : root.at("suggestions")) {
                if (!Foundation::HasAllowedFields(entry, {"action", "profile", "control"}) ||
                    !Text(entry.at("action"), XR::XRActionBindingLimits::MaximumNameBytes) || !Identity(entry.at("profile")) ||
                    !Identity(entry.at("control")))
                    return Result<void>::Failure(InvalidBindings());
                output.schema.suggestions.push_back({Input::ActionId{entry.at("action").get<std::string>()},
                                                     {entry.at("profile").get<std::uint32_t>()},
                                                     {entry.at("control").get<std::uint32_t>()}});
            }
            return Result<void>::Success();
        }

        /** @brief Private schema supports only the existing grip/aim controller roles, not head/gaze/haptics. */
        Result<void> Poses(const Json &root, OpenXRApplicationBindings &output) {
            for (const auto &entry : root.at("poses")) {
                using enum Horo::XR::XRTrackedPoseKind;
                if (!Foundation::HasAllowedFields(entry, {"profile", "set", "role", "kind", "path", "label"}) ||
                    !Identity(entry.at("profile")) || !Identity(entry.at("set")) || !Role(entry.at("role")) ||
                    !Number(entry.at("kind"), static_cast<std::uint64_t>(Aim), true) ||
                    (entry.at("kind") != static_cast<std::uint64_t>(Grip) && entry.at("kind") != static_cast<std::uint64_t>(Aim)) ||
                    !Text(entry.at("path"), XR_MAX_PATH_LENGTH - 1) || !Text(entry.at("label"), XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1))
                    return Result<void>::Failure(InvalidBindings());
                output.poses.push_back({{entry.at("profile").get<std::uint32_t>()},
                                        {entry.at("set").get<std::uint32_t>()},
                                        static_cast<XR::XRTrackedDeviceRole>(entry.at("role").get<std::uint64_t>()),
                                        static_cast<XR::XRTrackedPoseKind>(entry.at("kind").get<std::uint64_t>()),
                                        entry.at("path").get<std::string>(),
                                        entry.at("label").get<std::string>()});
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ReadInstalledOpenXRBindings */
    Result<std::shared_ptr<const OpenXRApplicationBindings>> ReadInstalledOpenXRBindings(const VerifiedOpenXRComponent &component,
                                                                                         const Packages::PackagePath &path) {
        using Outcome = Result<std::shared_ptr<const OpenXRApplicationBindings>>;
        const auto &package = component.Install()->Graph()->packages[component.PackageIndex()];
        auto bytes = package.archive->ReadDeclaredFile(path, 256U * 1024U);
        if (bytes.HasError())
            return Outcome::Failure(bytes.ErrorValue());
        if (bytes.Value().empty())
            return Outcome::Failure(InvalidBindings());
        try {
            Foundation::JsonParseGuard guard{6};
            const auto text = std::span<const std::byte>{bytes.Value()} | std::views::transform([](const std::byte value) {
                return std::to_integer<unsigned char>(value);
            });
            const auto root = guard.Parse(text.begin(), text.end());
            if (guard.HasDuplicate() || guard.IsTooDeep() || !Shape(root))
                return Outcome::Failure(InvalidBindings());
            auto output = std::make_shared<OpenXRApplicationBindings>();
            output->schema.version = 1;
            output->schema.genericFallback = {root.at("genericFallback").get<std::uint32_t>()};
            if (auto actions = Actions(root, *output); actions.HasError())
                return Outcome::Failure(actions.ErrorValue());
            if (auto controls = Controls(root, *output); controls.HasError())
                return Outcome::Failure(controls.ErrorValue());
            if (auto poses = Poses(root, *output); poses.HasError())
                return Outcome::Failure(poses.ErrorValue());
            return Outcome::Success(std::move(output));
        } catch (const Foundation::Json::exception &) {
            return Outcome::Failure(InvalidBindings());
        }
    }
}  // namespace Horo::Application::Internal
