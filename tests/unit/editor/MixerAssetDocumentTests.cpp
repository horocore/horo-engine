#include "Horo/Audio/AudioErrors.h"
#include "Horo/Editor/MixerAssetDocument.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Editor {
    namespace {
        template <typename Id> Id Stable(const std::uint64_t value) {
            auto id = Id::Create(value);
            REQUIRE(id.HasValue());
            return std::move(id).Value();
        }

        DocumentIdentity Identity(const std::uint64_t instance = 1) {
            auto source = SourceDocumentId::Parse("audio/main.mixer");
            REQUIRE(source.HasValue());
            return {{DocumentKind::Asset, std::move(source).Value()}, Stable<DocumentInstanceId>(instance)};
        }

        MixerAssetDocument Document() {
            auto opened = MixerAssetDocument::Open(Identity(), Audio::MakeDefaultMixerAsset());
            REQUIRE(opened.HasValue());
            return std::move(opened).Value();
        }

        MixerDocumentSnapshot Capture(const MixerAssetDocument &document) {
            auto captured = document.Snapshot();
            REQUIRE(captured.HasValue());
            return std::move(captured).Value();
        }

        Result<void> Execute(MixerAssetDocument &document, MixerDocumentCommand command) {
            const std::array commands{std::move(command)};
            return document.Execute(document.Revision(), commands);
        }

        EditMixerBus Rename(const std::uint64_t bus, std::string name) {
            return {Stable<Audio::AudioBusId>(bus), std::move(name), Audio::MakeAudioSpeakerLayout(Audio::AudioSpeakerPreset::Stereo), {}};
        }

        Audio::MixerRouteDescriptor Route(const std::uint64_t id, const std::uint64_t source, const std::uint64_t destination,
                                          const Audio::MixerRouteKind kind = Audio::MixerRouteKind::Send) {
            return {Stable<Audio::AudioRouteId>(id), Stable<Audio::AudioBusId>(source), Stable<Audio::AudioBusId>(destination), kind};
        }

        InsertMixerBus Bus(const std::uint64_t id, const std::uint64_t parent = 1) {
            return {{Stable<Audio::AudioBusId>(id),
                     Audio::MixerBusRole::Bus,
                     "New bus",
                     Audio::MakeAudioSpeakerLayout(Audio::AudioSpeakerPreset::Stereo),
                     {},
                     {}},
                    Route(id, id, parent, Audio::MixerRouteKind::Primary)};
        }

        Audio::MixerEffectDescriptor Gain(const std::uint64_t id, const float gain = -3.0F) {
            return {Stable<Audio::AudioEffectId>(id), Audio::MixerEffectKind::Gain, false, Audio::MixerGainEffectParameters{gain}};
        }

        template <typename T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &error) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == error.code.Value());
        }
    }  // namespace

    TEST_CASE("Mixer document saved state follows semantic undo and redo", "[unit][editor][audio][mixer-document]") {
        auto document = Document();
        const auto original = Capture(document);
        CHECK_FALSE(document.IsDirty());
        CHECK_FALSE(document.CanUndo());
        ErrorIs(document.Undo(document.Revision()), MixerDocumentErrors::HistoryUnavailable);
        ErrorIs(document.Redo(document.Revision()), MixerDocumentErrors::HistoryUnavailable);
        REQUIRE(Execute(document, Rename(2, "Localized 音楽")).HasValue());
        const auto renamed = Capture(document);
        CHECK(document.IsDirty());
        CHECK(renamed.Asset().buses[1].id == original.Asset().buses[1].id);
        CHECK(renamed.Asset().routes == original.Asset().routes);
        REQUIRE(document.MarkSaved(renamed).HasValue());
        CHECK_FALSE(document.IsDirty());
        REQUIRE(document.Undo(document.Revision()).HasValue());
        CHECK(Capture(document).Asset() == original.Asset());
        CHECK(Capture(document).State() == original.State());
        CHECK(document.IsDirty());
        REQUIRE(document.Redo(document.Revision()).HasValue());
        CHECK(Capture(document).Asset() == renamed.Asset());
        CHECK(Capture(document).State() == renamed.State());
        CHECK_FALSE(document.IsDirty());
        CHECK(document.Revision() > renamed.Revision());
    }

    TEST_CASE("Mixer document transactions reject invalid graphs without publishing partial state",
              "[unit][editor][audio][mixer-document]") {
        auto document = Document();
        const auto original = Capture(document);
        const std::array<MixerDocumentCommand, 3> cycle{Rename(2, "Staged"), SetMixerRoute{Route(100, 2, 3)},
                                                        SetMixerRoute{Route(101, 3, 2)}};
        ErrorIs(document.Execute(document.Revision(), cycle), Audio::AudioErrors::MixerAssetSchemaInvalid);
        CHECK(Capture(document).Asset() == original.Asset());
        CHECK(document.Revision() == original.Revision());
        CHECK_FALSE(document.CanUndo());
        CHECK_FALSE(document.IsDirty());
        ErrorIs(Execute(document, RemoveMixerRoute{original.Asset().routes.front().id}), Audio::AudioErrors::MixerAssetSchemaInvalid);
        ErrorIs(Execute(document, SetMixerRoute{Route(100, 2, 999)}), Audio::AudioErrors::MixerAssetSchemaInvalid);
        auto invalid = Rename(2, "Nonfinite");
        invalid.defaults.gainDb = std::numeric_limits<float>::infinity();
        ErrorIs(Execute(document, invalid), Audio::AudioErrors::MixerAssetSchemaInvalid);
        CHECK(Capture(document).Asset() == original.Asset());
    }

    TEST_CASE("Mixer bus deletion preserves explicit references and source positions", "[unit][editor][audio][mixer-document]") {
        auto document = Document();
        REQUIRE(Execute(document, Bus(100, 2)).HasValue());
        REQUIRE(Execute(document, SetMixerRoute{Route(101, 3, 100)}).HasValue());
        const auto withReferences = Capture(document);
        ErrorIs(Execute(document, RemoveMixerBus{Stable<Audio::AudioBusId>(2)}), MixerDocumentErrors::InvalidCommand);
        ErrorIs(Execute(document, RemoveMixerBus{Stable<Audio::AudioBusId>(100)}), MixerDocumentErrors::InvalidCommand);
        ErrorIs(Execute(document, RemoveMixerBus{Stable<Audio::AudioBusId>(1)}), MixerDocumentErrors::InvalidCommand);
        CHECK(Capture(document).Asset() == withReferences.Asset());
        const std::array<MixerDocumentCommand, 2> remove{RemoveMixerRoute{Stable<Audio::AudioRouteId>(101)},
                                                         RemoveMixerBus{Stable<Audio::AudioBusId>(100)}};
        REQUIRE(document.Execute(document.Revision(), remove).HasValue());
        REQUIRE(document.Undo(document.Revision()).HasValue());
        CHECK(Capture(document).Asset() == withReferences.Asset());
        REQUIRE(document.Redo(document.Revision()).HasValue());
        CHECK(Capture(document).Asset() == Audio::MakeDefaultMixerAsset());
        REQUIRE(document.Undo(document.Revision()).HasValue());
        const auto restored = Capture(document);
        const auto sourceBus = restored.Asset().buses[3];
        const auto sourceRoute = restored.Asset().routes[2];
        const std::array<MixerDocumentCommand, 2> reorder{RemoveMixerBus{sourceBus.id}, InsertMixerBus{sourceBus, sourceRoute}};
        REQUIRE(document.Execute(document.Revision(), reorder).HasValue());
        CHECK(Capture(document).Asset().buses.back().id.Value() == 4);
        REQUIRE(document.Undo(document.Revision()).HasValue());
        CHECK(Capture(document).Asset() == restored.Asset());
        REQUIRE(document.Redo(document.Revision()).HasValue());
        CHECK(Capture(document).Asset().buses.back().id.Value() == 4);
    }

    TEST_CASE("Mixer effect commands preserve stable chain order and exact undo", "[unit][editor][audio][mixer-document]") {
        auto document = Document();
        const auto bus = Stable<Audio::AudioBusId>(2);
        REQUIRE(Execute(document, InsertMixerEffect{bus, Gain(200), 0}).HasValue());
        REQUIRE(Execute(document, InsertMixerEffect{bus, Gain(201), 1}).HasValue());
        const auto initial = Capture(document);
        REQUIRE(Execute(document, MoveMixerEffect{bus, Stable<Audio::AudioEffectId>(201), 0}).HasValue());
        CHECK(Capture(document).Asset().buses[1].effects.front().id.Value() == 201);
        REQUIRE(Execute(document, MoveMixerEffect{bus, Stable<Audio::AudioEffectId>(201), 1}).HasValue());
        CHECK(Capture(document).Asset() == initial.Asset());
        REQUIRE(Execute(document, EditMixerEffect{bus, Gain(200, -12.0F)}).HasValue());
        REQUIRE(Execute(document, RemoveMixerEffect{bus, Stable<Audio::AudioEffectId>(201)}).HasValue());
        REQUIRE(document.Undo(document.Revision()).HasValue());
        REQUIRE(document.Undo(document.Revision()).HasValue());
        CHECK(Capture(document).Asset() == initial.Asset());
        REQUIRE(document.Redo(document.Revision()).HasValue());
        REQUIRE(document.Redo(document.Revision()).HasValue());
        CHECK(Capture(document).Asset().buses[1].effects.size() == 1);
        CHECK(std::get<Audio::MixerGainEffectParameters>(Capture(document).Asset().buses[1].effects.front().parameters).gainDb == -12.0F);
        ErrorIs(Execute(document, InsertMixerEffect{Stable<Audio::AudioBusId>(3), Gain(200), 0}),
                Audio::AudioErrors::MixerAssetSchemaInvalid);
        auto malformed = Gain(300);
        malformed.kind = Audio::MixerEffectKind::LowPass;
        ErrorIs(Execute(document, InsertMixerEffect{bus, malformed, 1}), Audio::AudioErrors::MixerAssetSchemaInvalid);
    }

    TEST_CASE("Mixer command identity and position failures preserve redo", "[unit][editor][audio][mixer-document]") {
        auto document = Document();
        REQUIRE(Execute(document, Rename(2, "First")).HasValue());
        REQUIRE(document.Undo(document.Revision()).HasValue());
        const auto original = Capture(document);
        const auto bus = Stable<Audio::AudioBusId>(2);
        const auto unknown = Stable<Audio::AudioBusId>(999);
        const auto effect = Stable<Audio::AudioEffectId>(999);
        const std::array<MixerDocumentCommand, 12> invalid{Rename(999, "Missing"),
                                                           RemoveMixerBus{unknown},
                                                           Bus(2),
                                                           RemoveMixerRoute{Stable<Audio::AudioRouteId>(999)},
                                                           InsertMixerEffect{bus, Gain(200), 1},
                                                           InsertMixerEffect{unknown, Gain(200), 0},
                                                           EditMixerEffect{bus, Gain(999)},
                                                           EditMixerEffect{unknown, Gain(999)},
                                                           RemoveMixerEffect{bus, effect},
                                                           RemoveMixerEffect{unknown, effect},
                                                           MoveMixerEffect{bus, effect, 0},
                                                           MoveMixerEffect{unknown, effect, 0}};
        for (const auto &command : invalid) {
            ErrorIs(Execute(document, command), MixerDocumentErrors::InvalidCommand);
            CHECK(Capture(document).Asset() == original.Asset());
            CHECK(document.Revision() == original.Revision());
            CHECK(document.CanRedo());
        }
        REQUIRE(Execute(document, Rename(2, "Music")).HasValue());
        CHECK(document.Revision() == original.Revision());
        CHECK(document.CanRedo());
        REQUIRE(Execute(document, Rename(2, "New branch")).HasValue());
        CHECK_FALSE(document.CanRedo());
    }

    TEST_CASE("Mixer transaction admission bounds commands and active schema limits", "[unit][editor][audio][mixer-document]") {
        auto document = Document();
        ErrorIs(document.Execute(document.Revision(), {}), MixerDocumentErrors::InvalidCommand);
        const std::vector<MixerDocumentCommand> excess(MaximumMixerDocumentCommands + 1, Rename(2, "Too many"));
        ErrorIs(document.Execute(document.Revision(), excess), MixerDocumentErrors::InvalidCommand);
        Audio::MixerAssetSchemaLimits limits;
        limits.maximumBuses = 6;
        limits.maximumRoutes = 5;
        limits.maximumEffects = 1;
        auto opened = MixerAssetDocument::Open(Identity(), Audio::MakeDefaultMixerAsset(), limits);
        REQUIRE(opened.HasValue());
        auto bounded = std::move(opened).Value();
        ErrorIs(Execute(bounded, Bus(100)), MixerDocumentErrors::InvalidCommand);
        ErrorIs(Execute(bounded, SetMixerRoute{Route(100, 2, 1)}), MixerDocumentErrors::InvalidCommand);
        REQUIRE(Execute(bounded, InsertMixerEffect{Stable<Audio::AudioBusId>(2), Gain(200), 0}).HasValue());
        ErrorIs(Execute(bounded, InsertMixerEffect{Stable<Audio::AudioBusId>(2), Gain(201), 1}), MixerDocumentErrors::InvalidCommand);
        ErrorIs(Execute(bounded, InsertMixerEffect{Stable<Audio::AudioBusId>(3), Gain(201), 0}),
                Audio::AudioErrors::MixerAssetSchemaLimitExceeded);
        auto longName = Rename(2, std::string(Audio::MaximumMixerBusDisplayNameBytes + 1, 'x'));
        ErrorIs(Execute(document, longName), MixerDocumentErrors::InvalidCommand);
    }

    TEST_CASE("Mixer bounded history eviction retains exact current source", "[unit][editor][audio][mixer-document]") {
        auto document = Document();
        for (std::size_t index = 0; index < MaximumMixerDocumentHistoryEntries + 3; ++index)
            REQUIRE(Execute(document, Rename(2, std::to_string(index))).HasValue());
        for (std::size_t index = 0; index < MaximumMixerDocumentHistoryEntries; ++index)
            REQUIRE(document.Undo(document.Revision()).HasValue());
        CHECK_FALSE(document.CanUndo());
        CHECK(Capture(document).Asset().buses[1].displayName == "2");
        for (std::size_t index = 0; index < MaximumMixerDocumentHistoryEntries; ++index)
            REQUIRE(document.Redo(document.Revision()).HasValue());
        CHECK(Capture(document).Asset().buses[1].displayName == "66");
    }

    TEST_CASE("Mixer routing replacements validate only the atomic final tree", "[unit][editor][audio][mixer-document]") {
        auto document = Document();
        const auto original = Capture(document);
        const std::array<MixerDocumentCommand, 2> replacement{RemoveMixerRoute{original.Asset().routes.front().id},
                                                              SetMixerRoute{Route(100, 2, 3, Audio::MixerRouteKind::Primary)}};
        REQUIRE(document.Execute(document.Revision(), replacement).HasValue());
        const auto routed = Capture(document);
        CHECK(routed.Asset().routes.back().destination.Value() == 3);
        REQUIRE(Audio::ValidateMixerAssetSchema(routed.Asset()).HasValue());
        REQUIRE(document.Undo(document.Revision()).HasValue());
        CHECK(Capture(document).Asset() == original.Asset());
        REQUIRE(document.Redo(document.Revision()).HasValue());
        CHECK(Capture(document).Asset() == routed.Asset());
        auto changed = routed.Asset().routes.back();
        changed.gainDb = -6.0F;
        REQUIRE(Execute(document, SetMixerRoute{changed}).HasValue());
        CHECK(Capture(document).Asset().routes.back().id == changed.id);
        REQUIRE(document.Undo(document.Revision()).HasValue());
        CHECK(Capture(document).Asset() == routed.Asset());
    }

    TEST_CASE("Mixer history memory pressure evicts steps without losing the current graph", "[unit][editor][audio][mixer-document]") {
        auto source = Audio::MakeDefaultMixerAsset();
        for (std::size_t index = 0; index < Audio::MaximumMixerAssetEffects; ++index)
            source.buses[1].effects.push_back(Gain(static_cast<std::uint64_t>(index + 1)));
        auto opened = MixerAssetDocument::Open(Identity(), source);
        REQUIRE(opened.HasValue());
        auto document = std::move(opened).Value();
        for (std::size_t index = 0; index < MaximumMixerDocumentHistoryEntries + 1; ++index)
            REQUIRE(Execute(document, Rename(2, std::to_string(index))).HasValue());
        const auto current = Capture(document);
        std::size_t steps = 0;
        while (document.CanUndo()) {
            REQUIRE(document.Undo(document.Revision()).HasValue());
            ++steps;
        }
        CHECK(steps < MaximumMixerDocumentHistoryEntries);
        CHECK(steps > 0);
        while (document.CanRedo())
            REQUIRE(document.Redo(document.Revision()).HasValue());
        CHECK(Capture(document).Asset() == current.Asset());
        CHECK(Capture(document).State() == current.State());
    }

    TEST_CASE("Mixer snapshots fence stale commands saves and reload boundaries", "[unit][editor][audio][mixer-document]") {
        auto document = Document();
        const auto original = Capture(document);
        REQUIRE(Execute(document, Rename(2, "Edit")).HasValue());
        const std::array<MixerDocumentCommand, 1> commands{Rename(2, "Stale")};
        ErrorIs(document.Execute(original.Revision(), commands), MixerDocumentErrors::StaleRevision);
        ErrorIs(document.Undo(original.Revision()), MixerDocumentErrors::StaleRevision);
        ErrorIs(document.Redo(original.Revision()), MixerDocumentErrors::StaleRevision);
        REQUIRE(document.MarkSaved(original).HasValue());
        CHECK(document.IsDirty());
        auto other = MixerAssetDocument::Open(Identity(2), Audio::MakeDefaultMixerAsset());
        REQUIRE(other.HasValue());
        ErrorIs(document.MarkSaved(Capture(other.Value())), MixerDocumentErrors::StaleRevision);
        ErrorIs(document.Reload(original.Asset()), MixerDocumentErrors::DirtyDocument);
        const auto edited = Capture(document);
        auto invalid = original.Asset();
        invalid.version = {2, 0};
        ErrorIs(document.Reload(invalid, MixerDocumentDiscardPolicy::DiscardChanges),
                Audio::AudioErrors::MixerAssetSchemaVersionUnsupported);
        CHECK(Capture(document).Asset() == edited.Asset());
        CHECK(document.CanUndo());
        REQUIRE(document.Reload(original.Asset(), MixerDocumentDiscardPolicy::DiscardChanges).HasValue());
        CHECK_FALSE(document.IsDirty());
        CHECK_FALSE(document.CanUndo());
        CHECK_FALSE(document.CanRedo());
        ErrorIs(document.MarkSaved(original), MixerDocumentErrors::StaleRevision);
        ErrorIs(document.MarkSaved(edited), MixerDocumentErrors::StaleRevision);
        CHECK(Capture(document).Asset() == original.Asset());
    }

    TEST_CASE("Mixer source migration is validated detached and remains unsaved", "[unit][editor][audio][mixer-document]") {
        auto legacy = Audio::MakeDefaultMixerAsset();
        legacy.version = {1, 0};
        for (auto &bus : legacy.buses)
            bus.layout = {};
        const auto original = legacy;
        auto opened = MixerAssetDocument::Open(Identity(), legacy);
        REQUIRE(opened.HasValue());
        auto document = std::move(opened).Value();
        CHECK(document.IsDirty());
        CHECK(legacy == original);
        CHECK(Capture(document).Asset().version == Audio::CurrentMixerAssetSchemaVersion);
        REQUIRE(Audio::ValidateMixerAssetSchema(Capture(document).Asset()).HasValue());
        REQUIRE(document.MarkSaved(Capture(document)).HasValue());
        CHECK_FALSE(document.IsDirty());
        REQUIRE(document.Reload(legacy).HasValue());
        CHECK(document.IsDirty());
        auto identity = Identity();
        identity.key.kind = DocumentKind::Scene;
        ErrorIs(MixerAssetDocument::Open(identity, original), MixerDocumentErrors::InvalidCommand);
        ErrorIs(MixerAssetDocument::Open({}, original), MixerDocumentErrors::InvalidCommand);
    }

    TEST_CASE("Mixer closure and move release ownership without resurrecting source captures", "[unit][editor][audio][mixer-document]") {
        auto document = Document();
        REQUIRE(Execute(document, Rename(2, "Dirty")).HasValue());
        const auto saved = Capture(document);
        ErrorIs(document.Close(), MixerDocumentErrors::DirtyDocument);
        ErrorIs(document.Close(static_cast<MixerDocumentDiscardPolicy>(255)), MixerDocumentErrors::InvalidCommand);
        ErrorIs(document.Reload(saved.Asset(), static_cast<MixerDocumentDiscardPolicy>(255)), MixerDocumentErrors::InvalidCommand);
        auto moved = std::move(document);
        CHECK(document.IsClosed());
        CHECK(moved.IsDirty());
        REQUIRE(moved.Close(MixerDocumentDiscardPolicy::DiscardChanges).HasValue());
        REQUIRE(moved.Close().HasValue());
        CHECK(moved.Revision() == 0);
        CHECK_FALSE(moved.IsDirty());
        CHECK_FALSE(moved.CanUndo());
        CHECK_FALSE(moved.CanRedo());
        ErrorIs(moved.Snapshot(), MixerDocumentErrors::Closed);
        ErrorIs(Execute(moved, Rename(2, "Closed")), MixerDocumentErrors::Closed);
        ErrorIs(moved.Undo(0), MixerDocumentErrors::Closed);
        ErrorIs(moved.Redo(0), MixerDocumentErrors::Closed);
        ErrorIs(moved.Reload(saved.Asset()), MixerDocumentErrors::Closed);
        ErrorIs(moved.MarkSaved(saved), MixerDocumentErrors::Closed);
    }
}  // namespace Horo::Editor
