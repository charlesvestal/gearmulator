cmake_minimum_required(VERSION 3.10)

project(dsp56kBench)

# One executable per synth, all from the same source. They cannot share a binary:
# each synth's microcontroller defines the Musashi callbacks (m68k_read_memory_16
# and friends) as global symbols, so linking two synth libs together is 14
# duplicate symbols. That is also why every synth here has its own test console.
function(addDsp56kBench _name _lib _define)
	if(NOT TARGET ${_lib})
		return()
	endif()

	add_executable(${_name} dsp56kBench.cpp)
	source_group("source" FILES dsp56kBench.cpp)
	target_link_libraries(${_name} PUBLIC baseLib synthLib ${_lib})
	target_compile_definitions(${_name} PRIVATE ${_define}=1)
	set_property(TARGET ${_name} PROPERTY FOLDER "Tools")
endfunction()

addDsp56kBench(dsp56kBench_virus	virusLib	BENCH_HAS_VIRUS)
addDsp56kBench(dsp56kBench_xt		xtLib		BENCH_HAS_XT)
addDsp56kBench(dsp56kBench_mq		mqLib		BENCH_HAS_MQ)
addDsp56kBench(dsp56kBench_n2x		n2xLib		BENCH_HAS_N2X)

# Convenience target so `--target dsp56kBench` still builds everything available.
add_custom_target(dsp56kBench)
foreach(t dsp56kBench_virus dsp56kBench_xt dsp56kBench_mq dsp56kBench_n2x)
	if(TARGET ${t})
		add_dependencies(dsp56kBench ${t})
	endif()
endforeach()
