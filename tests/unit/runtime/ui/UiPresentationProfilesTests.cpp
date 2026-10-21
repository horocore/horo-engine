#include "Horo/Runtime/Ui/UiPresentationProfiles.h"
#include "UiHotReloadTestFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Id> Id ProfileId(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return Id::Create(bytes).Value();
        }

        template <typename Revision> Revision ProfileRevision(const std::uint64_t value = 1) {
            return Revision::Create(value).Value();
        }

        UiPresentationProfileEvidence ProfileEvidence(const std::int32_t width = 640, const std::int32_t height = 480) {
            return {ProfileId<UiCanvasId>(2),
                    ReloadTests::Instance(),
                    {ReloadTests::Owner(), 2, 1},
                    ProfileRevision<UiRuntimeTreeRevision>(),
                    {ReloadTests::Owner(), 1, 1},
                    ProfileRevision<UiPresentationEvidenceRevision>(),
                    {width, height}};
        }

        struct ProfileTableFixture final {
            UiElementSlotAllocator allocator;
            UiReloadGeneration generation;
            UiPresentationProfileTable table;

            explicit ProfileTableFixture(const std::span<const UiPresentationProfile> rows = {})
                : allocator(std::move(UiElementSlotAllocator::Create(ReloadTests::Owner())).Value()),
                  generation(ReloadTests::Generation(allocator, 1)), table(Create(generation, rows)) {}

            static UiPresentationProfileTable Create(const UiReloadGeneration &generation,
                                                     const std::span<const UiPresentationProfile> rows) {
                auto result = UiPresentationProfileTable::Create(generation, ProfileId<UiCanvasId>(2),
                                                                 ProfileRevision<UiPresentationProfileRevision>(), rows);
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            const UiElementTree &Tree() const {
                return generation.Canvases().front().tree;
            }

            auto Rows() const {
                return table.Rows();
            }

            auto Select(const UiPresentationProfileEvidence &evidence) const {
                return table.Select(Tree(), evidence);
            }

            auto ResolveLayout(const UiPresentationProfileSelection &selection, const UiElementId element,
                               const UiLayoutStyle &base) const {
                return table.ResolveLayout(Tree(), selection, element, base);
            }
        };

        ProfileTableFixture ProfileTable(const std::span<const UiPresentationProfile> rows = {}) {
            return ProfileTableFixture{rows};
        }

        TEST_CASE("Responsive profiles select canonical priority and half-open exact breakpoints", "[runtime_ui][responsive]") {
            UiPresentationProfile wide{ProfileId<UiPresentationProfileId>(5), 20};
            wide.selector.width.minimum = 640;
            UiPresentationProfile narrow{ProfileId<UiPresentationProfileId>(6), 10};
            narrow.selector.width.maximum = 640;
            const std::array rows{wide, narrow};
            const auto table = ProfileTable(rows);
            REQUIRE(table.Rows().front().id == narrow.id);
            REQUIRE(table.Select(ProfileEvidence(639)).Value().profile == narrow.id);
            REQUIRE(table.Select(ProfileEvidence(640)).Value().profile == wide.id);
            REQUIRE(table.Select(ProfileEvidence(641)).Value().profile == wide.id);
            REQUIRE_FALSE(ProfileTable().Select(ProfileEvidence()).Value().profile.has_value());
        }

        TEST_CASE("Responsive exact aspect and independent capabilities do not infer platform or modality", "[runtime_ui][responsive]") {
            UiPresentationProfile row{ProfileId<UiPresentationProfileId>(5), 1};
            row.selector.aspect.minimum = UiPresentationAspectRatio{4, 3};
            row.selector.aspect.maximum = UiPresentationAspectRatio{16, 9};
            row.selector.formFactor = UiPresentationFormFactor::Television;
            row.selector.viewRole = UiPresentationViewRole::Split;
            row.selector.required = UiPresentationCapabilities::Touch;
            const std::array rows{row};
            const auto table = ProfileTable(rows);
            auto evidence = ProfileEvidence(640, 480);
            REQUIRE_FALSE(table.Select(evidence).Value().profile.has_value());
            evidence.formFactor = UiPresentationFormFactor::Television;
            evidence.viewRole = UiPresentationViewRole::Split;
            evidence.available = UiPresentationCapabilities::Gamepad;
            REQUIRE_FALSE(table.Select(evidence).Value().profile.has_value());
            evidence.available = static_cast<UiPresentationCapabilities>(12);
            REQUIRE(table.Select(evidence).Value().profile == row.id);
            evidence.content = {639, 480};
            REQUIRE_FALSE(table.Select(evidence).Value().profile.has_value());
            evidence.content = {1600, 900};
            REQUIRE_FALSE(table.Select(evidence).Value().profile.has_value());
            evidence.content = {1599, 900};
            REQUIRE(table.Select(evidence).Value().profile == row.id);
        }

        TEST_CASE("Responsive rational comparisons remain exact at admitted integer limits", "[runtime_ui][responsive]") {
            constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
            UiPresentationProfile row{ProfileId<UiPresentationProfileId>(5), 1};
            row.selector.aspect.minimum = UiPresentationAspectRatio{static_cast<std::uint32_t>(maximum), 1};
            const std::array rows{row};
            const auto table = ProfileTable(rows);
            REQUIRE(table.Select(ProfileEvidence(maximum, 1)).Value().profile == row.id);
            REQUIRE_FALSE(table.Select(ProfileEvidence(maximum - 1, 1)).Value().profile.has_value());
            REQUIRE_FALSE(table.Select(ProfileEvidence(maximum, maximum)).Value().profile.has_value());
            UiPresentationAspectRange range{{UiPresentationAspectRatio{4294967294U, 4294967295U}},
                                            {UiPresentationAspectRatio{4294967295U, 4294967294U}}};
            REQUIRE(range.IsValid());
        }

        TEST_CASE("Responsive complete layouts preserve neutral base and exact selection lineage", "[runtime_ui][responsive]") {
            UiLayoutStyle base;
            base.width = UiLength::Dip(80);
            UiLayoutStyle compact = base;
            compact.container.orientation = UiLayoutOrientation::Horizontal;
            compact.width = UiLength::Dip(120);
            UiPresentationProfile row{ProfileId<UiPresentationProfileId>(5), 1};
            row.selector.width.maximum = 640;
            row.overrides.push_back({ProfileId<UiElementId>(10), compact});
            const std::array rows{row};
            const auto table = ProfileTable(rows);
            const auto selected = table.Select(ProfileEvidence(639)).Value();
            REQUIRE(table.ResolveLayout(selected, ProfileId<UiElementId>(10), base).Value() == compact);
            REQUIRE(table.ResolveLayout(selected, ProfileId<UiElementId>(11), base).Value() == base);
            const auto neutral = table.Select(ProfileEvidence()).Value();
            REQUIRE(table.ResolveLayout(neutral, ProfileId<UiElementId>(10), base).Value() == base);
            auto stale = selected;
            stale.documentRevision = ProfileRevision<UiDocumentRevision>(2);
            REQUIRE(table.ResolveLayout(stale, ProfileId<UiElementId>(10), base).HasError());
            auto forged = selected;
            forged.profile.reset();
            forged.priority.reset();
            REQUIRE(table.ResolveLayout(forged, ProfileId<UiElementId>(10), base).HasError());
            auto changedEvidence = selected;
            changedEvidence.evidence.content.width = 640;
            REQUIRE(table.ResolveLayout(changedEvidence, ProfileId<UiElementId>(10), base).HasError());
            REQUIRE(table.ResolveLayout(selected, ProfileId<UiElementId>(99), base).HasError());
            auto foreign = ProfileEvidence();
            foreign.canvas = ProfileId<UiCanvasId>(99);
            REQUIRE(table.Select(foreign).HasError());
        }

        TEST_CASE("Responsive bulk base application validates every descriptor before changing geometry", "[runtime_ui][responsive]") {
            UiLayoutStyle compact;
            compact.container.orientation = UiLayoutOrientation::Horizontal;
            UiPresentationProfile row{ProfileId<UiPresentationProfileId>(5), 1};
            row.overrides.push_back({ProfileId<UiElementId>(10), compact});
            const std::array rows{row};
            auto fixture = ProfileTable(rows);
            const auto selected = fixture.Select(ProfileEvidence()).Value();
            const auto &tree = fixture.Tree();
            std::array descriptors{UiLayoutElementDescriptor{tree.Find(ProfileId<UiElementId>(10)).Value(), {}, {}},
                                   UiLayoutElementDescriptor{tree.Find(ProfileId<UiElementId>(11)).Value(), {}, {}}};
            const auto original = descriptors;
            ++descriptors.back().element.generation;
            REQUIRE(fixture.table.ApplyLayouts(tree, selected, descriptors).HasError());
            REQUIRE(descriptors.front() == original.front());
            descriptors = original;
            REQUIRE(fixture.table.ApplyLayouts(tree, selected, descriptors).HasValue());
            REQUIRE(descriptors.front().style == compact);
            REQUIRE(descriptors.back() == original.back());
        }

        TEST_CASE("Responsive tables accept ordinary same-owner structure updates and reject foreign issuers", "[runtime_ui][responsive]") {
            UiPresentationProfile row{ProfileId<UiPresentationProfileId>(5), 1};
            row.overrides.push_back({ProfileId<UiElementId>(10), {}});
            const std::array rows{row};
            auto fixture = ProfileTable(rows);
            const auto prior = fixture.Select(ProfileEvidence()).Value();
            auto &tree = fixture.generation.Canvas(ProfileId<UiCanvasId>(2))->tree;
            auto commands =
                UiStructuralCommandBuffer::Create(tree.Instance(), tree.Canvas(), tree.SourceDocumentRevision(), tree.Revision(), 1);
            REQUIRE(commands.HasValue());
            REQUIRE(commands.Value().Add(UiInsertElementCommand{ProfileId<UiElementId>(12), tree.Root().Value().handle, 1}).HasValue());
            REQUIRE(tree.CommitDeferred(commands.Value(), UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());
            REQUIRE(fixture.Select(ProfileEvidence()).HasError());
            REQUIRE(fixture.ResolveLayout(prior, ProfileId<UiElementId>(10), {}).HasError());
            auto evidence = ProfileEvidence();
            evidence.tree = tree.Revision();
            evidence.revision = ProfileRevision<UiPresentationEvidenceRevision>(2);
            const auto updated = fixture.Select(evidence);
            REQUIRE(updated.HasValue());
            REQUIRE(fixture.ResolveLayout(updated.Value(), ProfileId<UiElementId>(12), {}).HasValue());
            auto rebuilt = ReloadTests::Generation(fixture.allocator, 1);
            REQUIRE(rebuilt.Canvases().front().tree.Root().Value().handle != tree.Root().Value().handle);
            REQUIRE(fixture.table.Select(rebuilt.Canvases().front().tree, ProfileEvidence()).HasError());
            auto foreign = ProfileTable(rows);
            REQUIRE(foreign.Tree().Root().Value().handle == tree.Root().Value().handle);
            REQUIRE(fixture.table.Select(foreign.Tree(), ProfileEvidence()).HasError());
            REQUIRE(fixture.table.ResolveLayout(foreign.Tree(), prior, ProfileId<UiElementId>(10), {}).HasError());
            tree.Shutdown();
            REQUIRE(fixture.Select(evidence).HasError());
        }

        TEST_CASE("Responsive table admission rejects malformed duplicate foreign and oversized input", "[runtime_ui][responsive]") {
            ProfileTableFixture fixture;
            const auto admit = [&](const std::span<const UiPresentationProfile> rows) {
                return UiPresentationProfileTable::Create(fixture.generation, ProfileId<UiCanvasId>(2),
                                                          ProfileRevision<UiPresentationProfileRevision>(), rows);
            };
            UiPresentationProfile valid{ProfileId<UiPresentationProfileId>(5), 1};
            const std::array duplicate{valid, valid};
            REQUIRE(admit(duplicate).HasError());
            auto invalid = valid;
            invalid.selector.aspect.minimum = UiPresentationAspectRatio{2, 2};
            REQUIRE(admit(std::array{invalid}).HasError());
            invalid = valid;
            invalid.selector.width = {640, 640};
            REQUIRE(admit(std::array{invalid}).HasError());
            invalid = valid;
            invalid.selector.formFactor = static_cast<UiPresentationFormFactor>(99);
            REQUIRE(admit(std::array{invalid}).HasError());
            invalid = valid;
            invalid.selector.required = static_cast<UiPresentationCapabilities>(128);
            REQUIRE(admit(std::array{invalid}).HasError());
            invalid = valid;
            invalid.overrides.push_back({ProfileId<UiElementId>(99), {}});
            REQUIRE(admit(std::array{invalid}).HasError());
            invalid = valid;
            invalid.overrides.resize(MaximumUiPresentationOverrides + 1);
            REQUIRE(admit(std::array{invalid}).HasError());
            const std::vector<UiPresentationProfile> oversized(MaximumUiPresentationProfiles + 1, valid);
            REQUIRE(admit(oversized).HasError());
            auto evidence = ProfileEvidence();
            evidence.content.width = 0;
            REQUIRE(ProfileTable().Select(evidence).HasError());
            evidence = ProfileEvidence();
            evidence.available = static_cast<UiPresentationCapabilities>(128);
            REQUIRE(ProfileTable().Select(evidence).HasError());
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
