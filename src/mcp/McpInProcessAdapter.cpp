#include "Horo/Mcp/McpInProcessAdapter.h"

#include "Horo/Mcp/McpErrors.h"

#include <utility>

namespace Horo::Mcp {
    /** @copydoc McpInProcessAdapter::Start */
    Result<std::shared_ptr<McpInProcessAdapter>> McpInProcessAdapter::Start(std::shared_ptr<McpSessionManager> manager,
                                                                            McpSessionAdmission admission) {
        if (manager == nullptr)
            return Result<std::shared_ptr<McpInProcessAdapter>>::Failure(MakeError(McpErrors::ConfigurationInvalid));
        auto opened = manager->Open(std::move(admission));
        if (opened.HasError())
            return Result<std::shared_ptr<McpInProcessAdapter>>::Failure(opened.ErrorValue());
        return Result<std::shared_ptr<McpInProcessAdapter>>::Success(
            std::shared_ptr<McpInProcessAdapter>(new McpInProcessAdapter(std::move(manager), opened.Value())));
    }

    McpInProcessAdapter::~McpInProcessAdapter() {
        Disconnect();
    }

    /** @copydoc McpInProcessAdapter::Call */
    Result<nlohmann::json> McpInProcessAdapter::Call(const McpRequest &request) const {
        const McpSessionHandle session = Session();
        return manager_->Dispatch(session, request);
    }

    /** @copydoc McpInProcessAdapter::Cancel */
    Result<void> McpInProcessAdapter::Cancel(const nlohmann::json &requestId) const {
        const McpSessionHandle session = Session();
        return manager_->Cancel(session, requestId);
    }

    /** @copydoc McpInProcessAdapter::SwitchProject */
    Result<McpSessionHandle> McpInProcessAdapter::SwitchProject(std::optional<std::string> projectIdentity) {
        std::lock_guard lock{mutex_};
        auto switched = manager_->SwitchProject(session_, std::move(projectIdentity));
        if (switched.HasValue())
            session_ = switched.Value();
        return switched;
    }

    /** @copydoc McpInProcessAdapter::Disconnect */
    void McpInProcessAdapter::Disconnect() noexcept {
        McpSessionHandle closed;
        {
            std::lock_guard lock{mutex_};
            closed = session_;
            session_ = {};
        }
        if (closed.IsValid())
            manager_->Close(closed);
    }

    /** @copydoc McpInProcessAdapter::Session */
    McpSessionHandle McpInProcessAdapter::Session() const noexcept {
        std::lock_guard lock{mutex_};
        return session_;
    }

    McpInProcessAdapter::McpInProcessAdapter(std::shared_ptr<McpSessionManager> manager, const McpSessionHandle session) noexcept
        : manager_(std::move(manager)), session_(session) {}
}  // namespace Horo::Mcp
