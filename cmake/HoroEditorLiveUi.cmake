# Development-only RCC++ integration. Upstream's top-level CMake project changes
# global output paths and adds examples, so compile only its private core here.
include("${CMAKE_CURRENT_LIST_DIR}/HoroRccppDevelopment.cmake")

# Internal built-in GUI integration. This does not grant extension packages access
# to the GUI backend or broaden any installed target's include surface.
set(HORO_GUI_LIVE_DIR "${PROJECT_BINARY_DIR}/gui-live")
add_library(HoroGuiLiveReloadInternal INTERFACE)
target_include_directories(HoroGuiLiveReloadInternal INTERFACE
    "${PROJECT_SOURCE_DIR}/src/editor/development/live_reload"
    "${PROJECT_SOURCE_DIR}/src/editor/development/live_reload/rccpp")
target_link_libraries(HoroGui PRIVATE HoroGuiLiveReloadInternal HoroThirdPartyRccpp)
target_compile_definitions(HoroEditorGuiLiveReloadHost PRIVATE HORO_EDITOR_LIVE_UI=1)
target_link_libraries(HoroEditorGuiLiveReloadHost PRIVATE HoroGuiLiveReloadInternal HoroThirdPartyRccpp HoroEngine::Gui)
foreach(target HoroGui HoroEditorGuiLiveReloadHost)
    target_include_directories(${target} SYSTEM PRIVATE
        "${HORO_RCCPP_ROOT}/RuntimeCompiler" "${HORO_RCCPP_ROOT}/RuntimeObjectSystem")
    if(MSVC)
        target_compile_options(${target} PRIVATE /FC /utf-8)
    endif()
endforeach()

