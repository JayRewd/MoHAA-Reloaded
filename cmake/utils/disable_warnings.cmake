include_guard(GLOBAL)

function(disable_warnings)
    set(SOURCES ${ARGN})

    foreach(FILE IN LISTS SOURCES)
        set_source_files_properties(${FILE} PROPERTIES COMPILE_FLAGS -w)
    endforeach()
endfunction()
