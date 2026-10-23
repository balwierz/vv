# vvg — Qt6 graphical frontend. Linked against libvvcore (the headless
# reader core); the KF6 thumbnailer / metadata plugins (later stages) link
# the same core. Built only when -DVV_BUILD_GUI=ON.

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)   # compile vvg.qrc (the app icon, the field reference)

# The table widgets vvg shares with the KDE part and the Dolphin Properties
# page: the model, the typed header, the table view with frozen columns, the
# sequence colours, the tab pane, opening files, column widths and
# VvTablePane. Position-independent so a plugin .so can link it.
add_library(vvgui_core STATIC
    arrowtablemodel.cpp
    typedheader.cpp
    tableview.cpp
    sequencedelegate.cpp
    tabpane.cpp
    loadsources.cpp
    columnwidths.cpp
    tablepane.cpp
)
set_target_properties(vvgui_core PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC ON)
target_include_directories(vvgui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vvgui_core PUBLIC vvcore Qt6::Widgets Qt6::Concurrent)

add_executable(vvg
    main.cpp
    jsontreemodel.cpp
    vvcommand.cpp
    columnpanel.cpp
    contextbar.cpp
    shortcutsdialog.cpp
    helpdialog.cpp
    actionpalette.cpp
    vvg.qrc
)
target_include_directories(vvg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
  target_sources(vvg PRIVATE "${VV_WIN_RC}")   # manifest, icon, version (top level)
  set_target_properties(vvg PROPERTIES WIN32_EXECUTABLE ON)   # no console window
endif()
target_link_libraries(vvg PRIVATE vvgui_core vvcore Qt6::Widgets Qt6::Concurrent)

message(STATUS "GUI target  : vvg (Qt6 ${Qt6_VERSION})")

install(TARGETS vvg RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# KDE Plasma integration: thumbnailer + metadata plugins + MIME/.desktop.
add_subdirectory(kde)
