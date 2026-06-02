# KDE Plasma integration: shared-mime-info, .desktop, AppStream, icon, and
# two KF6 plugins (Dolphin thumbnailer + Information-Panel metadata) backed by
# libvvcore. Each plugin is optional — built only when its KF6 component is
# found — so a plain-Qt GUI still builds without KF6.

find_package(ECM QUIET NO_MODULE)
if(ECM_FOUND)
  list(APPEND CMAKE_MODULE_PATH ${ECM_KDE_MODULE_DIR} ${ECM_MODULE_PATH})
  include(KDEInstallDirs)
endif()
if(NOT DEFINED KDE_INSTALL_PLUGINDIR)
  set(KDE_INSTALL_PLUGINDIR "${CMAKE_INSTALL_LIBDIR}/qt6/plugins")
endif()

# ── KF6-free cores (testable, reused by both plugins) ────────────────────────
add_library(vvkde_core STATIC thumbrender.cpp metaprobe.cpp)
set_target_properties(vvkde_core PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF)
target_include_directories(vvkde_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vvkde_core PUBLIC vvcore Qt6::Gui)

# ── Headless test harness (validates the cores without a display) ────────────
add_executable(vvkdetest vvkdetest.cpp)
target_link_libraries(vvkdetest PRIVATE vvkde_core Qt6::Gui)

# The KF6 plugins are shared objects, so every transitive dependency must be
# position-independent. The vendored ../static-libs / static-Arrow archives
# (used for the portable CLI binary) are NOT compiled -fPIC and cannot be
# linked into a .so. KDE plugins are therefore a shared-deps (distro) build
# artifact; skip them in the static configuration. The KF6-free cores and the
# headless test still build everywhere.
set(_vv_plugins_ok TRUE)
if(_prefer_static)
  set(_vv_plugins_ok FALSE)
  message(STATUS "GUI plugins : SKIPPED (static-libs build — .so needs PIC deps)")
endif()

# ── Dolphin thumbnailer (KIO::ThumbnailCreator) ──────────────────────────────
find_package(KF6 COMPONENTS KIO CoreAddons QUIET)
if(_vv_plugins_ok AND KF6KIO_FOUND AND KF6CoreAddons_FOUND)
  add_library(vvthumbnail MODULE vvthumbnail.cpp)
  # KF6 plugins are discovered by JSON metadata, not filename, and drop the
  # "lib" prefix by convention (audiothumbnail.so, etc.).
  set_target_properties(vvthumbnail PROPERTIES AUTOMOC ON PREFIX "")
  target_link_libraries(vvthumbnail PRIVATE vvkde_core KF6::KIOGui KF6::CoreAddons)
  install(TARGETS vvthumbnail
          DESTINATION ${KDE_INSTALL_PLUGINDIR}/kf6/thumbcreator)
  message(STATUS "GUI plugin  : vvthumbnail (KIO thumbnailer)")
else()
  message(STATUS "GUI plugin  : vvthumbnail SKIPPED (KF6 KIO/CoreAddons not found)")
endif()

# ── Information-Panel metadata (KFileMetaData::ExtractorPlugin) ───────────────
find_package(KF6FileMetaData QUIET)
if(_vv_plugins_ok AND KF6FileMetaData_FOUND)
  add_library(vvextractor MODULE vvextractor.cpp)
  set_target_properties(vvextractor PROPERTIES AUTOMOC ON PREFIX "")
  target_link_libraries(vvextractor PRIVATE vvkde_core KF6::FileMetaData)
  install(TARGETS vvextractor
          DESTINATION ${KDE_INSTALL_PLUGINDIR}/kf6/kfilemetadata)
  message(STATUS "GUI plugin  : vvextractor (KFileMetaData)")
else()
  message(STATUS "GUI plugin  : vvextractor SKIPPED (KF6FileMetaData not found)")
endif()

# ── Desktop integration assets ───────────────────────────────────────────────
install(FILES vv-formats.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
install(FILES org.vv.Viewer.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES org.vv.Viewer.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES icons/vv.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
