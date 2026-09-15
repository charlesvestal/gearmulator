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

	# iOS: the bench only reaches a device as a signed .app. Bundle the ROMs as
	# resources -- main() chdirs into them -- and name the synth at compile time
	# because devicectl launches with no argv.
	if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
		string(REPLACE "dsp56kBench_" "" _synthName ${_name})
		target_compile_definitions(${_name} PRIVATE BENCH_IOS_SYNTH="${_synthName}")

		# A plain main() is SIGKILLed after ~20 s on iOS; iosmain.mm makes this a
		# real UIApplication and runs the bench on a background queue instead.
		target_sources(${_name} PRIVATE iosmain.mm)
		set_source_files_properties(iosmain.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
		target_link_libraries(${_name} PUBLIC "-framework CoreFoundation" "-framework UIKit" "-framework Foundation")

		set_target_properties(${_name} PROPERTIES
			MACOSX_BUNDLE TRUE
			MACOSX_BUNDLE_GUI_IDENTIFIER com.charlesvestal.${_name}
			MACOSX_BUNDLE_BUNDLE_NAME ${_name}
			XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.charlesvestal.${_name}
			XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
			XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")

		if(DSP56KBENCH_IOS_ROMS AND EXISTS ${DSP56KBENCH_IOS_ROMS})
			file(GLOB _benchRoms "${DSP56KBENCH_IOS_ROMS}/*")
			if(_benchRoms)
				target_sources(${_name} PRIVATE ${_benchRoms})
				set_source_files_properties(${_benchRoms} PROPERTIES
					MACOSX_PACKAGE_LOCATION Resources
					HEADER_FILE_ONLY TRUE)
			endif()
		endif()
	endif()
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
