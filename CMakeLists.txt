cmake_minimum_required(VERSION 3.20)
project(kwin4_effect_smoothlamp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

list(APPEND CMAKE_AUTOMOC_MACRO_NAMES
    "KWIN_EFFECT_FACTORY"
    "KWIN_EFFECT_FACTORY_SUPPORTED"
    "KWIN_EFFECT_FACTORY_ENABLED"
    "KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED"
)

set(QT_MAJOR_VERSION 6)
find_package(ECM 6.22.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick Widgets)
find_package(KF6 REQUIRED COMPONENTS CoreAddons Config WindowSystem)
find_package(KWin REQUIRED)

# 1. KWin 宿主特效插件
add_library(kwin4_effect_smoothlamp MODULE
    smoothlamp.cpp
    main.cpp
)

set_target_properties(kwin4_effect_smoothlamp PROPERTIES PREFIX "")

target_compile_definitions(kwin4_effect_smoothlamp PRIVATE
    SMOOTHLAMP_SYS_SO_PATH="${KDE_INSTALL_FULL_LIBDIR}/smoothlamp_algo.so"
)

target_link_libraries(kwin4_effect_smoothlamp
    PRIVATE
        Qt6::Core
        Qt6::Gui
        KF6::CoreAddons
        KF6::ConfigCore
        KF6::WindowSystem
        KWin::kwin
        ${CMAKE_DL_LIBS}
)

install(TARGETS kwin4_effect_smoothlamp
    DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins
)

# 2. 独立热加载算法库 (平时单独重编它并拷入 ~/.local/lib/smoothlamp_algo.so 即可热更新)
add_library(smoothlamp_algo SHARED
    algo.cpp
)

set_target_properties(smoothlamp_algo PROPERTIES PREFIX "")

install(TARGETS smoothlamp_algo
    LIBRARY DESTINATION ${KDE_INSTALL_FULL_LIBDIR}
)