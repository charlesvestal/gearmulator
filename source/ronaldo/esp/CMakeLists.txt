cmake_minimum_required(VERSION 3.10)

project(esp)

add_library(esp STATIC)

set(SOURCES
	esp.hpp
	esp_jit.cpp
	esp_jit.h
	esp_jit_arm64.cpp
	esp_jit_arm64.h
	esp_jit_types.h
	esp_jit_x64.cpp
	esp_jit_x64.h
	esp_jit_x64_pool.cpp
	esp_jit_x64_pool.h
	esp_jit_x64_types.h
	esp_opt.hpp
)

# The subset that carries no generated code and no asmjit dependency.
set(SOURCES_NOJIT
	esp.hpp
	esp_opt.hpp
	esp_nojit.cpp
)

source_group("source" FILES ${SOURCES})

set_property(TARGET esp PROPERTY FOLDER "Ronaldo")

# The ESP JIT is the only thing here that needs an executable page. A platform
# that forbids one (iOS/iPadOS) runs ESP::step_cores() instead -- the original
# interpreter -- selected at compile time so nothing tests a flag per sample.
option(${CMAKE_PROJECT_NAME}_JE_NO_JIT "Run the ESP cores interpreted, no JIT" ${JE_NO_JIT_DEFAULT})
if(${CMAKE_PROJECT_NAME}_JE_NO_JIT)
	target_compile_definitions(esp PUBLIC JE_ESP_NO_JIT=1)
	# The emitters and asmjit itself are not built: asmjit does not compile for
	# iOS (virtmem.cpp), and a build that cannot JIT should not carry one.
	target_sources(esp PRIVATE ${SOURCES_NOJIT})
	message(STATUS "JE-8086: ESP JIT disabled, cores run interpreted")
else()
	target_sources(esp PRIVATE ${SOURCES})
	target_link_libraries(esp PUBLIC asmjit)
endif()

target_include_directories(esp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
