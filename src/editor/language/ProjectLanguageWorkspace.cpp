#include "ProjectLanguageWorkspace.h"

#include "Horo/Editor/SourceFileOpenService.h"
#include "LspDocumentSync.h"

#include <algorithm>
#include <array>
#include <new>
#include <set>

namespace Horo::Editor::Language {
    namespace {
        constexpr std::size_t MaximumProviders = 8;
        constexpr std::size_t MaximumExtensionsPerProvider = 16;
        constexpr std::size_t MaximumArguments = 64;
        constexpr std::size_t MaximumArgumentBytes = 4096;
        constexpr auto MaximumOpenDocuments = SourceDocumentLimits{}.maximumDocuments;

        /** @brief Normalizes an ASCII extension without consulting the host locale. */
        std::string Lower(std::string value) {
            for (char &byte : value)
                if (byte >= 'A' && byte <= 'Z')
                    byte = static_cast<char>(byte + ('a' - 'A'));
            return value;
        }

        /** @brief Keeps signed path and argument boundaries in UTF-8 on all supported hosts. */
        std::string Utf8(const std::u8string &native) {
            return {reinterpret_cast<const char *>(native.data()), native.size()};
        }

        /** @brief Converts an already absolute project path into a POSIX, drive, or UNC file URI. */
        std::string ProjectUri(const std::filesystem::path &root) {
            const auto path = Utf8(root.generic_u8string());
            if (path.starts_with("//")) {
                const auto separator = path.find('/', 2);
                if (separator == std::string::npos || separator == 2)
                    return {};
                return DocumentUri("file://" + path.substr(2, separator - 2), std::string_view{path}.substr(separator + 1));
            }
            if (path.starts_with('/'))
                return DocumentUri("file:///", std::string_view{path}.substr(1));
            if (path.size() >= 3 && path[1] == ':' && path[2] == '/')
                return DocumentUri("file:///" + path.substr(0, 3), std::string_view{path}.substr(3));
            return {};
        }

        /** @brief Accepts one explicit suffix without wildcards or path separators. */
        bool ValidExtension(const std::string_view value) {
            return value.size() > 1 && value.size() <= 32 && value.front() == '.' &&
                   value.substr(1).find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_+-") == std::string_view::npos;
        }

        /** @brief Bounds trusted launch metadata and excludes shell resolution and embedded NUL arguments. */
        bool ValidLaunch(const ProjectLanguageProvider &provider) {
            return provider.executable.is_absolute() && provider.arguments.size() <= MaximumArguments &&
                   provider.executable.native().find(std::filesystem::path::value_type{}) == std::filesystem::path::string_type::npos &&
                   std::ranges::all_of(provider.arguments, [](const auto &argument) {
                return argument.size() <= MaximumArgumentBytes && argument.find('\0') == std::string::npos;
            });
        }

        /** @brief Validates descriptors before constructing any process owner. */
        bool ValidProvider(const ProjectLanguageProvider &provider) {
            return !provider.id.empty() && provider.id.size() <= 128 && !provider.languageId.empty() && provider.languageId.size() <= 128 &&
                   provider.languageId.find('\0') == std::string::npos && !provider.extensions.empty() &&
                   provider.extensions.size() <= MaximumExtensionsPerProvider && ValidLaunch(provider);
        }

        /** @brief Builds one unambiguous provider catalog before creating any workspace state. */
        std::optional<std::vector<ProjectLanguageProvider>> NormalizeProviders(const std::span<const ProjectLanguageProvider> providers) {
            std::vector<ProjectLanguageProvider> normalized;
            std::set<std::string> ids;
            std::set<std::string> extensions;
            for (const auto &provider : providers) {
                if (!ValidProvider(provider) || !ids.insert(provider.id).second)
                    return std::nullopt;
                auto registration = provider;
                for (auto &extension : registration.extensions) {
                    extension = Lower(std::move(extension));
                    if (!ValidExtension(extension) || !extensions.insert(extension).second)
                        return std::nullopt;
                }
                normalized.push_back(std::move(registration));
            }
            return normalized;
        }
    }  // namespace

    /** @brief Holds one stable source session across process replacement and defers worker retirement to Poll. */
    struct ProjectLanguageWorkspace::Provider final {
        explicit Provider(ProjectLanguageProvider configuration) : settings(std::move(configuration)) {}

        ProjectLanguageProvider settings;
        SourceLanguageSession source;
        std::unique_ptr<LspProcess> process;
        std::map<DocumentInstanceId, SourceDocumentSnapshot> observed;
        std::optional<Error> error;
        bool restartPending{};
    };

    /** @copydoc ProjectLanguageWorkspace::ProjectLanguageWorkspace */
    ProjectLanguageWorkspace::ProjectLanguageWorkspace(IExternalProcessRunner &runner, const SourceDocumentService &documents,
                                                       std::filesystem::path root, std::string uri)
        : runner_(runner), documents_(documents), root_(std::move(root)), uri_(std::move(uri)) {}

