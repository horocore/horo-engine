#include "Horo/Application/CodeQueryStoreProviders.h"
#include "Horo/Application/ProjectCodeQuery.h"
#include "Horo/Mcp/ProjectCodeQueryTools.h"
#include "Horo/Platform/NativeProjectReadFiles.h"

#include <type_traits>

static_assert(std::is_same_v<Horo::Application::CodeQueryTextSnapshot, Horo::Platform::ProjectReadTextSnapshot>);
static_assert(std::is_same_v<decltype(&Horo::Application::ProjectCodeQuery::Query),
                             Horo::Result<Horo::Application::CodeQueryPage> (Horo::Application::ProjectCodeQuery::*)(
                                 const Horo::Application::CodeQueryRequest &, const Horo::Application::CodeQueryContext &) const>);
static_assert(Horo::Application::CodeQueryLimits{}.maximumPageItems == 128);

static_assert(std::is_same_v<decltype(&Horo::Platform::IProjectReadFiles::Inventory),
                             Horo::Result<Horo::Platform::ProjectInventory> (Horo::Platform::IProjectReadFiles::*)(
                                 std::string_view, Horo::Platform::ProjectInventoryDepth, const Horo::Platform::ProjectReadContext &,
                                 const Horo::Platform::ProjectInventoryLimits &) const>);
static_assert(Horo::Platform::ProjectReadLimits{}.maximumEntries == 4096);
static_assert(Horo::Platform::ProjectInventoryLimits{}.maximumEntries == (1U << 20U));
