cmake_minimum_required(VERSION 3.0)
project(lightmqtt LANGUAGES C)

file(GLOB lib_srcs
	src/lmqtt_client.c src/lmqtt_packet.c src/lmqtt_store.c src/lmqtt_time.c
)
file(GLOB lib_headers
	include/lightmqtt/client.h
	include/lightmqtt/core.h
	include/lightmqtt/packet.h
	include/lightmqtt/store.h
	include/lightmqtt/time.h
	include/lightmqtt/types.h
)

add_library(lightmqtt ${lib_srcs})
add_library(lightmqtt::lightmqtt ALIAS lightmqtt)

target_include_directories(lightmqtt PUBLIC
	$<INSTALL_INTERFACE:include>
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
set_target_properties(lightmqtt PROPERTIES PUBLIC_HEADER "${lib_headers}")

add_executable(pingpong examples/pingpong.c examples/helpers.c)
target_link_libraries(pingpong lightmqtt)

add_executable(reconnect examples/reconnect.c examples/helpers.c)
target_link_libraries(reconnect lightmqtt)

if(WIN32)
target_compile_definitions(lightmqtt PUBLIC
	_WIN32_WINNT=0x600 _WIN32_WINDOWS=0x600 WINVER=0x600
)
target_link_libraries(pingpong ws2_32)
target_link_libraries(reconnect ws2_32)
endif()

include(GNUInstallDirs)
set(INSTALL_CONFIGDIR ${CMAKE_INSTALL_LIBDIR}/cmake/lightmqtt)

install(TARGETS lightmqtt
	EXPORT lightmqtt-targets
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lightmqtt
)

# Export the targets to a script
install(EXPORT lightmqtt-targets
	FILE lightmqttTargets.cmake
	NAMESPACE lightmqtt::
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lightmqtt
)

# Generate config file
include(CMakePackageConfigHelpers)
configure_package_config_file(
	${CMAKE_CURRENT_LIST_DIR}/cmake/lightmqttConfig.cmake.in
	${CMAKE_CURRENT_BINARY_DIR}/lightmqttConfig.cmake
	INSTALL_DESTINATION ${INSTALL_CONFIGDIR}
)

# Install config file
install(FILES
	${CMAKE_CURRENT_BINARY_DIR}/lightmqttConfig.cmake
	DESTINATION ${INSTALL_CONFIGDIR}
)

export(
	EXPORT lightmqtt-targets
	FILE ${CMAKE_CURRENT_BINARY_DIR}/lightmqttTargets.cmake
	NAMESPACE lightmqtt::
)

# Register package in user's package registry
export(PACKAGE lightmqtt)
