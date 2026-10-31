cmake_minimum_required(VERSION 3.24)
project(pentimento VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

# ---------------------------------------------------------------------------
# 依赖管理（单一来源：本文件）
#
# 1) Qt 6 是唯一的外部框架依赖，经系统包管理器安装、CMake find_package 发现：
#      Linux:  sudo apt install qt6-base-dev qt6-base-dev-tools libqt6sql6-sqlite
#      macOS:  brew install qt
#      Windows: 官方在线安装器（含 MSVC 2022 组件）或 vcpkg qtbase
#    缺组件时的报错定位见下方 COMPONENTS 清单；版本下限 6.4。
# 2) 其余三方 C++ 库一律走 FetchContent：在本节声明「名字 + 锁定的 tag」，
#    禁止手工 vendor 源码进仓、禁止在 CI/构建脚本里散装安装。
#    新增依赖时在此登记，并在 docs/zh/design/35-dependencies.md 记录选型理由。
# ---------------------------------------------------------------------------
find_package(Qt6 6.4 QUIET COMPONENTS Core Gui Widgets Sql)
if(NOT Qt6_FOUND)
    message(FATAL_ERROR
        "Qt 6.4+ 未找到（需要组件：Core Gui Widgets Sql）。"
        "Linux 请先: sudo apt install qt6-base-dev qt6-base-dev-tools libqt6sql6-sqlite；"
        "详见 docs/zh/design/35-dependencies.md")
endif()

include(FetchContent)
# FetchContent_MakeAvailable_once：<名字> <GIT_REPOSITORY ...> <GIT_TAG ...>
# 同名依赖只取第一次声明，防止传递依赖重复拉取。
function(fetch_dep name)
    if(TARGET ${name} OR DEFINED FETCH_DEP_${name}_DONE)
        return()
    endif()
    FetchContent_Declare(${name} ${ARGN})
    set(FETCH_DEP_${name}_DONE ON PARENT_SCOPE)
    FetchContent_MakeAvailable(${name})
endfunction()

# 当前无 FetchContent 依赖。示例（新增时取消注释并锁定 tag）：
# fetch_dep(miniz
#   GIT_REPOSITORY https://github.com/richgel999/miniz.git
#   GIT_TAG        3.0.2)

add_library(pentimento_core src/library.cpp src/library.h src/platform_immutable.cpp src/platform_immutable.h src/sentinel.cpp src/sentinel.h)
target_include_directories(pentimento_core PUBLIC src)
target_link_libraries(pentimento_core PUBLIC Qt6::Core Qt6::Sql)
add_executable(pentimento WIN32 MACOSX_BUNDLE src/main.cpp src/mainwindow.cpp src/mainwindow.h)
target_link_libraries(pentimento PRIVATE pentimento_core Qt6::Widgets)
set_target_properties(pentimento PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER org.pentimento.desktop)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(pentimento_tests tests/library_test.cpp)
    target_link_libraries(pentimento_tests PRIVATE pentimento_core Qt6::Test)
    add_test(NAME library_history COMMAND pentimento_tests)
    add_executable(pentimento_security_tests tests/security_test.cpp)
    target_link_libraries(pentimento_security_tests PRIVATE pentimento_core Qt6::Test)
    add_test(NAME security_sentinel COMMAND pentimento_security_tests)
endif()

include(GNUInstallDirs)
install(TARGETS pentimento BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
