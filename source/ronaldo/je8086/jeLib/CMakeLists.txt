cmake_minimum_required(VERSION 3.10)

project(jeLib)

add_library(jeLib STATIC)

set(SOURCES
	device.cpp device.h
	je8086.cpp je8086.h
	je8086devices.cpp je8086devices.h
	jeLcd.cpp jeLcd.h
	jemiditypes.h
	jePipeline.cpp jePipeline.h
	jeThread.cpp jeThread.h
	jetypes.h
	patch.cpp patch.h
	rom.cpp rom.h
	romloader.cpp romloader.h
	state.cpp state.h
	sysexRemoteControl.cpp sysexRemoteControl.h
)

target_sources(jeLib PRIVATE ${SOURCES})
source_group("source" FILES ${SOURCES})

set_property(TARGET jeLib PROPERTY FOLDER "JE8086")

target_link_libraries(jeLib PUBLIC synthLib hardwareLib rLib esp h8s)

target_include_directories(jeLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

#target_compile_options(jeLib PRIVATE -fsanitize=thread)
#target_link_options(jeLib PUBLIC -fsanitize=thread)

# jeDiag()'s thermal-state probe reaches NSProcessInfo through the ObjC runtime.
if(APPLE)
	target_link_libraries(jeLib PRIVATE objc)
endif()

# On-device diagnostics and the self-test chord. Off by default: they write a
# log file on every run. Invaluable while chasing scheduling behaviour on iOS,
# which is why they are kept rather than deleted.
option(${CMAKE_PROJECT_NAME}_JE_DEVICE_DIAGNOSTICS "Log per-second engine diagnostics on device" off)
if(${CMAKE_PROJECT_NAME}_JE_DEVICE_DIAGNOSTICS)
	target_compile_definitions(jeLib PRIVATE JE_DEVICE_DIAGNOSTICS=1)
endif()
