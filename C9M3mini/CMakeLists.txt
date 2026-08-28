cmake_minimum_required(VERSION 3.16)
project(PiCameraApp VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fpermissive")

# Find Qt 6 packages
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent Multimedia MultimediaWidgets)
find_package(OpenCV REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)

add_executable(PiCameraApp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/MainWindow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/AlbumWidget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/VideoHudWidget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SettingsMenu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/WifiShareWidget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/CameraEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/VideoRecorder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hardware/HardwareManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hardware/PioTriggerController.cpp
)

# 挂载 piolib
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/hardware/rpi_utils/piolib)

# 定义 pioasm 编译命令
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/imx296_trigger.pio.h
    COMMAND pioasm -o c-sdk ${CMAKE_CURRENT_SOURCE_DIR}/src/hardware/imx296_trigger.pio ${CMAKE_CURRENT_BINARY_DIR}/imx296_trigger.pio.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/hardware/imx296_trigger.pio
)

# 确保 PioTriggerController.cpp 在编译前生成 pio.h
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/hardware/PioTriggerController.cpp 
    PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/imx296_trigger.pio.h)

target_include_directories(PiCameraApp PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(PiCameraApp PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    ${OpenCV_LIBS}
    PkgConfig::LIBCAMERA
    pio
    pthread
)
set_target_properties(PiCameraApp PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
    AUTOGEN_ORIGIN_DEPENDS OFF
)
