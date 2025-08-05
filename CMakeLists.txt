# --- Detect if the plugin is build out of tree or not ---
if(CMAKE_PROJECT_NAME STREQUAL "obs-studio")
  set(BUILD_OUT_OF_TREE OFF)
else()
  set(BUILD_OUT_OF_TREE ON)
  cmake_minimum_required(VERSION 3.18)
endif()

project(audio-monitor VERSION 0.9.2)
set(PROJECT_FULL_NAME "Audio Monitor")

# Set new UUIDs when you start to create a new plugin.
set(MACOS_PACKAGE_UUID "A8B95053-211E-4AB1-B4FB-6CB7BA323886")
set(MACOS_INSTALLER_UUID "B7AAB6CD-A0A0-47E6-9BC6-76304DFB42EF")

add_library(${PROJECT_NAME} MODULE)

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

if(BUILD_OUT_OF_TREE)
	find_package(libobs REQUIRED)
	find_package(obs-frontend-api REQUIRED)
	include(cmake/ObsPluginHelpers.cmake)
	find_qt(COMPONENTS Widgets COMPONENTS_LINUX Gui)
	set(OBS_FRONTEND_API_NAME "obs-frontend-api")
else()
	if(OBS_VERSION VERSION_GREATER_EQUAL 30.1.0)
		find_package(Qt6 COMPONENTS Core Widgets)
	else()
		find_qt(COMPONENTS Widgets COMPONENTS_LINUX Gui)
	endif()
	set(OBS_FRONTEND_API_NAME "frontend-api")
endif()

if(OS_WINDOWS)
	get_filename_component(ISS_FILES_DIR "${CMAKE_BINARY_DIR}\\..\\package" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_FILES_DIR}" ISS_FILES_DIR)
	get_filename_component(ISS_PACKAGE_DIR "${CMAKE_PACKAGE_PREFIX}\\.." ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_PACKAGE_DIR}" ISS_PACKAGE_DIR)
	get_filename_component(ISS_SOURCE_DIR "${PROJECT_SOURCE_DIR}" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_SOURCE_DIR}" ISS_SOURCE_DIR)
	configure_file("installer.iss.in"
			"${PROJECT_BINARY_DIR}/installer.iss"
		)
	configure_file(${CMAKE_CURRENT_SOURCE_DIR}/resource.rc.in ${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.rc)
	target_sources(${PROJECT_NAME} PRIVATE 
		audio-monitor-win.c
		audio-monitor-win.h
		${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.rc)
	if(MSVC)
		target_link_libraries(${PROJECT_NAME}
		w32-pthreads
		WS2_32)
	endif()
elseif(OS_MACOS)
	set(MACOS_BUNDLEID "com.exeldro.${PROJECT_NAME}")
	set(MACOSX_PLUGIN_GUI_IDENTIFIER "${MACOS_BUNDLEID}")
	set(MACOSX_PLUGIN_BUNDLE_VERSION "${PROJECT_VERSION}")
	set(MACOSX_PLUGIN_SHORT_VERSION_STRING "1")
	configure_file(cmake/bundle/macos/installer-macos.pkgproj.in ${CMAKE_BINARY_DIR}/installer-macos.generated.pkgproj)
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall)

	find_library(COREFOUNDATION CoreFoundation)
	mark_as_advanced(COREFOUNDATION)
	include_directories(${COREFOUNDATION})

	find_library(AUDIOTOOLBOX AudioToolbox)
	mark_as_advanced(AUDIOTOOLBOX)
	include_directories(${AUDIOTOOLBOX})

	target_link_libraries(${PROJECT_NAME}
	${COREFOUNDATION}
	${AUDIOTOOLBOX})
	target_sources(${PROJECT_NAME} PRIVATE 
		audio-monitor-mac.c
		audio-monitor-mac.h)
elseif(UNIX)
	find_library(PULSEAUDIO_LIBRARY NAMES pulse)

	target_link_libraries(${PROJECT_NAME}
	${PULSEAUDIO_LIBRARY})
	target_sources(${PROJECT_NAME} PRIVATE 
		audio-monitor-pulse.c
		audio-monitor-pulse.h)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)

target_link_libraries(${PROJECT_NAME}
		OBS::${OBS_FRONTEND_API_NAME}
		Qt::Widgets
		OBS::libobs)

if(BUILD_OUT_OF_TREE)
	if(NOT LIB_OUT_DIR)
		set(LIB_OUT_DIR "/lib/obs-plugins")
	endif()
	if(NOT DATA_OUT_DIR)
		set(DATA_OUT_DIR "/share/obs/obs-plugins/${PROJECT_NAME}")
	endif()
	set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
	install(TARGETS ${PROJECT_NAME}
		LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/${LIB_OUT_DIR})
	install(DIRECTORY data/locale
		DESTINATION ${CMAKE_INSTALL_PREFIX}/${DATA_OUT_DIR})
	setup_plugin_target(${PROJECT_NAME})
else()
	target_include_directories(${PROJECT_NAME} PRIVATE
		"${CMAKE_SOURCE_DIR}/UI/obs-frontend-api")
	if(OBS_CMAKE_VERSION VERSION_GREATER_EQUAL 3.0.0)
		set_target_properties_obs(${PROJECT_NAME} PROPERTIES FOLDER "plugins/exeldro" PREFIX "")
	else()
		set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER "plugins/exeldro")
		setup_plugin_target(${PROJECT_NAME})
	endif()
endif()
