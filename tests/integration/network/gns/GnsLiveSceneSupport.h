#pragma once
/** @file GnsLiveSceneSupport.h @brief Test-only authored Scene and exact signed replication grants for the native pair. */

#include "../../../helpers/GameplayModuleTestSupport.h"
#include "../../../unit/runtime/physics/PhysicsSceneActivationTestSupport.h"
#include "../NetworkAdmissionProductTestSupport.h"
#include "../NetworkProductPrefabTestSupport.h"
#include "GnsEvidenceAssertions.h"
#include "HeadlessInitialStateAuthority.h"
#include "Horo/Network/NetworkAuthoredScenePage.h"
#include "Horo/Network/NetworkPrefabLifecycle.h"

namespace Horo::Network::NativeQualification {
    namespace LiveScene {
        namespace App = Horo::Application::Internal;
        using TestSupport::Id;
        using TestSupport::WireIdentity;
        inline constexpr Runtime::SceneObjectId Object{1};
        inline constexpr float ClientStartY = -100.0F;
        inline constexpr float AuthorityStartY = 25.0F;

        /** @brief Distinct registered Control/Command/Snapshot routes for the same production admission. */
        struct Routes final {
            explicit Routes(const AdmissionTests::ProductPair &fixture) : binding(fixture.WorldPolicy()) {
                std::ranges::copy(fixture.features, features.begin());
                features[2] = WireIdentity<ProtocolFeatureId>(3);
                features[3] = WireIdentity<ProtocolFeatureId>(4);
                std::ranges::copy(fixture.channels, channels.begin());
                channels[2] = {ChannelId::Create(2, 3).Value(), DeliveryPolicy::UnreliableUnordered, MessageTrafficClass::Snapshot, false};
                // Append to the existing signed fixture identities without changing its handshake or command routes.
                std::vector<MessageIdentityDescriptor> messages(fixture.messages.begin(), fixture.messages.end());
                std::vector<MessageCodecDescriptor> descriptors(fixture.codecDescriptors.begin(), fixture.codecDescriptors.end());
                const auto add = [&](std::uint16_t ordinal, std::size_t maximum) {
                    auto &message = messages.emplace_back(binding.control.protocol, WireIdentity<MessageTypeId>(ordinal),
                                                          WireIdentity<MessageSchemaId>(ordinal), MessageSchemaVersion{1, 0}, false);
                    descriptors.push_back({message.protocol, message.id, message.schema, {{1, 0}, {1, 0}}, maximum});
                    return message;
                };
                binding.authoredScene = NetworkAuthoredScenePolicy{add(110, MaximumNetworkAuthoredScenePageBytes), binding.feature};
                binding.prefabLifecycle = NetworkPrefabLifecyclePolicy{add(111, MaximumNetworkPrefabLifecycleBytes), binding.feature};
                binding.initialState = NetworkInitialStatePolicy{add(112, MaximumNetworkInitialStateFrameBytes), features[2], 4096};
                binding.stateUpdates = NetworkStateUpdatePolicy{add(113, 512), features[3], channels[2].channel, 4, 4096};
                std::array<FeatureIdentityDescriptor, 4> featureDescriptors;
                for (std::size_t index{}; index < features.size(); ++index)
                    featureDescriptors[index] = {binding.control.protocol, features[index], {1, 0}};
                const auto identities = ProtocolIdentityRegistry::Create({fixture.protocols, messages, featureDescriptors, {}}).Value();
                codecs = std::make_shared<const MessageCodecRegistry>(MessageCodecRegistry::Create({descriptors, {}}, identities).Value());
            }

            void Apply(NetworkAdmissionPolicy &policy) const {
                policy.local.compatibility.features = {features, features};
                policy.local.transport.maximumChannels = 3;
                policy.local.transport.delivery[static_cast<std::size_t>(DeliveryPolicy::UnreliableUnordered)] =
                    TransportSupport::Available;
                policy.requirements.requiredChannels = 3;
                policy.requirements.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::UnreliableUnordered)] = true;
                policy.gameplayChannels = channels;
            }

