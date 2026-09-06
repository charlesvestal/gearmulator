set(TUS_CHANGELOG_SOURCE ${CMAKE_SOURCE_DIR}/doc/changelog.txt)
set(TUS_CHANGELOG_OUT_DIR ${CMAKE_SOURCE_DIR}/doc/changelog_split)
set(TUS_CHANGELOG_DEPENDS_FILE ${CMAKE_SOURCE_DIR}/doc/changelog_split/changelog_Osirus.txt)

# Cross-compiling (iOS): changelogGenerator is built FOR THE TARGET and cannot
# run on the build host. The split changelogs are checked in, so use them as-is.
if(CMAKE_CROSSCOMPILING)
	add_custom_target(tus_genChangelogs)
	macro(tus_registerChangelog targetName)
	endmacro()
	return()
endif()

add_custom_command(
    OUTPUT ${TUS_CHANGELOG_DEPENDS_FILE}
    COMMAND $<TARGET_FILE:changelogGenerator>
            -i ${TUS_CHANGELOG_SOURCE}
			-o ${TUS_CHANGELOG_OUT_DIR}
    DEPENDS
        changelogGenerator
        "${TUS_CHANGELOG_SOURCE}"
    COMMENT "Generating per-product changelogs"
)

add_custom_target(tus_genChangelogs
	DEPENDS ${TUS_CHANGELOG_DEPENDS_FILE}
)

macro(tus_registerChangelog targetName)
	add_dependencies(${targetName} tus_genChangelogs)
endmacro()
