cmake_minimum_required(VERSION 3.10)

project(virusBench)

add_executable(virusBench)

set(SOURCES
	virusBench.cpp
)

target_sources(virusBench PRIVATE ${SOURCES})
source_group("source" FILES ${SOURCES})

target_link_libraries(virusBench PUBLIC virusLib)

set_property(TARGET virusBench PROPERTY FOLDER "Virus")
