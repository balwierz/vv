# vvg — Qt6 graphical frontend. Linked against libvvcore (the headless
# reader core); the KF6 thumbnailer / metadata plugins (later stages) link
# the same core. Built only when -DVV_BUILD_GUI=ON.

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)

add_executable(vvg
    main.cpp
    arrowtablemodel.cpp
)
target_include_directories(vvg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vvg PRIVATE vvcore Qt6::Widgets)

message(STATUS "GUI target  : vvg (Qt6 ${Qt6_VERSION})")

install(TARGETS vvg RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
