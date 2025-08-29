# --- Detect if the plugin is build out of tree or not ---
if(CMAKE_PROJECT_NAME STREQUAL "obs-studio")
  set(BUILD_OUT_OF_TREE OFF)
else()
  set(BUILD_OUT_OF_TREE ON)
  cmake_minimum_required(VERSION 3.16...3.26)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${PROJECT_NAME} MODULE)

target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs)

if(BUILD_OUT_OF_TREE)
  find_package(libobs REQUIRED)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE OBS::frontend-api)
endif()

find_package(Qt6 COMPONENTS Widgets Core)
if(BUILD_OUT_OF_TREE)
  if(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
    find_package(Qt6 REQUIRED Gui)
  endif()
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE Qt::Core Qt::Widgets)

if((OS_LINUX OR OS_FREEBSD OR OS_OPENBSD) AND Qt_VERSION VERSION_LESS "6.9.0")
	target_link_libraries(${PROJECT_NAME} PRIVATE Qt::GuiPrivate)
endif()

target_compile_options(
${PROJECT_NAME} PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header
                                -Wno-comma>)
set_target_properties(${PROJECT_NAME} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(
${PROJECT_NAME}
PROPERTIES AUTOMOC ON
            AUTOUIC ON
            AUTORCC ON)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/version.h)

target_sources(${PROJECT_NAME} PRIVATE
	audio-monitor-filter.c
	audio-monitor-dock.cpp
	audio-control.cpp
	audio-output-control.cpp
	volume-meter.cpp
	utils.cpp
	audio-monitor-filter.h
	audio-monitor-dock.hpp
	audio-control.hpp
	audio-output-control.hpp
	volume-meter.hpp
	utils.hpp
	version.h)

if(OS_WINDOWS)
	configure_file(cmake/windows/resources/installer-Windows.iss.in "${CMAKE_CURRENT_BINARY_DIR}/installer-Windows.generated.iss")
	if(MSVC)
		target_link_libraries(${PROJECT_NAME} PRIVATE WS2_32)
	endif()
	target_sources(${PROJECT_NAME} PRIVATE 
		audio-monitor-win.c
		audio-monitor-win.h)
elseif(OS_MACOS)
	find_library(COREFOUNDATION CoreFoundation)
	mark_as_advanced(COREFOUNDATION)
	include_directories(${COREFOUNDATION})

	find_library(AUDIOTOOLBOX AudioToolbox)
	mark_as_advanced(AUDIOTOOLBOX)
	include_directories(${AUDIOTOOLBOX})

	target_link_libraries(${PROJECT_NAME} PRIVATE
	${COREFOUNDATION}
	${AUDIOTOOLBOX})
	target_sources(${PROJECT_NAME} PRIVATE 
		audio-monitor-mac.c
		audio-monitor-mac.h)
elseif(UNIX)
	find_library(PULSEAUDIO_LIBRARY NAMES pulse libpulse REQUIRED)

	target_link_libraries(${PROJECT_NAME} PRIVATE
	${PULSEAUDIO_LIBRARY})
	target_sources(${PROJECT_NAME} PRIVATE 
		audio-monitor-pulse.c
		audio-monitor-pulse.h)
endif()

if(BUILD_OUT_OF_TREE)
	set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
else()
	set_target_properties_obs(${PROJECT_NAME} PROPERTIES FOLDER "plugins/exeldro" PREFIX "")
endif()
