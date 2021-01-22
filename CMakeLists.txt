project(audio-monitor VERSION 0.1.1)
set(PROJECT_FULL_NAME "Audio Monitor")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/version.h)

set(audio-monitor_HEADERS
	audio-monitor.h
	version.h)
set(audio-monitor_SOURCES
	audio-monitor.c)

if(WIN32)
	get_filename_component(ISS_FILES_DIR "${CMAKE_BINARY_DIR}\\..\\package" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_FILES_DIR}" ISS_FILES_DIR)
	get_filename_component(ISS_PACKAGE_DIR "${CMAKE_PACKAGE_PREFIX}\\.." ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_PACKAGE_DIR}" ISS_PACKAGE_DIR)
	get_filename_component(ISS_SOURCE_DIR "${PROJECT_SOURCE_DIR}" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_SOURCE_DIR}" ISS_SOURCE_DIR)
	configure_file("installer.iss.in"
			"${PROJECT_BINARY_DIR}/installer.iss"
		)
	configure_file(resource.rc.in audio-monitor.rc)
	list(APPEND audio-monitor_SOURCES
		audio-monitor.rc)
	if(MSVC)
		set(audio-monitor_PLATFORM_DEPS
		w32-pthreads)
	endif()
elseif(APPLE)
	find_library(COREFOUNDATION CoreFoundation)
	mark_as_advanced(COREFOUNDATION)
	include_directories(${COREFOUNDATION})

	find_library(AUDIOTOOLBOX AudioToolbox)
	mark_as_advanced(AUDIOTOOLBOX)
	include_directories(${AUDIOTOOLBOX})

	set(audio-monitor_PLATFORM_DEPS
	${COREFOUNDATION}
	${AUDIOTOOLBOX})
elseif(UNIX)

endif()

add_library(audio-monitor MODULE
	${audio-monitor_HEADERS}
	${audio-monitor_SOURCES})
target_link_libraries(audio-monitor
		libobs
		${audio-monitor_PLATFORM_DEPS})

set_target_properties(audio-monitor PROPERTIES FOLDER "plugins/exeldro")
install_obs_plugin_with_data(audio-monitor data)