    /** @copydoc ProjectLanguageWorkspace::~ProjectLanguageWorkspace */
    ProjectLanguageWorkspace::~ProjectLanguageWorkspace() = default;

    /** @copydoc ProjectLanguageWorkspace::Create */
    Result<std::unique_ptr<ProjectLanguageWorkspace>> ProjectLanguageWorkspace::Create(
        IExternalProcessRunner &runner, SourceFileOpenService &sources, const std::span<const ProjectLanguageProvider> providers) {
        using Output = Result<std::unique_ptr<ProjectLanguageWorkspace>>;
        const auto &root = sources.ProjectRoot();
        if (!root.is_absolute() || providers.size() > MaximumProviders)
            return Output::Failure(MakeError(SourceDocumentErrors::Invalid));
        try {
            auto path = root.lexically_normal();
            auto uri = ProjectUri(path);
            if (uri.empty() || uri.size() > LspLifecycle::MaximumRootUriBytes)
                return Output::Failure(MakeError(SourceDocumentErrors::Invalid));
            auto settings = NormalizeProviders(providers);
            if (!settings)
                return Output::Failure(MakeError(SourceDocumentErrors::Invalid));
            std::unique_ptr<ProjectLanguageWorkspace> workspace{
                new ProjectLanguageWorkspace(runner, sources.Documents(), std::move(path), std::move(uri))};
            for (auto &registration : *settings)
                workspace->providers_.push_back(std::make_unique<Provider>(std::move(registration)));
            return Output::Success(std::move(workspace));
        } catch (const std::filesystem::filesystem_error &) {
            return Output::Failure(MakeError(SourceDocumentErrors::Invalid));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(SourceDocumentErrors::TooLarge));
        }
    }

    /** @copydoc ProjectLanguageWorkspace::Select */
    std::optional<std::size_t> ProjectLanguageWorkspace::Select(const SourceDocumentSnapshot &source) const {
        if (!source.IsCurrent())
            return std::nullopt;
        const auto identity = source.Identity();
        const auto owned = documents_.Snapshot(identity.instance);
        if (owned.HasError() || !owned.Value().SharesStateWith(source))
            return std::nullopt;
        const std::string_view path{identity.key.source.Value()};
        const auto dot = path.rfind('.');
        if (dot == std::string_view::npos || path.substr(dot).find('/') != std::string_view::npos)
            return std::nullopt;
        const auto extension = Lower(std::string{path.substr(dot)});
        for (std::size_t index = 0; index < providers_.size(); ++index) {
            const auto &extensions = providers_[index]->settings.extensions;
            if (std::ranges::find(extensions, extension) != extensions.end())
                return index;
        }
        return std::nullopt;
    }

    /** @copydoc ProjectLanguageWorkspace::Start */
    void ProjectLanguageWorkspace::Start(Provider &provider, const Clock::time_point now) {
        try {
            ExternalProcessRequest request;
            request.executable = Utf8(provider.settings.executable.u8string());
            request.arguments = provider.settings.arguments;
            request.workingDirectory = root_;
            request.timeout = std::chrono::hours{24};
            request.gracefulTermination = std::chrono::milliseconds{50};
            request.maximumDrainDuration = std::chrono::milliseconds{250};
            provider.process = std::make_unique<LspProcess>(runner_, provider.source);
            auto result = provider.process->Start(std::move(request), uri_, std::nullopt, now);
            if (result.HasError())
                provider.error = std::move(result).ErrorValue();
        } catch (const std::filesystem::filesystem_error &) {
            provider.error = MakeError(SourceDocumentErrors::Invalid);
        } catch (const std::bad_alloc &) {
            provider.error = MakeError(SourceDocumentErrors::TooLarge);
        }
    }

    /** @copydoc ProjectLanguageWorkspace::Sync */
    void ProjectLanguageWorkspace::Sync(Provider &provider, const std::span<const SourceDocumentSnapshot> documents,
                                        const std::size_t providerIndex) {
        using OpenDocument = std::pair<DocumentInstanceId, const SourceDocumentSnapshot *>;
        std::array<OpenDocument, MaximumOpenDocuments> active{};
        std::size_t size = 0;
        for (const auto &document : documents) {
            if (Select(document) != providerIndex)
                continue;
            active[size++] = {document.Identity().instance, &document};
        }
        const auto retained = std::span{active}.first(size);
        std::erase_if(provider.observed, [&](const auto &entry) {
            if (std::ranges::find(retained, entry.first, &OpenDocument::first) != retained.end())
                return false;
            auto closed = provider.process->Connection().CloseSource(entry.first);
            if (closed.HasError())
                provider.error = std::move(closed).ErrorValue();
            return true;
        });
        for (const auto &[instance, document] : retained) {
            auto &previous = provider.observed[instance];
            if (previous.SharesStateWith(*document))
                continue;
            auto sent = provider.process->Connection().SyncSource(*document, provider.settings.languageId);
            if (sent.HasError())
                provider.error = std::move(sent).ErrorValue();
            previous = *document;
        }
    }

    /** @copydoc ProjectLanguageWorkspace::Pump */
    void ProjectLanguageWorkspace::Pump(Provider &provider, std::vector<LanguageResult> &results, const Clock::time_point now) {
        auto output = provider.process->Poll(now);
        if (output.HasError()) {
            provider.error = std::move(output).ErrorValue();
        } else if (output.Value() && output.Value()->reply) {
            auto projected = provider.process->Connection().ProjectReply(*output.Value());
            if (projected.HasValue())
                results.push_back(std::move(projected).Value());
        }
        if (provider.process->Finished()) {
            provider.process.reset();
            provider.observed.clear();
            if (std::exchange(provider.restartPending, false))
                provider.error.reset();
        }
    }

    /** @copydoc ProjectLanguageWorkspace::Poll */
    std::vector<LanguageResult> ProjectLanguageWorkspace::Poll(const std::span<const SourceDocumentSnapshot> documents,
                                                               const Clock::time_point now) {
        std::vector<LanguageResult> results;
        const auto bounded = documents.first(std::min(documents.size(), MaximumOpenDocuments));
        for (std::size_t index = 0; index < providers_.size(); ++index) {
            auto &provider = *providers_[index];
            if (!shuttingDown_ && !provider.process && !provider.error && std::ranges::any_of(bounded, [&](const auto &document) {
                return Select(document) == index;
            }))
                Start(provider, now);
            if (!provider.process)
                continue;
            Pump(provider, results, now);
            if (!shuttingDown_ && provider.process && provider.source.State() == SourceLanguageSessionState::Ready)
                Sync(provider, bounded, index);
        }
        return results;
    }

    /** @copydoc ProjectLanguageWorkspace::Request */
    Result<ProjectLanguageRequest> ProjectLanguageWorkspace::Request(const SourceDocumentSnapshot &source,
                                                                     const SourceLanguageRequestKind kind, const LspSourceQuery query) {
        using Output = Result<ProjectLanguageRequest>;
        if (shuttingDown_)
            return Output::Failure(MakeError(SourceDocumentErrors::Closed));
        const auto index = Select(source);
        if (!index || !providers_[*index]->process)
            return Output::Failure(MakeError(SourceDocumentErrors::Closed));
        auto id = providers_[*index]->process->Connection().Request(source, kind, query);
        if (id.HasError())
            return Output::Failure(std::move(id).ErrorValue());
        return Output::Success({*index, id.Value(), identity_});
    }

    /** @copydoc ProjectLanguageWorkspace::Cancel */
    Result<void> ProjectLanguageWorkspace::Cancel(const ProjectLanguageRequest request) {
        if (request.workspace.lock() != identity_ || request.provider >= providers_.size() || !providers_[request.provider]->process)
            return Result<void>::Success();
        return providers_[request.provider]->process->Connection().CancelRequest(request.request);
    }

    /** @copydoc ProjectLanguageWorkspace::Restart */
    bool ProjectLanguageWorkspace::Restart(const std::string_view providerId, const Clock::time_point now) {
        if (shuttingDown_)
            return false;
        const auto found = std::ranges::find_if(providers_, [&](const auto &entry) {
            return entry->settings.id == providerId;
        });
        if (found == providers_.end())
            return false;
        auto &provider = **found;
        provider.error.reset();
        provider.restartPending = provider.process != nullptr;
        Stop(provider, now);
        return true;
    }

    /** @copydoc ProjectLanguageWorkspace::ProviderError */
    const Error *ProjectLanguageWorkspace::ProviderError(const std::string_view providerId) const noexcept {
        const auto found = std::ranges::find_if(providers_, [&](const auto &entry) {
            return entry->settings.id == providerId;
        });
        if (found == providers_.end() || !(*found)->error)
            return nullptr;
        return &*(*found)->error;
    }

    /** @copydoc ProjectLanguageWorkspace::Shutdown */
    void ProjectLanguageWorkspace::Shutdown(const Clock::time_point now) {
        shuttingDown_ = true;
        for (auto &provider : providers_) {
            provider->restartPending = false;
            Stop(*provider, now);
        }
    }

    /** @copydoc ProjectLanguageWorkspace::Finished */
    bool ProjectLanguageWorkspace::Finished() const noexcept {
        return std::ranges::none_of(providers_, [](const auto &provider) {
            return provider->process != nullptr;
        });
    }

    /** @copydoc ProjectLanguageWorkspace::Stop */
    void ProjectLanguageWorkspace::Stop(Provider &provider, const Clock::time_point now) {
        if (!provider.process)
            return;
        auto stopped = provider.process->Connection().Shutdown(now);
        if (stopped.HasError())
            provider.error = std::move(stopped).ErrorValue();
    }
}  // namespace Horo::Editor::Language
