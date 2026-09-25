#include "editor/modals/build/BuildWorkflowPreviewModal.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/design_system/components/BuildPreviewProgress.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <imgui.h>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Editor {
    namespace {
        constexpr std::array<const char *, 4> kTitles{"build.preview.build.title", "build.preview.tests.title",
                                                      "build.preview.release.title", "build.preview.publish.title"};
        constexpr std::array<const char *, 4> kProfiles{"build.preview.profile.game", "build.preview.profile.server",
                                                        "build.preview.profile.editor", "build.preview.profile.diagnostics"};
        constexpr std::array<const char *, 6> kReleaseProfiles{"build.preview.profile.game",
                                                               "build.preview.release.profile.server",
                                                               "build.preview.profile.editor",
                                                               "build.preview.release.profile.cli",
                                                               "build.preview.release.profile.sdk",
                                                               "build.preview.release.profile.diagnostics"};
        constexpr std::array<const char *, 3> kPlatforms{"build.preview.platform.windows", "build.preview.platform.linux",
                                                         "build.preview.platform.macos"};
        constexpr std::array<const char *, 2> kArchitectures{"build.preview.arch.x64", "build.preview.arch.arm64"};
        constexpr std::array<const char *, 3> kConfigurations{"build.preview.config.development", "build.preview.config.profile",
                                                              "build.preview.config.shipping"};
        constexpr std::array<const char *, 3> kReleaseConfigurations{"build.preview.config.development",
                                                                     "build.preview.release.config.test", "build.preview.config.shipping"};
        constexpr std::array<const char *, 3> kModes{"build.preview.mode.playable", "build.preview.mode.compile", "build.preview.mode.qa"};
        constexpr std::array<const char *, 3> kToolchains{"build.preview.toolchain.default", "build.preview.toolchain.clang",
                                                          "build.preview.toolchain.msvc"};
        constexpr std::array<const char *, 3> kReleaseToolchains{"build.preview.toolchain.default", "build.preview.toolchain.msvc",
                                                                 "build.preview.toolchain.clang"};
        constexpr std::array<const char *, 3> kContent{"build.preview.content.all", "build.preview.content.scenes",
                                                       "build.preview.content.selected"};
        constexpr std::array<const char *, 3> kReleaseContent{"build.preview.content.all", "build.preview.release.content.scenes",
                                                              "build.preview.release.content.chunks"};
        constexpr std::array<const char *, 2> kSources{"build.preview.source.project", "build.preview.source.package"};
        constexpr std::array<const char *, 3> kTestProfiles{"build.preview.test.fast", "build.preview.test.smoke",
                                                            "build.preview.test.full"};
        constexpr std::array<const char *, 3> kReleasePackages{"build.preview.package.archive", "build.preview.release.package.installer",
                                                               "build.preview.release.package.store"};
        constexpr std::array<const char *, 3> kReleaseTestProfiles{"build.preview.release.test.fast", "build.preview.release.test.smoke",
                                                                   "build.preview.release.test.full"};
        constexpr std::array<const char *, 2> kReleaseSigning{"build.preview.release.signing.production",
                                                              "build.preview.release.signing.test"};
        constexpr std::array<const char *, 2> kProtection{"build.preview.protection.default", "build.preview.protection.secure"};
        constexpr std::array<const char *, 3> kChannels{"build.preview.channel.internal", "build.preview.channel.beta",
                                                        "build.preview.channel.public"};
        constexpr std::array<const char *, 2> kVisibility{"build.preview.visibility.private", "build.preview.visibility.public"};
        constexpr std::array<const char *, 2> kCredentials{"build.preview.credentials.project", "build.preview.credentials.ci"};
        constexpr std::array<const char *, 4> kDestinations{"build.preview.destination.cdn", "build.preview.destination.steam",
                                                            "build.preview.destination.itch", "build.preview.destination.custom"};

        [[nodiscard]] bool IsPositiveInteger(const std::string &value) {
            int parsed = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            return result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed > 0;
        }

        [[nodiscard]] float ActionWidth(const std::string_view label) {
            return std::max(68.0F, ImGui::CalcTextSize(label.data(), label.data() + label.size()).x + 28.0F);
        }

        void AddFormGap(const float height) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Ui::ScaledLayoutValue(height));
        }

        template <typename Left, typename Right> void DrawPair(const char *id, Left &&left, Right &&right) {
            if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame))
                return;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const float rowTop = ImGui::GetCursorPosY();
            left();
            ImGui::TableSetColumnIndex(1);
            ImGui::SetCursorPosY(rowTop);
            right();
            ImGui::EndTable();
        }

        [[nodiscard]] constexpr int KindIndex(const BuildPreviewKind kind) noexcept {
            return static_cast<int>(kind);
        }

        [[nodiscard]] const char *CompletionNoticeKey(const BuildPreviewKind kind) noexcept {
            constexpr std::array<const char *, 4> keys{"build.preview.notice.completed.build", "build.preview.notice.completed.tests",
                                                       "build.preview.notice.completed.release", "build.preview.notice.completed.publish"};
            return keys[static_cast<std::size_t>(KindIndex(kind))];
        }

        [[nodiscard]] int HostPlatform() noexcept {
#if defined(_WIN32)
            return 0;
#elif defined(__APPLE__)
            return 2;
#else
            return 1;
#endif
        }
    }  // namespace

    BuildWorkflowPreviewModal::BuildWorkflowPreviewModal(const EditorGuiContext &context, BuildWorkflowPreviewState &state,
                                                         const BuildPreviewKind kind)
        : context_(context), state_(state), kind_(kind), platform_(kind == BuildPreviewKind::Release ? 0 : HostPlatform()) {
        if (kind_ == BuildPreviewKind::Build)
            output_ = "builds/local";
        if (kind_ == BuildPreviewKind::Release) {
            configuration_ = 2;
            testProfile_ = 1;
            output_ = "releases";
        }
    }

    ModalId BuildWorkflowPreviewModal::Id() const {
        return ModalId{0x4255494C44505200ULL + static_cast<std::uint64_t>(kind_)};
    }

    ModalPresentation BuildWorkflowPreviewModal::Presentation() const {
        return {.size = ModalSizePolicy::Large, .dimWorkspace = true};
    }

    ModalClosePolicy BuildWorkflowPreviewModal::ClosePolicy() const {
        return {.allowCloseButton = true, .allowEscape = true, .allowOutsideClick = false, .allowApplicationShutdown = true};
    }

    Result<void> BuildWorkflowPreviewModal::OnOpen(EditorModalContext &) {
        if (state_.Job() && state_.Job()->request.kind == kind_)
            page_ = Page::Activity;
        return Result<void>::Success();
    }

    CloseDecision BuildWorkflowPreviewModal::CanClose(ModalCloseReason) {
        return CloseDecision::Allow;
    }

    std::string BuildWorkflowPreviewModal::Label(const char *key) const {
        return context_.localization.Get("editor", key);
    }

    void BuildWorkflowPreviewModal::DrawTextField(const char *labelKey, const char *id, std::string &value, const char *hintKey) const {
        Ui::BuildPreviewFieldLabel(Label(labelKey).c_str(), context_.theme.fonts);
        const std::string hint = hintKey != nullptr ? Label(hintKey) : std::string{};
        static_cast<void>(Ui::InputTextControl(id, value, 512, context_.theme.fonts,
                                               {.height = 36.0F,
                                                .hint = hintKey != nullptr ? hint.c_str() : nullptr,
                                                .componentSize = Ui::ComponentSize::Medium}));
    }

    void BuildWorkflowPreviewModal::DrawTextPair(const char *tableId, const char *leftLabelKey, const char *leftId, std::string &leftValue,
                                                 const char *rightLabelKey, const char *rightId, std::string &rightValue) const {
        if (!ImGui::BeginTable(tableId, 2, ImGuiTableFlags_SizingStretchSame))
            return;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        Ui::BuildPreviewFieldLabel(Label(leftLabelKey).c_str(), context_.theme.fonts);
        ImGui::TableSetColumnIndex(1);
        Ui::BuildPreviewFieldLabel(Label(rightLabelKey).c_str(), context_.theme.fonts);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        static_cast<void>(Ui::InputTextControl(leftId, leftValue, 512, context_.theme.fonts,
                                               {.height = 36.0F, .componentSize = Ui::ComponentSize::Medium}));
        ImGui::TableSetColumnIndex(1);
        static_cast<void>(Ui::InputTextControl(rightId, rightValue, 512, context_.theme.fonts,
                                               {.height = 36.0F, .componentSize = Ui::ComponentSize::Medium}));
        ImGui::EndTable();
    }

    void BuildWorkflowPreviewModal::DrawComboField(const char *labelKey, const char *id, int &value, const char *const *keys,
                                                   const int count) const {
        Ui::BuildPreviewFieldLabel(Label(labelKey).c_str(), context_.theme.fonts);
        std::vector<std::string> labels;
        std::vector<const char *> pointers;
        labels.reserve(static_cast<std::size_t>(count));
        pointers.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index)
            labels.push_back(Label(keys[index]));
        for (const auto &label : labels)
            pointers.push_back(label.c_str());
        ImGui::PushItemWidth(-1.0F);
        static_cast<void>(Ui::ComboControl(id, &value, pointers.data(), count, context_.theme.fonts,
                                           {.height = 36.0F, .componentSize = Ui::ComponentSize::Medium}));
        ImGui::PopItemWidth();
    }

    void BuildWorkflowPreviewModal::DrawBuildForm() {
        constexpr float fieldRowGap = 12.0F;
        Ui::BuildPreviewFieldLabel(Label("build.preview.build_profile").c_str(), context_.theme.fonts);
        const std::string localProfile = Label("build.preview.build_profile.local");
        const std::string qaProfile = Label("build.preview.build_profile.qa");
        const std::array profileNames{localProfile.c_str(), qaProfile.c_str(), savedProfileName_.c_str()};
        const std::string manageLabel = Label("build.preview.manage_profiles");
        const float manageWidth = ImGui::CalcTextSize(manageLabel.c_str()).x + 30.0F;
        ImGui::PushItemWidth(std::max(160.0F, ImGui::GetContentRegionAvail().x - manageWidth - 12.0F));
        if (Ui::ComboControl("##savedBuildProfile", &buildProfile_, profileNames.data(), hasSavedProfile_ ? 3 : 2, context_.theme.fonts,
                             {.height = 36.0F, .componentSize = Ui::ComponentSize::Medium})) {
            if (buildProfile_ == 0) {
                profile_ = 0;
                mode_ = 0;
                configuration_ = 0;
                testAfterBuild_ = false;
                output_ = "builds/local";
            } else if (buildProfile_ == 1) {
                profile_ = 0;
                mode_ = 2;
                configuration_ = 1;
                testAfterBuild_ = true;
                output_ = "builds/qa";
            }
            runAfterBuild_ = false;
        }
        ImGui::PopItemWidth();
        ImGui::SameLine(0.0F, 12.0F);
        if (Ui::Button({.label = manageLabel.c_str(),
                        .size = {manageWidth, 36.0F},
                        .variant = Ui::ButtonVariant::Secondary,
                        .font = context_.theme.fonts.sans}))
            showProfiles_ = !showProfiles_;
        if (showProfiles_) {
            Ui::Hint(Label("build.preview.profiles_hint").c_str(), context_.theme.fonts);
            DrawTextField("build.preview.profile_name", "##newBuildProfile", profileName_);
            if (Ui::Button({.label = Label("build.preview.save_profile").c_str(),
                            .variant = Ui::ButtonVariant::Secondary,
                            .enabled = !profileName_.empty(),
                            .font = context_.theme.fonts.sans})) {
                savedProfileName_ = profileName_;
                profileName_.clear();
                hasSavedProfile_ = true;
                buildProfile_ = 2;
            }
        }
        AddFormGap(fieldRowGap);
        DrawPair("##buildProductMode", [this] {
            DrawComboField("build.preview.profile", "##buildProduct", profile_, kProfiles.data(), static_cast<int>(kProfiles.size()));
        }, [this] {
            DrawComboField("build.preview.workflow", "##buildMode", mode_, kModes.data(), static_cast<int>(kModes.size()));
        });
        AddFormGap(fieldRowGap);
        DrawPair("##buildTarget", [this] {
            DrawComboField("build.preview.platform", "##buildPlatform", platform_, kPlatforms.data(), static_cast<int>(kPlatforms.size()));
        }, [this] {
            DrawComboField("build.preview.architecture", "##buildArch", architecture_, kArchitectures.data(),
                           static_cast<int>(kArchitectures.size()));
        });
        AddFormGap(fieldRowGap);
        DrawComboField("build.preview.configuration", "##buildConfig", configuration_, kConfigurations.data(),
                       static_cast<int>(kConfigurations.size()));
        AddFormGap(fieldRowGap);
        if (mode_ != 1)
            DrawComboField("build.preview.content", "##buildContent", content_, kContent.data(), static_cast<int>(kContent.size()));
        AddFormGap(fieldRowGap);
        static_cast<void>(Ui::CheckboxControl(Label("build.preview.test_after").c_str(), &testAfterBuild_, context_.theme.fonts, 0.0F,
                                              Ui::CheckboxTextTone::Primary));
        if (testAfterBuild_) {
            AddFormGap(fieldRowGap);
            DrawComboField("build.preview.test_profile", "##buildTestProfile", testProfile_, kTestProfiles.data(),
                           static_cast<int>(kTestProfiles.size()));
        }
        const bool canRun = mode_ == 0 && profile_ == 0 && platform_ == HostPlatform();
        if (!canRun)
            runAfterBuild_ = false;
        AddFormGap(fieldRowGap);
        ImGui::BeginDisabled(!canRun);
        static_cast<void>(Ui::CheckboxControl(Label("build.preview.run_after").c_str(), &runAfterBuild_, context_.theme.fonts, 0.0F,
                                              Ui::CheckboxTextTone::Primary));
        ImGui::EndDisabled();
        if (!canRun)
            Ui::Hint(Label("build.preview.run_unavailable").c_str(), context_.theme.fonts);
        AddFormGap(16.0F);
        if (Ui::TextLink("##advancedBuild", Label("build.preview.advanced").c_str(), context_.theme.fonts.sans, Theme::TextPx::Body()))
            showAdvanced_ = !showAdvanced_;
        if (showAdvanced_) {
            DrawPair("##buildAdvanced", [this] {
                DrawComboField("build.preview.toolchain", "##buildToolchain", toolchain_, kToolchains.data(),
                               static_cast<int>(kToolchains.size()));
            }, [this] {
                DrawTextField("build.preview.parallel_jobs", "##buildParallel", parallelJobs_);
            });
            DrawTextField("build.preview.output", "##buildOutput", output_);
        }
        Ui::Hint(Label("build.preview.mock_notice").c_str(), context_.theme.fonts);
    }

    void BuildWorkflowPreviewModal::DrawTestForm() {
        constexpr float fieldRowGap = 12.0F;
        DrawPair("##testSelection", [this] {
            DrawComboField("build.preview.test_source", "##testSource", testSource_, kSources.data(), static_cast<int>(kSources.size()));
        }, [this] {
            DrawComboField("build.preview.test_profile", "##testProfile", testProfile_, kTestProfiles.data(),
                           static_cast<int>(kTestProfiles.size()));
        });
        if (testSource_ == 1) {
            AddFormGap(fieldRowGap);
            DrawTextField("build.preview.package_path", "##testPackagePath", packagePath_);
        }
        AddFormGap(fieldRowGap);
        DrawTextField("build.preview.test_filter", "##testFilter", testFilter_, "build.preview.all_tests");
        AddFormGap(fieldRowGap);
        DrawPair("##testTarget", [this] {
            DrawComboField("build.preview.platform", "##testPlatform", platform_, kPlatforms.data(), static_cast<int>(kPlatforms.size()));
        }, [this] {
            DrawTextField("build.preview.timeout", "##testTimeout", timeout_);
        });
        AddFormGap(fieldRowGap);
        Ui::Hint(Label("build.preview.tests_no_build").c_str(), context_.theme.fonts);
    }

    void BuildWorkflowPreviewModal::DrawReleaseForm() {
        constexpr float fieldRowGap = 12.0F;
        Ui::BuildPreviewSectionHeading(Label("build.preview.release.section.identity").c_str(), context_.theme.fonts, false);
        DrawTextPair("##releaseIdentity", "build.preview.release.product_name", "##releaseName", name_,
                     "build.preview.release.version_semver", "##releaseVersion", version_);
        AddFormGap(fieldRowGap);
        DrawTextPair("##releaseSource", "build.preview.source_ref", "##releaseSourceRef", source_, "build.preview.release.notes_relative",
                     "##releaseNotes", notes_);
        Ui::BuildPreviewSectionHeading(Label("build.preview.release.section.target").c_str(), context_.theme.fonts, true);
        DrawComboField("build.preview.release.product_profile", "##releaseProfile", profile_, kReleaseProfiles.data(),
                       static_cast<int>(kReleaseProfiles.size()));
        AddFormGap(fieldRowGap);
        DrawPair("##releaseTarget", [this] {
            DrawComboField("build.preview.platform", "##releasePlatform", platform_, kPlatforms.data(),
                           static_cast<int>(kPlatforms.size()));
        }, [this] {
            DrawComboField("build.preview.architecture", "##releaseArch", architecture_, kArchitectures.data(),
                           static_cast<int>(kArchitectures.size()));
        });
        AddFormGap(fieldRowGap);
        DrawPair("##releasePackageRow", [this] {
            DrawComboField("build.preview.configuration", "##releaseConfig", configuration_, kReleaseConfigurations.data(),
                           static_cast<int>(kReleaseConfigurations.size()));
        }, [this] {
            DrawComboField("build.preview.release.package_format", "##releasePackage", package_, kReleasePackages.data(),
                           static_cast<int>(kReleasePackages.size()));
        });
        AddFormGap(fieldRowGap);
        DrawComboField("build.preview.release.content_selection", "##releaseContent", content_, kReleaseContent.data(),
                       static_cast<int>(kReleaseContent.size()));
        Ui::BuildPreviewSectionHeading(Label("build.preview.release.section.quality").c_str(), context_.theme.fonts, true);
        DrawComboField("build.preview.release.project_test_profile", "##releaseTests", testProfile_, kReleaseTestProfiles.data(),
                       static_cast<int>(kReleaseTestProfiles.size()));
        AddFormGap(5.0F);
        Ui::Hint(Label("build.preview.release.smoke_hint").c_str(), context_.theme.fonts);
        AddFormGap(14.0F);
        Ui::BuildPreviewNotice("##releaseChecksNotice", Label("build.preview.release.checks_notice").c_str(), context_.theme.fonts);
        Ui::BuildPreviewSectionHeading(Label("build.preview.release.section.security").c_str(), context_.theme.fonts, true);
        DrawPair("##releaseSecurity", [this] {
            DrawComboField("build.preview.release.signing_reference", "##releaseSigning", signing_, kReleaseSigning.data(),
                           static_cast<int>(kReleaseSigning.size()));
        }, [this] {
            DrawComboField("build.preview.release.protection_profile", "##releaseProtection", protection_, kProtection.data(),
                           static_cast<int>(kProtection.size()));
        });
        AddFormGap(fieldRowGap);
        DrawTextField("build.preview.output_root", "##releaseOutput", output_);
        AddFormGap(16.0F);
        ImGui::Separator();
        AddFormGap(12.0F);
        if (Ui::TextLink("##advancedRelease", Label("build.preview.advanced").c_str(), context_.theme.fonts.sans, Theme::TextPx::Body()))
            showAdvanced_ = !showAdvanced_;
        ImGui::SameLine(0.0F, Ui::ScaledLayoutValue(14.0F));
        {
            Theme::ScopedTextStyle style(context_.theme.fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
            ImGui::TextColored(Theme::Muted(), "%s", Label("build.preview.release.advanced_hint").c_str());
        }
        if (showAdvanced_) {
            AddFormGap(fieldRowGap);
            DrawPair("##releaseAdvanced", [this] {
                DrawComboField("build.preview.toolchain", "##releaseToolchain", toolchain_, kReleaseToolchains.data(),
                               static_cast<int>(kReleaseToolchains.size()));
            }, [this] {
                DrawTextField("build.preview.parallel_jobs", "##releaseParallel", parallelJobs_);
            });
        }
        AddFormGap(14.0F);
        Ui::BuildPreviewNotice("##releaseBoundaryNotice", Label("build.preview.release_boundary").c_str(), context_.theme.fonts);
    }

    void BuildWorkflowPreviewModal::DrawPublishForm() {
        if (!state_.HasVerifiedCandidate()) {
            Ui::Hint(Label("build.preview.no_candidate").c_str(), context_.theme.fonts);
            return;
        }
        Ui::FieldLabel(Label("build.preview.candidate").c_str(), context_.theme.fonts);
        ImGui::TextUnformatted(state_.CandidateName().c_str());
        ImGui::Dummy({0.0F, 12.0F});
        Ui::FieldLabel(Label("build.preview.local_candidate").c_str(), context_.theme.fonts);
        ImGui::TextWrapped("%s", state_.CandidatePath().c_str());
        ImGui::Dummy({0.0F, 12.0F});
        DrawPair("##publishRoute", [this] {
            DrawComboField("build.preview.channel", "##publishChannel", channel_, kChannels.data(), static_cast<int>(kChannels.size()));
        }, [this] {
            DrawComboField("build.preview.destination", "##publishDestination", destinationIndex_, kDestinations.data(),
                           static_cast<int>(kDestinations.size()));
        });
        if (destinationIndex_ == 3)
            DrawTextField("build.preview.custom_destination", "##publishCustomDestination", destination_);
        DrawPair("##publishAccess", [this] {
            DrawComboField("build.preview.visibility", "##publishVisibility", visibility_, kVisibility.data(),
                           static_cast<int>(kVisibility.size()));
        }, [this] {
            DrawComboField("build.preview.credentials", "##publishCredentials", credential_, kCredentials.data(),
                           static_cast<int>(kCredentials.size()));
        });
        DrawTextField("build.preview.notes", "##publishNotes", notes_);
        Ui::Hint(Label("build.preview.publish_boundary").c_str(), context_.theme.fonts);
    }

    void BuildWorkflowPreviewModal::DrawForm() {
        if (state_.Job() && state_.Job()->status == BuildPreviewStatus::Running && state_.Job()->request.kind != kind_) {
            Ui::Hint(Label("build.preview.job_running").c_str(), context_.theme.fonts);
            ImGui::Dummy({0.0F, 12.0F});
        }
        switch (kind_) {
            using enum BuildPreviewKind;
            case Build:
                DrawBuildForm();
                break;
            case Tests:
                DrawTestForm();
                break;
            case Release:
                DrawReleaseForm();
                break;
            case Publish:
                DrawPublishForm();
                break;
        }
    }

    void BuildWorkflowPreviewModal::DrawSummary() const {
        const bool showingJob = state_.Job() && state_.Job()->request.kind == kind_ && page_ == Page::Activity;
        const bool completed = showingJob && state_.Job()->status == BuildPreviewStatus::Completed;
        const bool running = showingJob && state_.Job()->status == BuildPreviewStatus::Running;
        const char *statusKey = "build.preview.draft";
        if (completed)
            statusKey = "build.preview.badge.completed";
        else if (running)
            statusKey = state_.Job()->stages[state_.Job()->stageIndex].labelKey;
        const std::string status = Label(statusKey);
        std::string firstLabel;
        std::string firstValue;
        std::string secondLabel;
        std::string secondValue;
        switch (kind_) {
            using enum BuildPreviewKind;
            case Build:
                firstLabel = Label(showingJob ? "build.preview.target" : "build.preview.workflow");
                secondLabel = Label(showingJob ? "build.preview.output" : "build.preview.target");
                if (showingJob) {
                    firstValue = state_.Job()->request.target;
                    secondValue = state_.Job()->request.output;
                } else {
                    firstValue = Label(kModes[static_cast<std::size_t>(mode_)]);
                    secondValue = Label(kPlatforms[static_cast<std::size_t>(platform_)]) + " · " +
                                  Label(kArchitectures[static_cast<std::size_t>(architecture_)]) + " · " +
                                  Label(kConfigurations[static_cast<std::size_t>(configuration_)]);
                }
                break;
            case Tests:
                firstLabel = Label("build.preview.test_source");
                firstValue = Label(kSources[static_cast<std::size_t>(testSource_)]);
                secondLabel = Label("build.preview.test_profile");
                secondValue = Label(kTestProfiles[static_cast<std::size_t>(testProfile_)]);
                break;
            case Release:
                firstLabel = Label("build.preview.target");
                firstValue = ReleaseTargetLabel();
                secondLabel = Label("build.preview.local_candidate");
                secondValue = ReleaseCandidatePath();
                break;
            case Publish:
                firstLabel = Label("build.preview.candidate");
                firstValue = state_.HasVerifiedCandidate() ? state_.CandidateName() : Label("build.preview.no_candidate.short");
                secondLabel = Label("build.preview.channel");
                secondValue = Label(kChannels[static_cast<std::size_t>(channel_)]);
                break;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{20.0F, 0.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{8.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg2());
        ImGui::BeginChild("##buildPreviewSummary", {0.0F, Ui::ScaledLayoutValue(60.0F)}, false,
                          ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetCursorPosY(Ui::ScaledLayoutValue(17.0F));
        Ui::Badge({.label = status.c_str(), .tone = completed ? Ui::BadgeTone::Success : Ui::BadgeTone::Accent, .leadingIndicator = true},
                  context_.theme.fonts);
        const auto drawValue = [this](const std::string &label, const std::string &value) {
            ImGui::BeginGroup();
            {
                Theme::ScopedTextStyle style(context_.theme.fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
                ImGui::TextColored(Theme::Muted(), "%s", label.c_str());
            }
            {
                Theme::ScopedTextStyle style(context_.theme.fonts.sans, Theme::TextPx::Body(), Theme::FontPx::Sans);
                ImGui::TextColored(Theme::Text(), "%s", value.c_str());
            }
            ImGui::EndGroup();
        };
        ImGui::SameLine(0.0F, Ui::ScaledLayoutValue(26.0F));
        ImGui::SetCursorPosY(Ui::ScaledLayoutValue(11.0F));
        drawValue(firstLabel, firstValue);
        ImGui::SameLine(0.0F, Ui::ScaledLayoutValue(26.0F));
        ImGui::SetCursorPosY(Ui::ScaledLayoutValue(11.0F));
        drawValue(secondLabel, secondValue);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        const ImVec2 summaryMin = ImGui::GetItemRectMin();
        const ImVec2 summaryMax = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine({summaryMin.x, summaryMax.y - 1.0F}, {summaryMax.x, summaryMax.y - 1.0F},
                                            Theme::U32(Theme::Border()));
    }

    void BuildWorkflowPreviewModal::DrawPipeline() const {
        const BuildPreviewJob &job = *state_.Job();
        std::vector<Ui::BuildPreviewPipelineStep> steps;
        steps.reserve(job.stages.size());
        for (std::size_t index = 0; index < job.stages.size(); ++index) {
            const bool completed = job.status == BuildPreviewStatus::Completed || index < job.stageIndex;
            const bool current = job.status == BuildPreviewStatus::Running && index == job.stageIndex;
            const char *labelKey = job.stages[index].pipelineLabelKey ? job.stages[index].pipelineLabelKey : job.stages[index].labelKey;
            steps.push_back({.label = Label(labelKey), .completed = completed, .current = current, .progress = job.stageProgress[index]});
        }
        Ui::BuildPreviewPipeline(steps, context_.theme.fonts);
    }

    void BuildWorkflowPreviewModal::DrawReviewRow(const char *labelKey, const std::string &value) {
        reviewRows_.push_back(
            Ui::TableRow{.cells = {Ui::TableCell{.text = Label(labelKey), .color = Theme::Muted()}, Ui::TableCell{.text = value}}});
    }

    void BuildWorkflowPreviewModal::DrawBuildReviewRows() {
        DrawReviewRow("build.preview.profile", Label(kProfiles[static_cast<std::size_t>(profile_)]));
        DrawReviewRow("build.preview.configuration", Label(kConfigurations[static_cast<std::size_t>(configuration_)]));
        DrawReviewRow("build.preview.content", Label(kContent[static_cast<std::size_t>(content_)]));
        DrawReviewRow("build.preview.workflow", Label(kModes[static_cast<std::size_t>(mode_)]));
        DrawReviewRow("build.preview.platform", Label(kPlatforms[static_cast<std::size_t>(platform_)]));
        DrawReviewRow("build.preview.test_after", Label(testAfterBuild_ ? "build.preview.yes" : "build.preview.no"));
        DrawReviewRow("build.preview.run_after", Label(runAfterBuild_ ? "build.preview.yes" : "build.preview.no"));
        DrawReviewRow("build.preview.output", output_);
    }

    void BuildWorkflowPreviewModal::DrawTestReviewRows() {
        DrawReviewRow("build.preview.test_source", Label(kSources[static_cast<std::size_t>(testSource_)]));
        if (testSource_ == 1)
            DrawReviewRow("build.preview.package_path", packagePath_);
        DrawReviewRow("build.preview.platform", Label(kPlatforms[static_cast<std::size_t>(platform_)]));
        DrawReviewRow("build.preview.test_profile", Label(kTestProfiles[static_cast<std::size_t>(testProfile_)]));
        const std::string filter = testFilter_.empty() ? Label("build.preview.all_tests") : testFilter_;
        DrawReviewRow("build.preview.test_filter", filter);
        DrawReviewRow("build.preview.timeout", timeout_);
    }

    void BuildWorkflowPreviewModal::DrawReleaseReview() const {
        const auto field = [this](const char *labelKey, const std::string &value) {
            return Ui::BuildPreviewReviewField{.label = Label(labelKey), .value = value};
        };
        const std::array
            sections{Ui::BuildPreviewReviewSection{.title = Label("build.preview.release.review.identity"),
                                                   .fields = {field("build.preview.release.review.product", name_ + " v" + version_),
                                                              field("build.preview.release.review.version", version_),
                                                              field("build.preview.release.review.source", source_),
                                                              field("build.preview.release.review.notes", notes_)}},
                     Ui::BuildPreviewReviewSection{.title = Label("build.preview.release.review.target_package"),
                                                   .fields = {field("build.preview.release.review.profile",
                                                                    Label(kReleaseProfiles[static_cast<std::size_t>(profile_)])),
                                                              field("build.preview.target", ReleaseTargetLabel()),
                                                              field("build.preview.release.review.package",
                                                                    Label(kReleasePackages[static_cast<std::size_t>(package_)])),
                                                              field("build.preview.content",
                                                                    Label(kReleaseContent[static_cast<std::size_t>(content_)]))}},
                     Ui::BuildPreviewReviewSection{.title = Label("build.preview.release.review.verification_security"),
                                                   .fields = {field("build.preview.release.review.tests",
                                                                    Label(kReleaseTestProfiles[static_cast<std::size_t>(testProfile_)])),
                                                              field("build.preview.release.review.signing",
                                                                    Label(kReleaseSigning[static_cast<std::size_t>(signing_)])),
                                                              field("build.preview.release.review.archive_protection",
                                                                    Label(kProtection[static_cast<std::size_t>(protection_)]))}},
                     Ui::BuildPreviewReviewSection{.title = Label("build.preview.release.review.local_output"),
                                                   .fields = {field("build.preview.output_root", output_),
                                                              field("build.preview.release.review.candidate_path", ReleaseCandidatePath()),
                                                              field("build.preview.release.review.result",
                                                                    Label("build.preview.release.review.verified_local_only"))}}};
        const std::string heading = Label("build.preview.release.review.heading");
        const std::string description = Label("build.preview.release.review.description");
        const std::string notice = Label("build.preview.release.review.notice");
        Ui::BuildPreviewReview(heading.c_str(), description.c_str(), notice.c_str(), sections, context_.theme.fonts);
    }

    void BuildWorkflowPreviewModal::DrawPublishReviewRows() {
        DrawReviewRow("build.preview.candidate", state_.CandidateName());
        DrawReviewRow("build.preview.local_candidate", state_.CandidatePath());
        DrawReviewRow("build.preview.platform", state_.CandidateTarget());
        DrawReviewRow("build.preview.channel", Label(kChannels[static_cast<std::size_t>(channel_)]));
        if (destinationIndex_ == 3)
            DrawReviewRow("build.preview.destination", destination_);
        else
            DrawReviewRow("build.preview.destination", Label(kDestinations[static_cast<std::size_t>(destinationIndex_)]));
        DrawReviewRow("build.preview.visibility", Label(kVisibility[static_cast<std::size_t>(visibility_)]));
        DrawReviewRow("build.preview.credentials", Label(kCredentials[static_cast<std::size_t>(credential_)]));
        DrawReviewRow("build.preview.notes", notes_);
    }

    void BuildWorkflowPreviewModal::DrawReview() {
        if (kind_ == BuildPreviewKind::Release) {
            DrawReleaseReview();
            return;
        }
        Ui::FieldLabel(Label("build.preview.review_heading").c_str(), context_.theme.fonts);
        reviewRows_.clear();
        switch (kind_) {
            using enum BuildPreviewKind;
            case Build:
                DrawBuildReviewRows();
                break;
            case Tests:
                DrawTestReviewRows();
                break;
            case Release:
                break;
            case Publish:
                DrawPublishReviewRows();
                break;
        }
        const std::array columns{Ui::TableColumn{.id = "field", .label = Label("build.preview.table_field"), .width = 190.0F},
                                 Ui::TableColumn{.id = "value", .label = Label("build.preview.table_value")}};
        static_cast<void>(
            Ui::DrawTable({.id = "##buildPreviewReview", .selectableCells = false}, columns, reviewRows_, context_.theme.fonts));
        ImGui::Dummy({0.0F, 14.0F});
        const char *noticeKey = kind_ == BuildPreviewKind::Publish ? "build.preview.publish_boundary" : "build.preview.mock_notice";
        Ui::Hint(Label(noticeKey).c_str(), context_.theme.fonts);
    }

    const char *BuildWorkflowPreviewModal::ActivityResultKey() const noexcept {
        switch (kind_) {
            using enum BuildPreviewKind;
            case Build:
                return "build.preview.result.build";
            case Tests:
                return "build.preview.result.tests";
            case Release:
                return "build.preview.result.release";
            case Publish:
                return "build.preview.result.publish";
        }
        return "build.preview.completed";
    }

    void BuildWorkflowPreviewModal::DrawActivityNotice(const BuildPreviewJob &job) const {
        ImVec4 noticeTone = Theme::Warn();
        const char *noticeKey = "build.preview.notice.cancelled";
        if (job.status == BuildPreviewStatus::Running) {
            noticeTone = Theme::Accent();
            noticeKey = "build.preview.notice.running";
        } else if (job.status == BuildPreviewStatus::Completed) {
            noticeTone = Theme::Ok();
            noticeKey = CompletionNoticeKey(kind_);
        }
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg2());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Border());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16.0F, 11.0F});
        ImGui::BeginChild("##buildActivityNotice", {0.0F, Ui::ScaledLayoutValue(45.0F)}, true,
                          ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar);
        {
            Theme::ScopedTextStyle style(context_.theme.fonts.sans, Theme::TextPx::Body(), Theme::FontPx::Sans);
            ImGui::TextColored(Theme::Text(), "%s", Label(noticeKey).c_str());
        }
        const ImVec2 noticePosition = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine({noticePosition.x + 1.0F, noticePosition.y + 1.0F},
                                            {noticePosition.x + 1.0F, noticePosition.y + ImGui::GetWindowHeight() - 1.0F},
                                            Theme::U32(noticeTone), 3.0F);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }

    void BuildWorkflowPreviewModal::DrawActivityLog(const BuildPreviewJob &job) const {
        const bool running = job.status == BuildPreviewStatus::Running;
        const bool completed = job.status == BuildPreviewStatus::Completed;
        if (ImGui::BeginTable("##buildActivityLog", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX)) {
            ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, Ui::ScaledLayoutValue(68.0F));
            ImGui::TableSetupColumn("step", ImGuiTableColumnFlags_WidthFixed, Ui::ScaledLayoutValue(116.0F));
            ImGui::TableSetupColumn("message", ImGuiTableColumnFlags_WidthStretch);
            const auto drawRow = [](const std::string &time, const std::string &stage, const std::string &message, const bool success,
                                    const bool current) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(success ? Theme::Ok() : Theme::Muted(), "%s", time.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(success ? Theme::Ok() : Theme::Accent(), "%s", stage.c_str());
                ImGui::TableSetColumnIndex(2);
                if (current) {
                    const ImVec2 marker = ImGui::GetCursorScreenPos();
                    const float lineHeight = ImGui::GetTextLineHeight();
                    ImGui::Dummy({7.0F, lineHeight});
                    ImGui::GetWindowDrawList()->AddCircleFilled({marker.x + 3.5F, marker.y + lineHeight * 0.5F}, 3.0F,
                                                                Theme::U32(Theme::Accent()));
                    ImGui::SameLine(0.0F, 7.0F);
                }
                ImGui::TextColored(success ? Theme::Ok() : Theme::Text(), "%s", message.c_str());
            };
            Theme::ScopedTextStyle style(context_.theme.fonts.sans, Theme::TextPx::Body(), Theme::FontPx::Sans);
            for (std::size_t index = 0; index < job.stages.size(); ++index) {
                if (index > job.stageIndex || (!running && !completed && index == job.stageIndex))
                    break;
                const bool current = running && index == job.stageIndex;
                const BuildPreviewStage &stage = job.stages[index];
                drawRow(std::format("{:02}s", index * 2), Label(stage.labelKey), Label(current ? stage.activityKey : stage.logKey), false,
                        current);
            }
            if (completed)
                drawRow("✓", Label("build.preview.step.success"), Label(ActivityResultKey()), true, false);
            ImGui::EndTable();
        }
    }

    void BuildWorkflowPreviewModal::DrawActivity() const {
        if (!state_.Job())
            return;
        const BuildPreviewJob &job = *state_.Job();
        DrawActivityNotice(job);
        AddFormGap(17.0F);
        DrawActivityLog(job);
    }

    bool BuildWorkflowPreviewModal::CanSubmit() const {
        if (state_.Job() && state_.Job()->status == BuildPreviewStatus::Running)
            return false;
        if (kind_ == BuildPreviewKind::Publish)
            return state_.HasVerifiedCandidate() && !notes_.empty() && (destinationIndex_ != 3 || !destination_.empty());
        if (kind_ == BuildPreviewKind::Release)
            return !name_.empty() && !version_.empty() && !source_.empty() && !notes_.empty() && !output_.empty() &&
                   IsPositiveInteger(parallelJobs_);
        if (kind_ == BuildPreviewKind::Tests)
            return (testSource_ == 0 || !packagePath_.empty()) && IsPositiveInteger(timeout_);
        return !output_.empty() && IsPositiveInteger(parallelJobs_);
    }

    std::string BuildWorkflowPreviewModal::ReleaseCandidatePath() const {
        constexpr std::array<const char *, 3> platforms{"windows", "linux", "macos"};
        constexpr std::array<const char *, 2> architectures{"x86_64", "arm64"};
        constexpr std::array<const char *, 3> configurations{"development", "test", "shipping"};
        std::string root = output_;
        while (!root.empty() && (root.back() == '/' || root.back() == '\\'))
            root.pop_back();
        return root + "/" + version_ + "_" + platforms[static_cast<std::size_t>(platform_)] + "_" +
               architectures[static_cast<std::size_t>(architecture_)] + "_" + configurations[static_cast<std::size_t>(configuration_)] +
               "/";
    }

    std::string BuildWorkflowPreviewModal::BuildTargetLabel() const {
        return Label(kProfiles[static_cast<std::size_t>(profile_)]) + " · " + Label(kPlatforms[static_cast<std::size_t>(platform_)]) +
               " · " + Label(kArchitectures[static_cast<std::size_t>(architecture_)]) + " · " +
               Label(kConfigurations[static_cast<std::size_t>(configuration_)]);
    }

    std::string BuildWorkflowPreviewModal::ReleaseTargetLabel() const {
        return Label(kPlatforms[static_cast<std::size_t>(platform_)]) + " · " +
               Label(kArchitectures[static_cast<std::size_t>(architecture_)]) + " · " +
               Label(kReleaseConfigurations[static_cast<std::size_t>(configuration_)]);
    }

    std::string BuildWorkflowPreviewModal::RequestOutput() const {
        if (kind_ == BuildPreviewKind::Release)
            return ReleaseCandidatePath();
        if (kind_ == BuildPreviewKind::Publish) {
            if (destinationIndex_ == 3)
                return destination_;
            return Label(kDestinations[static_cast<std::size_t>(destinationIndex_)]);
        }
        return output_;
    }

    void BuildWorkflowPreviewModal::Submit() {
        BuildPreviewRequest request;
        request.kind = kind_;
        request.name = name_;
        if (kind_ == BuildPreviewKind::Publish)
            request.name = state_.CandidateName();
        request.version = version_;
        request.target = Label(kPlatforms[static_cast<std::size_t>(platform_)]);
        if (kind_ == BuildPreviewKind::Build)
            request.target = BuildTargetLabel();
        else if (kind_ == BuildPreviewKind::Release)
            request.target = ReleaseTargetLabel();
        request.output = RequestOutput();
        request.testAfterBuild = testAfterBuild_;
        request.runAfterBuild = runAfterBuild_;
        request.compileOnly = kind_ == BuildPreviewKind::Build && mode_ == 1;
        if (state_.Start(std::move(request)))
            page_ = Page::Activity;
    }

    const char *BuildWorkflowPreviewModal::PrimaryActionKey() const noexcept {
        if (page_ == Page::Form && (kind_ == BuildPreviewKind::Release || kind_ == BuildPreviewKind::Publish))
            return "build.preview.review";
        switch (kind_) {
            using enum BuildPreviewKind;
            case Publish:
                return "build.preview.publish_action";
            case Release:
                return "build.preview.prepare_action";
            case Tests:
                return "build.preview.test_action";
            case Build:
                if (testAfterBuild_ && runAfterBuild_)
                    return "build.preview.build_test_run_action";
                if (testAfterBuild_)
                    return "build.preview.build_test_action";
                if (runAfterBuild_)
                    return "build.preview.build_run_action";
                return "build.preview.build_action";
        }
        return "build.preview.build_action";
    }

    void BuildWorkflowPreviewModal::DrawActivityFooter(const BuildPreviewJob &job) {
        const bool running = job.status == BuildPreviewStatus::Running;
        const bool completed = job.status == BuildPreviewStatus::Completed;
        const char *activityKey = "build.preview.cancelled";
        if (running)
            activityKey = job.stages[job.stageIndex].activityKey;
        else if (completed)
            activityKey = ActivityResultKey();
        const std::string activity = Label(activityKey);
        ImGui::SetCursorPos({Ui::ScaledLayoutValue(22.0F), (ImGui::GetWindowHeight() - ImGui::GetTextLineHeight()) * 0.5F});
        if (running)
            Ui::BuildPreviewSpinner();
        else
            ImGui::TextColored(completed ? Theme::Ok() : Theme::Warn(), "%s", completed ? "✓" : "×");
        ImGui::SameLine(0.0F, 8.0F);
        ImGui::TextColored(completed ? Theme::Ok() : Theme::Text(), "%s", activity.c_str());
        const std::string secondaryLabel = Label(running ? "build.preview.cancel_job" : "build.preview.new_request");
        const std::string primaryLabel = Label(running ? "build.preview.background" : "build.preview.close");
        const float secondaryWidth = ActionWidth(secondaryLabel);
        const float primaryWidth = ActionWidth(primaryLabel);
        const float actionTop = (ImGui::GetWindowHeight() - Ui::ScaledLayoutValue(34.0F)) * 0.5F;
        ImGui::SetCursorPos({std::max(Ui::ScaledLayoutValue(22.0F), ImGui::GetWindowWidth() - Ui::ScaledLayoutValue(22.0F) -
                                                                        secondaryWidth - Ui::ScaledLayoutValue(8.0F) - primaryWidth),
                             actionTop});
        if (Ui::Button({.label = secondaryLabel.c_str(),
                        .size = {secondaryWidth, 34.0F},
                        .variant = Ui::ButtonVariant::Secondary,
                        .font = context_.theme.fonts.sans})) {
            if (running)
                state_.Cancel();
            else
                page_ = Page::Form;
        }
        ImGui::SameLine(0.0F, 8.0F);
        if (Ui::Button({.label = primaryLabel.c_str(), .size = {primaryWidth, 34.0F}, .font = context_.theme.fonts.sans}))
            closeRequested_ = true;
    }

    void BuildWorkflowPreviewModal::DrawRequestFooter() {
        const float actionTop = (ImGui::GetWindowHeight() - Ui::ScaledLayoutValue(34.0F)) * 0.5F;
        ImGui::SetCursorPosY(actionTop);
        const std::string primaryLabel = Label(PrimaryActionKey());
        const float primaryWidth = ActionWidth(primaryLabel);
        if (page_ == Page::Review) {
            const std::string backLabel = Label("build.preview.back");
            const float backWidth = ActionWidth(backLabel);
            ImGui::SetCursorPosX(std::max(22.0F, ImGui::GetWindowWidth() - 22.0F - backWidth - 8.0F - primaryWidth));
            if (Ui::Button({.label = backLabel.c_str(),
                            .size = {backWidth, 34.0F},
                            .variant = Ui::ButtonVariant::Secondary,
                            .font = context_.theme.fonts.sans}))
                page_ = Page::Form;
            ImGui::SameLine(0.0F, 8.0F);
        } else {
            ImGui::SetCursorPosX(std::max(22.0F, ImGui::GetWindowWidth() - 22.0F - primaryWidth));
        }
        if (Ui::Button({.label = primaryLabel.c_str(),
                        .size = {primaryWidth, 34.0F},
                        .enabled = CanSubmit(),
                        .font = context_.theme.fonts.sans})) {
            const bool requiresReview = kind_ == BuildPreviewKind::Release || kind_ == BuildPreviewKind::Publish;
            if (page_ == Page::Form && requiresReview)
                page_ = Page::Review;
            else
                Submit();
        }
    }

    void BuildWorkflowPreviewModal::DrawFooter() {
        if (page_ == Page::Activity && state_.Job())
            DrawActivityFooter(*state_.Job());
        else
            DrawRequestFooter();
    }

    ModalFrameResult BuildWorkflowPreviewModal::Draw() {
        const std::string title = Label(kTitles[static_cast<std::size_t>(KindIndex(kind_))]);
        const ImVec2 requestedSize = kind_ == BuildPreviewKind::Release ? ImVec2{1000.0F, 700.0F} : ImVec2{804.0F, 562.0F};
        Ui::ScopedModalShell shell({.id = "BuildWorkflowPreview",
                                    .title = title.c_str(),
                                    .requestedSize = requestedSize,
                                    .minimumWidth = 660.0F,
                                    .minimumHeight = 460.0F,
                                    .headerHeight = 40.0F,
                                    .headerHorizontalPadding = 12.0F,
                                    .footerHeight = 60.0F,
                                    .foregroundBorder = true,
                                    .closeButtonVariant = Ui::CloseButtonVariant::Compact,
                                    .titleFontSize = Theme::TextPx::Title()},
                                   context_.theme.fonts);
        const bool hasPipeline = page_ == Page::Activity && state_.Job() && state_.Job()->request.kind == kind_;
        if (hasPipeline) {
            DrawPipeline();
            ImGui::SetCursorPosY(Ui::ScaledLayoutValue(92.0F));
        }
        DrawSummary();
        const float pipelineHeight = hasPipeline ? Ui::ScaledLayoutValue(52.0F) : 0.0F;
        ImGui::SetCursorPosY(Ui::ScaledLayoutValue(100.0F) + pipelineHeight);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{22.0F, 14.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{8.0F, 4.0F});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg1());
        ImGui::BeginChild("##buildWorkflowBody", {0.0F, shell.BodyHeight() - Ui::ScaledLayoutValue(60.0F) - pipelineHeight}, false,
                          ImGuiWindowFlags_AlwaysUseWindowPadding);
        if (page_ == Page::Form)
            DrawForm();
        else if (page_ == Page::Review)
            DrawReview();
        else
            DrawActivity();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        shell.BeginFooter({22.0F, 11.0F});
        DrawFooter();
        shell.EndFooter();
        if (shell.CloseRequested() || closeRequested_)
            return ModalFrameResult::RequestClose(ModalCloseReason::Cancelled);
        return ModalFrameResult::None();
    }
}  // namespace Horo::Editor
