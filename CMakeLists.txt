cmake_minimum_required(VERSION 3.0)
project(lightmqtt LANGUAGES C)

include(GNUInstallDirs)

file(GLOB lib_srcs
	src/lmqtt_client.c src/lmqtt_packet.c src/lmqtt_store.c src/lmqtt_time.c
)

add_library(lightmqtt ${lib_srcs})
target_include_directories(lightmqtt PUBLIC include)

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
