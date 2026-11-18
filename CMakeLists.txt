cmake_minimum_required(VERSION 3.16)
project(kwin4_effect_smoothlamp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)

find_package(Qt5 REQUIRED COMPONENTS Core Gui)
find_package(KF5 REQUIRED COMPONENTS CoreAddons Config WindowSystem)
find_package(XCB REQUIRED COMPONENTS XCB)
find_package(epoxy REQUIRED)
find_package(KWinEffects REQUIRED)

add_library(kwin4_effect_smoothlamp MODULE
    smoothlamp.cpp
    main.cpp
)

set_target_properties(kwin4_effect_smoothlamp PROPERTIES PREFIX "")

target_link_libraries(kwin4_effect_smoothlamp
    PRIVATE
        Qt5::Core
        Qt5::Gui
        KF5::CoreAddons
        KF5::ConfigCore
        KF5::WindowSystem
        KWinEffects::kwineffects
        KWinEffects::kwinglutils
        ${CMAKE_DL_LIBS}
)

install(TARGETS kwin4_effect_smoothlamp
    DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins
)