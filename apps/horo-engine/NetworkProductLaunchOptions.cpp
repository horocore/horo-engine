#include "NetworkProductLaunchOptions.h"

#include "Horo/Gameplay/GameModuleRetirement.h"
#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <system_error>
#include <utility>

namespace Horo::Application::Internal {
    namespace Net = Network;

    namespace {
        [[nodiscard]] bool ParseFrameCount(const std::string_view text, std::uint32_t &frames) noexcept {
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), frames);
            return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && frames > 0 && frames <= 10'000;
        }

        [[nodiscard]] std::optional<Net::NetworkProjectRole> ParseRole(const std::string_view text) noexcept {
            using enum Net::NetworkProjectRole;
            if (text == "standalone")
                return Standalone;
            if (text == "client")
                return Client;
            if (text == "listen")
                return ListenServer;
            if (text == "dedicated")
                return DedicatedServer;
            return std::nullopt;
        }

        /** @brief Validate one native artifact's absolute identity and non-zero descriptor revision. */
        Result<void> ParseNativeGameplay(const std::span<char *> arguments, GameplayWorldSelection &selection) {
            selection.moduleRetirements = std::make_shared<Gameplay::GameModuleRetirementQueue>();
            selection.nativeArtifact = arguments[1];
            selection.moduleId = arguments[2];
            const std::string_view revision = arguments[3];
            if (const auto parsed = std::from_chars(revision.data(), revision.data() + revision.size(), selection.descriptorRevision);
                !selection.nativeArtifact.is_absolute() || parsed.ec != std::errc{} || parsed.ptr != revision.data() + revision.size() ||
                selection.descriptorRevision == 0)
                return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid));
            return Result<void>::Success();
        }

        /** @brief Decode a host-local explicit artifact request; permissions default to denied and are never module claims. */
        [[nodiscard]] Result<GameplayWorldSelection> ParseGameplaySelection(const std::span<char *> arguments) {
            if (arguments.size() != 5)
                return Result<GameplayWorldSelection>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeInvalid,
                              "Gameplay suffix: --game-module <absolute artifact> <module id> <revision> <grant|deny>, or "
                              "--game-script <absolute source> <absolute sidecar> <module id> <grant|deny>."));
            GameplayWorldSelection selection;
            if (const std::string_view kind = arguments[0]; kind == "--game-module") {
                if (const auto valid = ParseNativeGameplay(arguments, selection); valid.HasError())
                    return Result<GameplayWorldSelection>::Failure(valid.ErrorValue());
            } else if (kind == "--game-script") {
                selection.scriptSource = arguments[1];
                selection.scriptSidecar = arguments[2];
                selection.moduleId = arguments[3];
                if (!selection.scriptSource.is_absolute() || !selection.scriptSidecar.is_absolute())
                    return Result<GameplayWorldSelection>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid));
            } else
                return Result<GameplayWorldSelection>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid));
            const std::string_view permission = arguments[4];
            if (permission != "grant" && permission != "deny")
                return Result<GameplayWorldSelection>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid));
            selection.physicsPermission = permission == "grant" ? GameplayPhysicsPermission::Granted : GameplayPhysicsPermission::Denied;
            return Result<GameplayWorldSelection>::Success(std::move(selection));
        }

        /** @brief Validate the selected mode's explicit endpoints before extending its gameplay selection. */
        Result<void> ParseProductEndpoints(const std::span<char *> arguments, ProductLaunch &launch, const bool listener,
                                           const bool outbound) {
            std::size_t endpointIndex = 2;
            if (listener) {
                auto parsed = Net::NetworkAddress::Parse(arguments[endpointIndex++]);
                if (parsed.HasError() || parsed.Value().RequiresResolution())
                    return Result<void>::Failure(
                        MakeError(Net::NetworkErrors::NetworkModeInvalid, "Listener needs a numeric bind endpoint."));
                launch.bind = parsed.Value();
            }
            if (outbound) {
                auto parsed = Net::NetworkAddress::Parse(arguments[endpointIndex]);
                if (parsed.HasError())
                    return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid, "Invalid outbound endpoint."));
                launch.connect = parsed.Value();
            }
            return Result<void>::Success();
        }

        enum class SceneOption : std::size_t {
            Project,
            Scene,
            CookRoot,
            CookTarget,
            Physics,
            Timeout,
            Navigation,
            Count
        };
        constexpr std::array<std::string_view, static_cast<std::size_t>(SceneOption::Count)>
            SceneOptionNames{"--project", "--scene", "--cooked-root", "--cook-target", "--physics", "--load-timeout-ms", "--navigation"};
        using SeenSceneOptions = std::array<bool, static_cast<std::size_t>(SceneOption::Count)>;

        /** @brief Applies one admitted unique option; paths retain the canonical native UTF-8 conversion. */
        Result<void> ApplySceneOption(const SceneOption option, const std::string_view value, HeadlessProjectSceneOptions &input) {
            using enum SceneOption;
            switch (option) {
                case Project:
                case CookRoot: {
                    auto path = ParseSceneHostAbsolutePath(value);
                    if (path.HasError())
                        return Result<void>::Failure(path.ErrorValue());
                    (option == Project ? input.projectRoot : input.cookedRoot) = std::move(path).Value();
                    break;
                }
                case Scene:
                    input.scene = value;
                    break;
                case CookTarget:
                    input.cookTarget = value;
                    break;
                case Physics: {
                    const auto mode = ParseScenePhysicsHostMode(value);
                    if (!mode)
                        return Result<void>::Failure(
                            MakeError(Net::NetworkErrors::NetworkModeInvalid, "Physics mode must be null or canonical."));
                    input.physicsMode = *mode;
                    break;
                }
                case Timeout: {
                    std::uint32_t milliseconds{};
                    if (const auto parsed = std::from_chars(value.data(), value.data() + value.size(), milliseconds);
                        parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || milliseconds == 0 || milliseconds > 60'000)
                        return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid));
                    input.loadTimeout = std::chrono::milliseconds{milliseconds};
                    break;
                }
                case Navigation:
                    if (value != "recast-detour")
                        return Result<void>::Failure(
                            MakeError(Net::NetworkErrors::NetworkModeInvalid, "Navigation mode must be recast-detour."));
                    input.navigationMode = HeadlessNavigationMode::RecastDetour;
                    break;
                case Count:
                    return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Validates coupled options before any project I/O or backend admission. */
        Result<void> ValidateSceneOptions(const SeenSceneOptions &seen, const HeadlessProjectSceneOptions &input) {
            const auto has = [&seen](const SceneOption option) {
                return seen[static_cast<std::size_t>(option)];
            };
            if (has(SceneOption::Project) != has(SceneOption::Scene) || has(SceneOption::CookRoot) != has(SceneOption::CookTarget) ||
                (!has(SceneOption::Scene) && (has(SceneOption::Physics) || has(SceneOption::CookRoot) || has(SceneOption::Timeout))))
                return Result<void>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeInvalid, "Pair --project/--scene and --cooked-root/--cook-target."));
            if (has(SceneOption::Scene) &&
                (!input.projectRoot.is_absolute() || (has(SceneOption::CookRoot) && !input.cookedRoot.is_absolute()) ||
                 (!input.scene.starts_with("source:") && !input.scene.starts_with("cooked:"))))
                return Result<void>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeInvalid,
                              "Use an absolute project/cook root and source:<project path> or cooked:<Scene AssetId>."));
            if (has(SceneOption::Navigation) && (!input.scene.starts_with("cooked:") || !has(SceneOption::CookRoot)))
                return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid,
                                                       "Navigation requires a durable cooked Scene and committed cook generation."));
            return Result<void>::Success();
        }

        /** @brief Decodes one independent capability grant without inferring authority from cooked-file presence. */
        Result<void> ReadPrefabPermission(std::span<char *> arguments, std::size_t &index, std::optional<bool> &grant) {
            if (grant.has_value() || arguments.size() - index < 2)
                return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid, "Duplicate or missing prefab permission."));
            const std::string_view value{arguments[index + 1]};
            if (value != "grant" && value != "deny")
                return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid, "Prefab permission must be grant or deny."));
            grant = value == "grant";
            index += 2;
            return Result<void>::Success();
        }

        /** @brief Validates the complete option set before recording any explicit prefab grant. */
        Result<void> ApplyPrefabPermission(ProductLaunch &launch, std::optional<bool> grant) {
            if (!grant.has_value())
                return Result<void>::Success();
            if (!launch.gameplay || (*grant && launch.sceneInput.scene.empty()))
                return Result<void>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeInvalid,
                              "Prefab permission requires selected gameplay and grant requires a configured project Scene."));
            launch.gameplay->prefabPermission = *grant ? GameplayPrefabPermission::Granted : GameplayPrefabPermission::Denied;
            return Result<void>::Success();
        }

        /** @brief Reads one unique paired Scene option and advances only after successful typed validation. */
        Result<void> ReadSceneOption(const std::span<char *> arguments, std::size_t &index, SeenSceneOptions &seen,
                                     HeadlessProjectSceneOptions &input) {
            const std::string_view key{arguments[index]};
            if (arguments.size() - index < 2 || std::string_view{arguments[index + 1]}.empty())
                return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid, "A Scene option is missing its value."));
            const auto found = std::ranges::find(SceneOptionNames, key);
            if (found == SceneOptionNames.end())
                return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid, "Unknown product Scene option."));
            const auto optionIndex = static_cast<std::size_t>(found - SceneOptionNames.begin());
            if (seen[optionIndex])
                return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid, "Duplicate product Scene option."));
            if (const auto applied = ApplySceneOption(static_cast<SceneOption>(optionIndex), arguments[index + 1], input);
                applied.HasError())
                return applied;
            seen[optionIndex] = true;
            index += 2;
            return Result<void>::Success();
        }

        /** @brief Consume exactly one gameplay module selection without advancing on failure. */
        Result<void> ReadGameplaySelection(const std::span<char *> arguments, std::size_t &index, ProductLaunch &launch) {
            if (launch.gameplay || arguments.size() - index < 5)
                return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid));
            auto selection = ParseGameplaySelection(arguments.subspan(index, 5));
            if (selection.HasError())
                return Result<void>::Failure(selection.ErrorValue());
            launch.gameplay = std::move(selection).Value();
            index += 5;
            return Result<void>::Success();
        }

        /** @brief Decodes one exact controller and stable project asset without loading either. */
        Result<void> ReadCookedDecision(const std::span<char *> arguments, std::size_t &index, ProductLaunch &launch,
                                        const bool fromPrefab) {
            const std::size_t count = fromPrefab ? 4 : 3;
            if (arguments.size() - index < count)
                return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid,
                                                       fromPrefab
                                                           ? "Use --ai-prefab-decision <controller id> <Decision AssetId> <Prefab AssetId>."
                                                           : "Use --ai-decision <controller id> <Decision AssetId>."));
            const std::string_view identity{arguments[index + 1]};
            std::uint64_t value{};
            const auto parsed = std::from_chars(identity.data(), identity.data() + identity.size(), value);
            auto controller = AI::ControllerTypeId::Create(value);
            if (parsed.ec != std::errc{} || parsed.ptr != identity.data() + identity.size() || controller.HasError())
                return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid, "Invalid AI controller identity."));
            auto asset = Assets::AssetId::Parse(arguments[index + 2]);
            if (asset.HasError())
                return Result<void>::Failure(asset.ErrorValue());
            HeadlessCookedDecisionBinding binding{controller.Value(), std::move(asset).Value()};
            if (fromPrefab) {
                auto prefab = Assets::AssetId::Parse(arguments[index + 3]);
                if (prefab.HasError())
                    return Result<void>::Failure(prefab.ErrorValue());
                binding.prefab.emplace(std::move(prefab).Value());
            }
            launch.cookedDecisionBindings.push_back(std::move(binding));
            index += count;
            return Result<void>::Success();
        }

        /** @brief Decodes the Scene's missing-controller policy independently of explicit asset selection. */
        Result<void> ReadMissingControllerPolicy(const std::span<char *> arguments, std::size_t &index, ProductLaunch &launch) {
            if (launch.aiMissingController || arguments.size() - index < 2)
                return Result<void>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeInvalid, "Duplicate or missing AI controller policy."));
            const std::string_view value{arguments[index + 1]};
            if (value != "reject" && value != "degrade")
                return Result<void>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeInvalid, "Use --ai-missing-controller <reject|degrade>."));
            launch.aiMissingController =
                value == "degrade" ? AI::AiMissingControllerPolicy::PreserveInactive : AI::AiMissingControllerPolicy::Reject;
            index += 2;
            return Result<void>::Success();
        }

        /** @brief Requires the authoritative terminal path and the matching native/cooked authority. */
        Result<void> ValidateCookedDecisions(const ProductLaunch &launch) {
            if (launch.aiMissingController && (launch.role != Net::NetworkProjectRole::Standalone || launch.sceneInput.scene.empty()))
                return Result<void>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeInvalid, "AI controller policy requires a configured standalone Scene."));
            if (!launch.cookedDecisionBindings.empty() &&
                (launch.role != Net::NetworkProjectRole::Standalone || !launch.gameplay || launch.gameplay->nativeArtifact.empty() ||
                 !launch.sceneInput.scene.starts_with("cooked:") || launch.sceneInput.cookedRoot.empty()))
                return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid,
                                                       "AI decisions require standalone mode, a native gameplay module, a "
                                                       "cooked Scene and its committed generation."));
            if (launch.gameplay && launch.gameplay->prefabPermission != GameplayPrefabPermission::Granted &&
                std::ranges::any_of(launch.cookedDecisionBindings, [](const auto &binding) {
                return binding.prefab.has_value();
            }))
                return Result<void>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeInvalid, "AI prefab selection requires explicit --prefab grant."));
            return Result<void>::Success();
        }

        /** @brief Keeps legacy gameplay suffix parsing separate from paired explicit Scene options. */
        Result<void> ParseSceneOptions(const std::span<char *> arguments, ProductLaunch &launch) {
            SeenSceneOptions seen{};
            std::optional<bool> prefabGrant;
            for (std::size_t index = 0; index < arguments.size();) {
                const std::string_view key{arguments[index]};
                if (key == "--transport-qualification") {
                    if (launch.purpose != Net::NetworkWorldCompositionPurpose::Gameplay)
                        return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid));
                    launch.purpose = Net::NetworkWorldCompositionPurpose::TransportQualification;
                    ++index;
                    continue;
                }
                if (key == "--prefab") {
                    if (auto permission = ReadPrefabPermission(arguments, index, prefabGrant); permission.HasError())
                        return permission;
                    continue;
                }
                if (key == "--game-module" || key == "--game-script") {
                    if (auto selected = ReadGameplaySelection(arguments, index, launch); selected.HasError())
                        return selected;
                    continue;
                }
                if (key == "--ai-missing-controller") {
                    if (auto policy = ReadMissingControllerPolicy(arguments, index, launch); policy.HasError())
                        return policy;
                    continue;
                }
                if (key == "--ai-decision" || key == "--ai-prefab-decision") {
                    if (auto selected = ReadCookedDecision(arguments, index, launch, key == "--ai-prefab-decision"); selected.HasError())
                        return selected;
                    continue;
                }
                if (auto applied = ReadSceneOption(arguments, index, seen, launch.sceneInput); applied.HasError())
                    return applied;
            }
            if (launch.gameplay && launch.purpose == Net::NetworkWorldCompositionPurpose::TransportQualification)
                return Result<void>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeInvalid, "Transport qualification cannot request a gameplay module."));
            if (auto permission = ApplyPrefabPermission(launch, prefabGrant); permission.HasError())
                return permission;
            if (auto valid = ValidateSceneOptions(seen, launch.sceneInput); valid.HasError())
                return valid;
            return ValidateCookedDecisions(launch);
        }

    }  // namespace

    /** @copydoc ParseNetworkProductLaunch */
    [[nodiscard]] Result<ProductLaunch> ParseNetworkProductLaunch(const std::span<char *> arguments) {
        using enum Net::NetworkProjectRole;
        if (arguments.size() < 2) {
            return Result<ProductLaunch>::Failure(
                MakeError(Net::NetworkErrors::NetworkModeInvalid,
                          "Use <standalone|client|listen|dedicated> <frames 1..10000> [bind] [connect]."));
        }
        const auto role = ParseRole(arguments[0]);
        ProductLaunch launch;
        if (!role.has_value() || !ParseFrameCount(arguments[1], launch.frames))
            return Result<ProductLaunch>::Failure(
                MakeError(Net::NetworkErrors::NetworkModeInvalid, "Invalid mode or bounded frame count."));
        launch.role = *role;
        const bool listener = *role == DedicatedServer || *role == ListenServer;
        const bool outbound = *role == Client || *role == ListenServer;
        const std::size_t modeArgumentCount = 2 + static_cast<std::size_t>(listener) + static_cast<std::size_t>(outbound);
        if (arguments.size() < modeArgumentCount)
            return Result<ProductLaunch>::Failure(
                MakeError(Net::NetworkErrors::NetworkModeInvalid, "Selected mode requires exact explicit bind/connect endpoints."));
        if (const auto valid = ParseProductEndpoints(arguments, launch, listener, outbound); valid.HasError())
            return Result<ProductLaunch>::Failure(valid.ErrorValue());
        if (const auto options = ParseSceneOptions(arguments.subspan(modeArgumentCount), launch); options.HasError())
            return Result<ProductLaunch>::Failure(options.ErrorValue());
        return Result<ProductLaunch>::Success(std::move(launch));
    }

}  // namespace Horo::Application::Internal
