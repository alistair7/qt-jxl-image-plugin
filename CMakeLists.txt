cmake_minimum_required(VERSION 3.14)

project(qt-jxl-image-plugin LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-missing-field-initializers")

find_package(QT NAMES Qt6 Qt5 COMPONENTS Gui REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Gui REQUIRED)

add_library(qt-jxl-image-plugin SHARED
  qjxlhandler.cpp
  qjxlhandler.h
  qjxlplugin.cpp
  qt-jxl-image-plugin.json
)

target_link_libraries(qt-jxl-image-plugin PRIVATE Qt${QT_VERSION_MAJOR}::Gui -ljxl_threads -ljxl)

target_compile_definitions(qt-jxl-image-plugin PRIVATE QTJXLIMAGEPLUGIN_LIBRARY)
