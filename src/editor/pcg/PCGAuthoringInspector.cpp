#include "PCGAuthoringInspector.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/GuiLiveReload.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        /** @brief Full-width numeric text preserves exact integer seeds and round-trip floating values. */
        struct Fields final {
            PCGAuthoringInspectorState &state;
            const ILocalizationService &localization;
            const Theme::Fonts &fonts;
            std::string prefix;

            std::string Text(const char *key) const {
                return localization.Get("editor", std::string("workspace.pcg.authoring.") + key);
            }

            void Label(const char *key) const {
                Ui::FieldLabel(Text(key).c_str(), fonts);
            }

            template <class T> void Number(const std::string &id, T &value) {
                const auto key = prefix + id;
                auto [entry, inserted] = state.text.try_emplace(key);
                if (inserted) {
                    std::array<char, 128> buffer{};
                    const auto result = [&buffer, &value] {
                        if constexpr (std::is_floating_point_v<T>)
                            return std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general,
                                                 std::numeric_limits<T>::max_digits10);
                        else
                            return std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
                    }();
                    entry->second.assign(buffer.data(), result.ptr);
                }
                ImGui::PushID(id.c_str());
                static_cast<void>(Ui::InputTextControl("##value", entry->second, 128, fonts, {.error = state.invalid.contains(key)}));
                T candidate{};
                const auto &text = entry->second;
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), candidate);
                bool valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
                if constexpr (std::is_floating_point_v<T>)
                    valid = valid && std::isfinite(candidate);
                if (valid) {
                    value = candidate;
                    state.invalid.erase(key);
                } else
                    state.invalid.insert(key);
                if (state.invalid.contains(key))
                    Ui::ErrorText(Text("invalid_value").c_str(), fonts);
                ImGui::PopID();
            }

            template <class T> void Field(const char *key, T &value) {
                Label(key);
                if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
                    Number(key, value);
                else if constexpr (std::is_same_v<T, bool>) {
                    ImGui::PushID(key);
                    static_cast<void>(Ui::CheckboxControl("##value", &value, fonts));
                    ImGui::PopID();
                } else if constexpr (std::is_same_v<T, PCG::SpatialElementId>) {
                    auto numeric = value.Value();
                    Number(key, numeric);
                    const auto id = GuiLive::HostSymbol<&PCG::SpatialElementId::Create>()(numeric);
                    if (id.HasValue()) {
                        value = id.Value();
                        state.invalid.erase(prefix + key + ".identity");
                    } else {
                        state.invalid.insert(prefix + key + ".identity");
                        Ui::ErrorText(Text("invalid_value").c_str(), fonts);
                    }
                } else if constexpr (std::is_same_v<T, PCG::PCGAttributeKey>) {
                    const auto id = prefix + key;
                    auto [entry, inserted] = state.text.try_emplace(id, value.Value());
                    ImGui::PushID(key);
                    if (Ui::InputTextControl("##key", entry->second, PCG::MaximumAttributeKeyBytes + 1, fonts,
                                             {.error = state.invalid.contains(id)})) {
                        const auto parsed = GuiLive::HostSymbol<&PCG::PCGAttributeKey::Create>()(entry->second);
                        if (parsed.HasValue()) {
                            value = parsed.Value();
                            state.invalid.erase(id);
                        } else
                            state.invalid.insert(id);
                    }
                    if (state.invalid.contains(id))
                        Ui::ErrorText(Text("invalid_value").c_str(), fonts);
                    ImGui::PopID();
                } else if constexpr (std::is_same_v<T, Math::Vec2> || std::is_same_v<T, Math::Vec3> || std::is_same_v<T, Math::Vec4> ||
                                     std::is_same_v<T, Math::Quaternion>) {
                    Number(std::string(key) + ".x", value.x);
                    Number(std::string(key) + ".y", value.y);
                    if constexpr (!std::is_same_v<T, Math::Vec2>)
                        Number(std::string(key) + ".z", value.z);
                    if constexpr (std::is_same_v<T, Math::Vec4> || std::is_same_v<T, Math::Quaternion>)
                        Number(std::string(key) + ".w", value.w);
                }
            }

            template <class T, std::size_t N> void Choice(const char *key, T &value, const std::array<const char *, N> &keys) const {
                Label(key);
                std::array<std::string, N + 1> labels;
                std::array<const char *, N + 1> items{};
                for (std::size_t i = 0; i < N; ++i) {
                    labels[i] = Text(keys[i]);
                    items[i] = labels[i].c_str();
                }
                labels[N] = Text("unsupported");
                items[N] = labels[N].c_str();
                auto selected = static_cast<int>(value);
                if (selected < 0 || selected >= static_cast<int>(N))
                    selected = static_cast<int>(N);
                const Ui::ComboItemSource source{.label = [&items](int index) {
                    return items[static_cast<std::size_t>(index)];
                }, .enabled = [](int index) {
                    return index < static_cast<int>(N);
                }};
                const std::string id = std::string("##") + key;
                if (Ui::ComboControl(id.c_str(), &selected, static_cast<int>(N + 1), source, fonts))
                    value = static_cast<T>(selected);
            }

            void Space(PCG::PCGPointCoordinateSpace &value) const {
                Choice("space", value, std::array{"snapshot", "point_local"});
            }

            template <class V> void Value(const char *key, V &value) {
                std::visit([this, key]<class T>(T &typed) {
                    if constexpr (!std::is_same_v<T, std::monostate>)
                        Field(key, typed);
                    else
                        Ui::ErrorText(Text("unsupported").c_str(), fonts);
                }, value);
            }

            void Predicate(PCG::PCGAttributePredicateValue &value) {
                auto type = static_cast<int>(value.index());
                Choice("predicate_type", type, std::array{"boolean", "signed", "unsigned", "scalar", "vector2", "vector3", "vector4"});
                if (type != static_cast<int>(value.index())) {
                    switch (type) {
                        case 0:
                            value = false;
                            break;
                        case 1:
                            value = std::int64_t{};
                            break;
                        case 2:
                            value = std::uint64_t{};
                            break;
                        case 3:
                            value = double{};
                            break;
                        case 4:
                            value = Math::Vec2{};
                            break;
                        case 5:
                            value = Math::Vec3{};
                            break;
                        case 6:
                            value = Math::Vec4{};
                            break;
                        default:
                            break;
                    }
                    std::erase_if(state.text, [this](const auto &entry) {
                        return entry.first.starts_with(prefix + "predicate");
                    });
                    std::erase_if(state.invalid, [this](const auto &entry) {
                        return entry.starts_with(prefix + "predicate");
                    });
                }
                Value("predicate", value);
            }

            /** @brief Draws the exact monostate schema through shared fields. */
            void SettingsValue(std::monostate &) const {
                Ui::Hint(Text("no_properties").c_str(), fonts);
            }

            /** @brief Draws the exact PCGGridGenerationSettings schema through shared fields. */
            void SettingsValue(PCG::PCGGridGenerationSettings &value) {
                Field("grid", value.grid);
            }

            /** @brief Draws the exact PCGJitterGenerationSettings schema through shared fields. */
            void SettingsValue(PCG::PCGJitterGenerationSettings &value) {
                Field("grid", value.grid);
                Field("fraction", value.fraction);
            }

            /** @brief Draws the exact PCGStratifiedGenerationSettings schema through shared fields. */
            void SettingsValue(PCG::PCGStratifiedGenerationSettings &value) {
                Field("volume", value.volume);
                Label("strata");
                for (std::size_t axis = 0; axis < 3; ++axis)
                    Number(std::format("strata.{}", axis), value.strata[axis]);
            }

            /** @brief Draws the exact PCGPoissonGenerationSettings schema through shared fields. */
            void SettingsValue(PCG::PCGPoissonGenerationSettings &value) {
                Field("volume", value.volume);
                Field("count", value.count);
                Field("attempts", value.attempts);
                Field("minimum_distance", value.minimumDistance);
            }

            /** @brief Draws the exact PCGDensityRangeFilterSettings schema through shared fields. */
            void SettingsValue(PCG::PCGDensityRangeFilterSettings &value) {
                Field("minimum", value.minimum);
                Field("maximum", value.maximum);
            }

            /** @brief Draws the exact PCGBoundsFilterSettings schema through shared fields. */
            void SettingsValue(PCG::PCGBoundsFilterSettings &value) {
                Field("minimum", value.bounds.minimum);
                Field("maximum", value.bounds.maximum);
            }

            /** @brief Draws the exact PCGSlopeFilterSettings schema through shared fields. */
            void SettingsValue(PCG::PCGSlopeFilterSettings &value) {
                Field("minimum_degrees", value.minimumDegrees);
                Field("maximum_degrees", value.maximumDegrees);
            }

            /** @brief Draws the exact PCGDistanceFilterSettings schema through shared fields. */
            void SettingsValue(PCG::PCGDistanceFilterSettings &value) {
                Field("minimum_distance", value.minimumDistance);
            }

            /** @brief Draws the exact PCGAttributeFilterSettings schema through shared fields. */
            void SettingsValue(PCG::PCGAttributeFilterSettings &value) {
                Field("attribute", value.key);
                Choice("comparison", value.comparison, std::array{"equal", "not_equal", "less", "less_equal", "greater", "greater_equal"});
                Predicate(value.value);
            }

            /** @brief Draws the exact PCGPointTransformSettings schema through shared fields. */
            void SettingsValue(PCG::PCGPointTransformSettings &value) {
                Field("translation", value.delta.translation);
                Field("rotation", value.delta.rotation);
                Field("scale", value.delta.scale);
                Space(value.space);
            }

            /** @brief Draws the exact PCGPointAlignmentSettings schema through shared fields. */
            void SettingsValue(PCG::PCGPointAlignmentSettings &value) {
                Field("maximum_vertical_distance", value.maximumVerticalDistance);
            }

            /** @brief Draws the exact PCGPointRandomOffsetSettings schema through shared fields. */
            void SettingsValue(PCG::PCGPointRandomOffsetSettings &value) {
                Field("half_extent", value.halfExtent);
                Space(value.space);
                Field("salt", value.salt);
            }

            /** @brief Draws the exact PCGPointNoiseSettings schema through shared fields. */
            void SettingsValue(PCG::PCGPointNoiseSettings &value) {
                Field("attribute", value.attribute);
                Choice("attribute_type", value.type,
                       std::array{"boolean", "signed", "unsigned", "scalar", "vector2", "vector3", "vector4"});
                Field("frequency", value.frequency);
                Field("amplitude", value.amplitude);
                Field("octaves", value.octaves);
                Field("persistence", value.persistence);
                Field("lacunarity", value.lacunarity);
                Choice("normalization", value.normalization, std::array{"signed_noise", "unit_interval"});
                Field("salt", value.salt);
            }

            /** @brief Dispatches only the catalog-decoded typed alternative, without interpreting source bytes. */
            void Settings(PCG::PCGNodeAuthoringSettings &settings) {
                std::visit([this](auto &value) {
                    SettingsValue(value);
                }, settings);
            }
        };

        /** @brief Draws graph policy without granting any execution capability. */
        void DrawPolicy(Fields &fields, PCGGraphAuthoringDraft &draft, const PCG::PCGNodeCatalogSnapshot &catalog) {
            const auto &fonts = fields.fonts;
            Ui::SectionTitle(fields.Text("title").c_str(), fonts);
            fields.Label("tier");
            const auto tier = static_cast<std::size_t>(draft.values.tier);
            constexpr std::array tiers{"baseline", "standard", "high"};
            Ui::Hint(fields.Text(tier < tiers.size() ? tiers[tier] : "unsupported").c_str(), fonts);
            fields.Label("mode");
            constexpr std::array modes{"offline", "runtime", "hybrid"};
            static constexpr std::array capabilities{PCG::PCGCapability::OfflineBake, PCG::PCGCapability::RuntimeEvaluation,
                                                     PCG::PCGCapability::HybridEvaluation};
            std::array<std::string, 3> modeLabels;
            for (std::size_t i = 0; i < modes.size(); ++i)
                modeLabels[i] = fields.Text(modes[i]);
            auto mode = static_cast<int>(draft.values.mode);
            const auto unsupported = fields.Text("unsupported");
            if (const Ui::ComboItemSource source{.label =
                                                     [&modeLabels](int i) {
                return modeLabels[static_cast<std::size_t>(i)].c_str();
            },
                                                 .enabled =
                                                     [&catalog](int i) {
                return catalog.Capabilities().granted.Contains(capabilities[static_cast<std::size_t>(i)]);
            },
                                                 .disabledTooltip =
                                                     [&unsupported](int) {
                return unsupported.c_str();
            }};
                Ui::ComboControl("##mode", &mode, 3, source, fonts))
                draft.values.mode = static_cast<PCG::PCGGenerationMode>(mode);
            if (mode >= 0 && mode < 3 && !catalog.Capabilities().granted.Contains(capabilities[static_cast<std::size_t>(mode)]))
                Ui::ErrorText(fields.Text("unsupported").c_str(), fonts);
            fields.Field("seed", draft.values.deterministicSeed);
            Ui::Hint(fields.Text("deterministic_defaults").c_str(), fonts);
        }

        /** @brief Shows externally overrideable authored defaults with stable input identities. */
        void DrawInputs(Fields &fields, PCGGraphAuthoringDraft &draft) {
            const auto &fonts = fields.fonts;
            std::optional<PCG::ExposedInputId> remove;
            for (auto &input : draft.values.exposedInputs) {
                fields.prefix = std::format("input.{}.", input.id.Value());
                ImGui::PushID(fields.prefix.c_str());
                Ui::LabeledSeparator(input.key.c_str(), fonts);
                fields.Label("input_key");
                static_cast<void>(
                    Ui::InputTextControl("##input_key", input.key, PCG::PCGGraphSourceHardLimits::IdentifierBytes + 1, fonts));
                fields.Value("default", input.defaultValue);
                if (Ui::Button(
                        {.label = fields.Text("remove_input").c_str(), .variant = Ui::ButtonVariant::Secondary, .font = fonts.sans})) {
                    remove = input.id;
                    std::erase_if(fields.state.text, [&fields](const auto &entry) {
                        return entry.first.starts_with(fields.prefix);
                    });
                    std::erase_if(fields.state.invalid, [&fields](const auto &entry) {
                        return entry.starts_with(fields.prefix);
                    });
                }
                Ui::Hint(fields.Text("external_input").c_str(), fonts);
                ImGui::PopID();
            }
            if (remove)
                std::erase_if(draft.values.exposedInputs, [&remove](const auto &input) {
                    return input.id == *remove;
                });
        }

        /** @brief Exact deterministic zero of one pin type, used only by explicit add-default/expose actions. */
        PCG::PCGGraphValue ZeroFor(PCG::PCGPinType type) {
            using enum PCG::PCGPinType;
            switch (type) {
                case Boolean:
                    return false;
                case SignedInteger:
                    return std::int64_t{};
                case UnsignedInteger:
                    return std::uint64_t{};
                case Scalar:
                    return double{};
                case Vector2:
                    return Math::Vec2{};
                case Vector3:
                    return Math::Vec3{};
                case Vector4:
                    return Math::Vec4{};
                default:
                    return std::monostate{};
            }
        }

        /** @brief Exposes one typed source pin in the same pending gesture, bounded before allocation. */
        void DrawInputBinding(Fields &fields, PCGGraphAuthoringDraft &draft, PCG::NodeId node, const PCG::PCGGraphPin &pin) {
            if (const bool bound = std::ranges::any_of(draft.values.exposedInputs,
                                                       [&pin](const auto &input) {
                return input.pin == pin.id;
            });
                !Ui::Button({.label = fields.Text("expose_input").c_str(), .enabled = !bound, .font = fields.fonts.sans}))
                return;
            if (const auto limits = PCG::GraphSourceLimitsForTier(draft.values.tier);
                limits.HasError() || draft.values.exposedInputs.size() >= limits.Value().maximumExposedInputs) {
                fields.state.error = MakeError(PCGDocumentError(PCGDocumentCommandFailureReason::Limit));
                return;
            }
            std::uint64_t maximum{};
            for (const auto &input : draft.baseline.source->Data().exposedInputs)
                maximum = std::max(maximum, input.id.Value());
            for (const auto &input : draft.values.exposedInputs)
                maximum = std::max(maximum, input.id.Value());
            if (maximum == std::numeric_limits<std::uint64_t>::max()) {
                fields.state.error = MakeError(PCGDocumentError(PCGDocumentCommandFailureReason::Exhausted));
                return;
            }
            const auto id = GuiLive::HostSymbol<&PCG::ExposedInputId::Create>()(maximum + 1).Value();
            draft.values.exposedInputs.emplace_back(id, std::format("graph.input_{}", id.Value()), node, pin.id,
                                                    pin.defaultValue.value_or(ZeroFor(pin.type)));
        }

        /** @brief Adds/removes a typed pin default only as pending gesture intent. */
        void DrawPinDefault(Fields &fields, PCG::PCGGraphPin &pin) {
            const auto &fonts = fields.fonts;
            if (pin.defaultValue) {
                fields.Value("pin_default", *pin.defaultValue);
                if (Ui::Button(
                        {.label = fields.Text("remove_default").c_str(), .variant = Ui::ButtonVariant::Secondary, .font = fonts.sans})) {
                    pin.defaultValue.reset();
                    std::erase_if(fields.state.text, [&fields](const auto &entry) {
                        return entry.first.starts_with(fields.prefix);
                    });
                    std::erase_if(fields.state.invalid, [&fields](const auto &entry) {
                        return entry.starts_with(fields.prefix);
                    });
                }
            } else if (Ui::Button({.label = fields.Text("add_default").c_str(), .font = fonts.sans})) {
                pin.defaultValue = ZeroFor(pin.type);
            }
        }

        /** @brief Projects one exact catalog schema and its typed input defaults. */
        void DrawNodeProperties(Fields &fields, PCGGraphAuthoringDraft &draft, std::size_t index,
                                const PCG::PCGNodeCatalogSnapshot &catalog) {
            const auto &fonts = fields.fonts;
            auto &node = draft.values.nodes[index];
            fields.prefix = std::format("node.{}.", node.id.Value());
            ImGui::PushID(fields.prefix.c_str());
            if (const auto descriptor = catalog.Find(node.type);
                descriptor.HasValue() && descriptor.Value()->determinism != PCG::PCGNodeDeterminism::PortableDeterministic)
                Ui::Hint(fields
                             .Text(descriptor.Value()->determinism == PCG::PCGNodeDeterminism::BestEffortPreview ? "non_deterministic"
                                                                                                                 : "profile_deterministic")
                             .c_str(),
                         fonts);
            auto &property = draft.properties[index];
            if (property.settings)
                fields.Settings(*property.settings);
            else {
                Ui::ErrorText(fields.Text("unsupported").c_str(), fonts);
                if (property.diagnostic)
                    Ui::Hint(property.diagnostic->code.Value().c_str(), fonts);
            }
            for (auto &pin : node.pins) {
                if (pin.direction != PCG::PCGPinDirection::Input || pin.type == PCG::PCGPinType::PointSet || !property.settings)
                    continue;
                const auto prefix = fields.prefix;
                fields.prefix += std::format("pin.{}.", pin.id.Value());
                ImGui::PushID(fields.prefix.c_str());
                DrawPinDefault(fields, pin);
                DrawInputBinding(fields, draft, node.id, pin);
                ImGui::PopID();
                fields.prefix = prefix;
            }
            ImGui::PopID();
        }
    }  // namespace

    /** @copydoc DrawPCGAuthoringInspector */
    std::optional<PCGDocumentCommand> DrawPCGAuthoringInspector(PCGAuthoringInspectorState &state, const PCGDocumentSnapshot &current,
                                                                const PCG::PCGNodeCatalogSnapshot &catalog,
                                                                std::span<const PCG::NodeId> selection, bool canEdit,
                                                                const ILocalizationService &localization, const Theme::Fonts &fonts) {
        if (!state.draft || !state.draft->Matches(current, catalog)) {
            state = {};
            auto capture = PCGGraphAuthoringDraft::Capture(current, catalog);
            if (capture.HasError()) {
                state.error = capture.ErrorValue();
            } else
                state.draft = std::move(capture).Value();
        }
        Fields fields{state, localization, fonts, "graph."};
        if (!state.draft) {
            Ui::ErrorText(fields.Text("unavailable").c_str(), fonts);
            return {};
        }
        auto &draft = *state.draft;
        ImGui::BeginDisabled(!canEdit);
        DrawPolicy(fields, draft, catalog);
        DrawInputs(fields, draft);
        for (std::size_t index = 0; index < draft.values.nodes.size(); ++index)
            if (std::ranges::find(selection, draft.values.nodes[index].id) != selection.end())
                DrawNodeProperties(fields, draft, index, catalog);
        if (!state.invalid.empty())
            Ui::ErrorText(fields.Text("invalid_value").c_str(), fonts);
        if (state.error) {
            Ui::ErrorText(fields.Text("rejected").c_str(), fonts);
            Ui::Hint(state.error->code.Value().c_str(), fonts);
        }
        if (Ui::Button({.label = fields.Text("apply").c_str(), .enabled = state.invalid.empty(), .font = fonts.sans})) {
            auto command = draft.Command(current, catalog, canEdit);
            if (command.HasValue()) {
                ImGui::EndDisabled();
                return std::move(command).Value();
            }
            state.error = command.ErrorValue();
        }
        ImGui::EndDisabled();
        if (Ui::Button({.label = fields.Text("cancel").c_str(), .variant = Ui::ButtonVariant::Secondary, .font = fonts.sans}))
            state = {};
        return {};
    }
}  // namespace Horo::Editor