# Each record generates a stateless RCC++ adapter. GUI state and class instances
# are borrowed from the original host; member calls explicitly bypass old vtables.
function(horo_register_live_gui name source symbol)
    cmake_parse_arguments(GUI "MEMBER" "BASE;HEADER" "SUPPORT" ${ARGN})
    get_property(index GLOBAL PROPERTY HORO_GUI_LIVE_COUNT)
    if(NOT index)
        set(index 0)
    endif()
    if(index GREATER_EQUAL 64)
        message(FATAL_ERROR "GUI live module capacity exceeded")
    endif()
    set(full_source "${PROJECT_SOURCE_DIR}/src/${source}")
    get_target_property(gui_sources HoroGui SOURCES)
    if(NOT source IN_LIST gui_sources)
        message(FATAL_ERROR "GUI live source ${source} must be owned by HoroGui")
    endif()
    set(tracked "${full_source}" ${HORO_GUI_LIVE_SHARED_SOURCES})
    foreach(support IN LISTS GUI_SUPPORT)
        file(RELATIVE_PATH relative_support "${PROJECT_SOURCE_DIR}/src" "${support}")
        if(relative_support IN_LIST gui_sources OR support IN_LIST gui_sources)
            list(APPEND tracked "${support}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES tracked)
    get_filename_component(source_name "${source}" NAME_WE)
    get_filename_component(source_directory "${full_source}" DIRECTORY)
    set(header "${source_directory}/${source_name}.h")
    if(GUI_HEADER)
        set(header "${PROJECT_SOURCE_DIR}/${GUI_HEADER}")
    elseif(NOT EXISTS "${header}")
        set(header "${PROJECT_SOURCE_DIR}/include/Horo/Editor/${source_name}.h")
    endif()
    if(NOT EXISTS "${header}")
        message(FATAL_ERROR "GUI live entry ${name} needs a declaring header")
    endif()
    set(tracking "")
    set(number 0)
    foreach(path IN LISTS tracked)
        # RCC++ compares tracking entries to native watcher paths verbatim.
        file(TO_NATIVE_PATH "${path}" native_path)
        string(REPLACE "\\" "\\\\" native_path "${native_path}")
        string(APPEND tracking "
namespace {
    enum { GuiTracking${number} = __COUNTER__ - COUNTER_OFFSET };
    template<> RuntimeTackingInfo GetTrackingInfoFunc<GuiTracking${number} + 1>(size_t n) {
        if(n == GuiTracking${number}) {
            auto info = RuntimeTackingInfo::GetNULL();
            info.includeFile = \"${native_path}\";
            info.sourceDependencyInfo = { \"${native_path}\", \".cpp\", nullptr };
            return info;
        }
        return n < GuiTracking${number} ? GetTrackingInfoFunc<GuiTracking${number}>(n) : RuntimeTackingInfo::GetNULL();
    }
}
")
        math(EXPR number "${number} + 1")
    endforeach()
    if(GUI_MEMBER)
        string(REGEX REPLACE "::[^:]+$" "" class "${symbol}")
        string(REGEX REPLACE ".*::" "" method "${symbol}")
        if(GUI_BASE)
            set(route "GuiLive::TypeId(typeid(${class}))")
            set(binding "GuiLive::Adapter<GuiLive::MethodSignature<&${GUI_BASE}::${method}>, [](${GUI_BASE} &self, auto &&...args) -> decltype(auto) { using Owner = ${class}; return static_cast<Owner &>(self).Owner::${method}(std::forward<decltype(args)>(args)...); }>::Bind(signature)")
        else()
            set(route "GuiLive::EntryId<&${symbol}>()")
            set(binding "GuiLive::Adapter<GuiLive::MethodSignature<&${symbol}>, [](auto &self, auto &&...args) -> decltype(auto) { using Owner = ${class}; return self.Owner::${method}(std::forward<decltype(args)>(args)...); }>::Bind(signature)")
        endif()
    else()
        if(GUI_BASE)
            message(FATAL_ERROR "GUI live BASE requires a MEMBER entry")
        endif()
        set(route "GuiLive::EntryId<&${symbol}>()")
        set(binding "GuiLive::Adapter<decltype(${symbol}), &${symbol}>::Bind(signature)")
    endif()
    set(wrapper "${HORO_GUI_LIVE_DIR}/GuiLive_${name}.cpp")
    file(GENERATE OUTPUT "${wrapper}" CONTENT "
#include \"GuiRuntimeBridge.h\"
#include \"ISimpleSerializer.h\"
#include \"ObjectInterfacePerModule.h\"
${tracking}
#if defined(HORO_GUI_RUNTIME_COMPILATION)
#include \"${header}\"
#endif
class HoroGui${name} : public HoroGuiModule {
public:
    void Init(bool) override { PerModuleInterface::g_pSystemTable->modules[${index}] = this; }
    void Serialize(ISimpleSerializer *) override {}
    Horo::Editor::GuiLive::Binding Find(Horo::Editor::GuiLive::Id route, Horo::Editor::GuiLive::Id signature) const noexcept override {
#if defined(HORO_GUI_RUNTIME_COMPILATION)
        using namespace Horo::Editor;
        if(route == ${route}) return ${binding};
#endif
        return {};
    }
};
REGISTERSINGLETON(HoroGui${name}, true);
void HoroGuiAnchor${name}() {}
")
    target_sources(HoroGui PRIVATE "${wrapper}")
    set_property(GLOBAL APPEND_STRING PROPERTY HORO_GUI_LIVE_DECLS "void HoroGuiAnchor${name}();\n")
    set_property(GLOBAL APPEND_STRING PROPERTY HORO_GUI_LIVE_LINKS "    HoroGuiAnchor${name}();\n")
    math(EXPR index "${index} + 1")
    set_property(GLOBAL PROPERTY HORO_GUI_LIVE_COUNT ${index})
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/HoroGuiLiveSources.cmake")
get_property(declarations GLOBAL PROPERTY HORO_GUI_LIVE_DECLS)
get_property(links GLOBAL PROPERTY HORO_GUI_LIVE_LINKS)
file(GENERATE OUTPUT "${HORO_GUI_LIVE_DIR}/GuiModules.cpp" CONTENT
    "#include \"GuiRuntimeBridge.h\"\n${declarations}\nvoid Horo::Editor::Development::LinkGuiModules() {\n${links}}\n")
target_sources(HoroGui PRIVATE "${HORO_GUI_LIVE_DIR}/GuiModules.cpp")
file(GENERATE OUTPUT "${HORO_GUI_LIVE_DIR}/includes.txt" CONTENT
    "$<JOIN:$<TARGET_PROPERTY:HoroGui,INCLUDE_DIRECTORIES>,\n>\n${HORO_RCCPP_ROOT}/RuntimeCompiler\n${HORO_RCCPP_ROOT}/RuntimeObjectSystem\n")
