#pragma once
/** @file NavigationSettingsProjectTestSupport.h
 * @brief Genuine registered Navigation and Scene sources shared by session and presentation regressions.
 */
#include "Horo/Editor/NavigationProjectSettingsSession.h"
#include "NativePublicationFiles.h"
#include "NavigationProjectPolicyTestSupport.h"
#include "OwnedTestDirectory.h"

namespace Horo::Tests::NavigationSettingsSupport {
    /** @brief Injects uncertainty only after a genuine native replacement; source identities still advance. */
    class SessionFiles final : public TestSupport::NativePublicationFiles {
    public:
        enum class ReplacementMode {
            Durable,
            Uncertain
        };

        void SetReplacementMode(const ReplacementMode mode) noexcept {
            mode_ = mode;
        }

        [[nodiscard]] std::size_t UncertainReplacements() const noexcept {
            return uncertainReplacements_;
        }

        [[nodiscard]] std::size_t DurableReplacements() const noexcept {
            return durableReplacements_;
        }

        Result<void> WritePrivateDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes, bool &created,
                                         const std::filesystem::perms mode) override {
            return native.WritePrivateDurable(path, bytes, created, mode);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                          AtomicFileReplacementReceipt &receipt) override {
            auto replaced = native.AtomicReplaceTracked(prepared, destination, receipt);
            if (replaced.HasError())
                return replaced;
            if (mode_ == ReplacementMode::Uncertain) {
                ++uncertainReplacements_;
                return Result<void>::Failure(MakeError(N::NavigationSettingsError(N::NavigationSettingsFailure::Invalid)));
            }
            ++durableReplacements_;
            return replaced;
        }

    private:
        ReplacementMode mode_{ReplacementMode::Durable};
        std::size_t uncertainReplacements_{};
        std::size_t durableReplacements_{};
    };

    /** @brief Real project sources, registered identities and canonical active Scene owner. */
    struct SessionProjectFixture final {
        Tests::OwnedTestDirectory directory{"navigation-project"};
        std::filesystem::path root{directory.Path() / std::filesystem::path{u8"çalışma alanı"}};
        SessionFiles files;
        E::ProjectMutationCoordinator mutations{files};
        Assets::AssetRegistry registry;
        E::SceneDocumentTransactions scene = E::SceneDocumentTransactions::Create({Asset(2), 7}, Objects()).Value();

        SessionProjectFixture() {
            REQUIRE(std::filesystem::create_directory(root));
            REQUIRE(
                registry.Publish({Record(1, "core.navmesh", "assets/nav.horoasset"), Record(2, "core.scene", "assets/a.horo")}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
            const auto record = N::EncodeNavigationDefinitionRecord(N::NavigationDefinition::Create(Definition()).Value(),
                                                                    Id<N::NavigationAuthoredRecordId>(1));
            REQUIRE(record.HasValue());
            const std::array support{N::NavigationDefinitionRecordSupport()};
            const N::NavigationSourceLoadContext context{.supportedRecords = support};
            const auto envelope = N::NavigationSourceRecords::Create(N::CurrentNavigationSourceSchemaVersion, {record.Value()}, context);
            REQUIRE(envelope.HasValue());
            const auto bytes = N::SerializeNavigationSourceRecords(envelope.Value());
            REQUIRE(bytes.HasValue());
            REQUIRE(files.WriteDurable(root / "assets/nav.horoasset", bytes.Value()).HasValue());
            const auto text = SceneSource::EncodeSceneSource({Objects(), {}});
            REQUIRE(files.WriteDurable(root / "assets/a.horo", std::as_bytes(std::span{text.data(), text.size()})).HasValue());
        }

        auto Open(const std::uint64_t generation = 7, const std::span<const Application::NavigationTargetProviderEvidence> targets = {}) {
            return E::NavigationProjectSettingsSession::Open(root, registry, files, mutations, generation, &scene, targets);
        }
    };
}  // namespace Horo::Tests::NavigationSettingsSupport