            NetworkWorldBindingPolicy binding;
            std::array<ProtocolFeatureId, 4> features;
            std::array<MessageDeliveryChannel, 3> channels;
            std::shared_ptr<const MessageCodecRegistry> codecs;
        };

        /** @brief Binds an explicit test gameplay peer to the actual locally witnessed signed authentication. */
        class Grants final : public App::IHeadlessInitialStateAuthority,
                             public App::IHeadlessInitialReplicaAuthority,
                             public INetworkInitialStateHost {
        public:
            explicit Grants(std::shared_ptr<const CredentialObservation> observed) : observed_(std::move(observed)) {}

            Result<App::HeadlessInitialRecipient> Resolve(const AuthenticationResult &authentication,
                                                          const ReplicationWorldReadLease &world) const override {
                using Grant = Result<App::HeadlessInitialRecipient>;
                if (!Matches(authentication))
                    return Grant::Failure(MakeError(NetworkErrors::AuthenticationRejected));
                App::HeadlessInitialRecipient recipient{Id<NetworkPeerId>(71), 9, {}};
                for (const auto &mapping : world.Mapping().Entries())
                    recipient.ownership.push_back({mapping.object, Id<ReplicationRoleRevision>(6), {}});
                return Grant::Success(std::move(recipient));
            }

            Result<std::uint64_t> DescriptorGeneration(const AuthenticationResult &authentication,
                                                       const HandshakeSelection &negotiated) const override {
                if (!Matches(authentication) || negotiated.connection != authentication.connection ||
                    negotiated.sessionGeneration != authentication.sessionGeneration)
                    return Result<std::uint64_t>::Failure(MakeError(NetworkErrors::AuthenticationRejected));
                return Result<std::uint64_t>::Success(9);
            }

            Result<NetworkPeerId> ClientRecipient(const PeerSessionLifecycle &peer) override {
                if (!peer.Authentication() || !Matches(*peer.Authentication()))
                    return Result<NetworkPeerId>::Failure(MakeError(NetworkErrors::AuthenticationRejected));
                return Result<NetworkPeerId>::Success(Id<NetworkPeerId>(71));
            }

            Result<std::optional<ReplicationInitialStateSnapshot>> Capture(const ReplicationWorldReadLease &,
                                                                           const ReplicationWorldLifecycle &, const PeerSessionLifecycle &,
                                                                           std::size_t) override {
                FAIL("The native Scene authority must capture its own canonical state.");
                return Result<std::optional<ReplicationInitialStateSnapshot>>::Failure(
                    MakeError(NetworkErrors::ReplicationWorldUnavailable));
            }

            Result<NetworkInitialStateTarget> PrepareReceiver(const ReplicationInboundAuthority &, ReplicationWorldLifecycle &,
                                                              std::span<const ReplicationInitialOwnership>, std::uint64_t) override {
                FAIL("The native Scene receiver must apply through its own scheduler.");
                return Result<NetworkInitialStateTarget>::Failure(MakeError(NetworkErrors::ReplicationWorldUnavailable));
            }

        private:
            bool Matches(const AuthenticationResult &authentication) const {
                if (!observed_->grant)
                    return false;
                const auto &grant = *observed_->grant;
                return grant.challenge.connection == authentication.connection &&
                       grant.challenge.sessionGeneration == authentication.sessionGeneration &&
                       grant.result.principal.session == authentication.principal.session &&
                       grant.result.principal.principal == authentication.principal.principal;
            }

            std::shared_ptr<const CredentialObservation> observed_;
        };

        /** @brief Owns one native test module registry, authored resource boundary and observable Scene service. */
        struct Product final {
            Product(const AdmissionTests::ProductPair &fixture, const std::shared_ptr<CredentialObservation> &credentials, bool authority)
                : routes(fixture), grants(std::make_shared<Grants>(credentials)), authority(authority) {
                auto settings = DefaultNetworkProjectSettings(Id<NetworkProjectSettingsId>(10)).Value();
                settings.supportedRoles = NetworkProjectRoleSet::Client | NetworkProjectRoleSet::DedicatedServer;
                settings.defaultRole = NetworkProjectRole::Client;
                settings.protocol = {fixture.protocols[0].id, {{1, 0}, {1, 0}}, 42};
                settings.transport.requirement = NetworkProjectTransportRequirement::Required;
                settings.transport.capabilities.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
                settings.transport.capabilities.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::UnreliableUnordered)] = true;
                settings.transport.capabilities.requiredChannels = 3;
                settings.transport.capabilities.requiredMaximumMessageBytes = 1200;
                settings.replication = {.completeness = NetworkReplicationInventoryCompleteness::Complete,
                                        .declarations = {PrefabProductTestSupport::NativePrefabDeclaration()}};
                project.emplace(NetworkProjectSettings::Create(settings).Value());
                const auto &profile = project->Profile();
                const App::HeadlessAuthorityUpdateLimits limits{profile.maxCapturedObjectsPerTick.value,
                                                                profile.maxCandidatesPerConnectionPerTick.value,
                                                                profile.maxSerializedFieldsPerConnectionPerTick.value};
                state = authority ? std::make_shared<App::HeadlessInitialStateHost>(grants, grants, nullptr, nullptr, limits)
                                  : std::make_shared<App::HeadlessInitialStateHost>(nullptr, grants, grants);
            }

            [[nodiscard]] Result<App::NetworkProductComposition> Compose(NetworkModeFactories factories) {
                const auto declaration = PrefabProductTestSupport::NativePrefabDeclaration();
                loadBoundary = {{}, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
                App::HeadlessSceneActivationInput
                    input{.mode = Physics::PhysicsRuntimeMode::Canonical,
                          .settings = Physics::Settings(),
                          .registry = registry,
                          .loads = loads,
                          .load = loadBoundary,
                          .prefabLoads = &loads,
                          .prefabTarget = AssetCookTargetId::Parse("linux-x64").Value(),
                          .replicatedSceneObjects = {{declaration.schema.id, declaration.schema.version, Object}},
                          .publicTransforms = App::HeadlessSceneTransformReplication{declaration.schema.id,
                                                                                     declaration.schema.version,
                                                                                     {{declaration.schema.fields.front().id,
                                                                                       SceneReplicationProperty::TranslationY}}},
                          .initialSceneState = state};
                const App::GameplayWorldSelection selection{.moduleId = "game.tests",
                                                            .nativeArtifact = HORO_TEST_GAME_MODULE_PATH,
                                                            .descriptorRevision =
                                                                Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                                                            .prefabPermission = App::GameplayPrefabPermission::Granted,
                                                            .moduleRetirements = std::make_shared<Gameplay::GameModuleRetirementQueue>()};
                Gameplay::GameplayReplicationAdmissionPolicy admission{.inventory = project->ReplicationInventory()};
                if (authority)
                    admission.use = ReplicationDeclarationUse::ServerActivation;
                return App::ComposeHeadlessSceneNetworkServices(Definition(), input, std::move(factories), selection, admission);
            }

            [[nodiscard]] std::optional<float> SceneY() const {
                const auto view = scene ? App::InspectHeadlessScene(*scene) : std::nullopt;
                const auto entity = view ? view->Find(Object) : std::nullopt;
                if (!entity)
                    return std::nullopt;
                return view->Get(*entity).Value().localTransform->translation.y;
            }

            Routes routes;
            std::optional<NetworkProjectSettings> project;
            std::shared_ptr<Grants> grants;
            std::shared_ptr<App::HeadlessInitialStateHost> state;
            const INetworkModeService *scene{};  // Synchronous host-owned view, cleared before host teardown.

        private:
            std::shared_ptr<const Runtime::RuntimeSceneDefinition> Definition() {
                Runtime::SceneDefinitionBuilder builder{{1700}, {1}};
                Runtime::RuntimeEntityDefinition entity{.object = Object};
                entity.localTransform.translation.y = authority ? AuthorityStartY : ClientStartY;
                if (authority)
                    entity.components.behaviors.push_back(
                        {{1}, Gameplay::BehaviorTypeId::Parse("game.tests.replicated_presentation_motion").Value()});
                builder.Add(std::move(entity));
                return std::make_shared<const Runtime::RuntimeSceneDefinition>(std::move(builder).Build().Value());
            }

            const bool authority;
            JobSystem jobs{{1, 8}};
            Assets::AssetRegistry registry;
            Assets::MemoryAssetProvider provider;
            Assets::AssetLoadService loads{jobs, provider};
            App::HeadlessSceneLoadBoundary loadBoundary; /**< Borrowed through Scene startup and retirement. */
        };
    }  // namespace LiveScene
}  // namespace Horo::Network::NativeQualification
