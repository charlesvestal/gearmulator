cmake_minimum_required(VERSION 3.15)

add_subdirectory(common)

# iOS cannot map executable pages, so a build for it is interpreted by default.
if(IOS)
	set(JE_NO_JIT_DEFAULT ON)
else()
	set(JE_NO_JIT_DEFAULT OFF)
endif()

if(${CMAKE_PROJECT_NAME}_SYNTH_JE8086)
	add_subdirectory(esp)
	add_subdirectory(je8086)
endif()
