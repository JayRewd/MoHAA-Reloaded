# Apple Clang compiler specific settings

if(NOT CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
    return()
endif()

set(CMAKE_INSTALL_RPATH "@executable_path")
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)

# Apple clang 21.0.0 (clang-2100.3.34.2, Xcode 27.1) miscompiles C++ at -O2/-O3: SimplifyCFG's
# MergeCompatibleInvokes merges two invokes with different function types into one call with one calling
# convention. In ScriptVM::executeSetter, the variadic EVENT_DPrintf and the (this->*response)(&ev) call became
# one call that never passes &ev, and most maps crashed at load (SimpleEntity::SetOrigin). The pass runs at
# compile time and again at the ThinLTO link, so it's turned off in both. Re-check with a newer Xcode
# (docs/METAL_PORT_PLAN.md, section 3.2 "Added in session 7", has a standalone repro) and drop it once fixed.
string(APPEND CMAKE_CXX_FLAGS " -mllvm -simplifycfg-merge-compatible-invokes=false")
add_link_options("-Wl,-mllvm,-simplifycfg-merge-compatible-invokes=false")

if(CMAKE_C_COMPILER_ID MATCHES "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "GNU")
	# Set the visibility to hidden on macOS to prevent shared libraries from
	# using functions in the executable
	# it's ok to hide them because the backtrace on macOS will still print the name of the functions
	add_compile_options(-fvisibility=hidden)
endif()
