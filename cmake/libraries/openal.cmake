if(NOT USE_OPENAL)
    return()
endif()

if(NOT BUILD_CLIENT)
    return()
endif()

set(INTERNAL_OPENAL_DIR ${SOURCE_DIR}/thirdparty/openal-soft-1.25.2)

find_package(OpenAL QUIET)

if(NOT OpenAL_FOUND)
    set(OPENAL_DEFINITIONS USE_INTERNAL_OPENAL_HEADERS)
    set(OPENAL_INCLUDE_DIR ${INTERNAL_OPENAL_DIR}/include)
    set(OPENAL_LIBRARY openal)
endif()

list(APPEND CLIENT_DEFINITIONS ${OPENAL_DEFINITIONS} USE_OPENAL)
list(APPEND CLIENT_INCLUDE_DIRS ${OPENAL_INCLUDE_DIR})

if(USE_OPENAL_DLOPEN)
    list(APPEND CLIENT_DEFINITIONS USE_OPENAL_DLOPEN)
else()
    if(NOT OpenAL_FOUND)
        # Build the internal openal-soft (a shared library) with its default options, as upstream's macOS release
        # workflow builds it, except what isn't shipped: the utilities, the examples and the install files.
        # The backends that can only come from Homebrew on macOS (the build machine's, not upstream's runner) stay
        # off, so the library holds CoreAudio and the wave writer and links only system libraries
        set(ALSOFT_UTILS OFF)
        set(ALSOFT_EXAMPLES OFF)
        set(ALSOFT_INSTALL OFF)
        set(ALSOFT_INSTALL_CONFIG OFF)
        set(ALSOFT_INSTALL_HRTF_DATA OFF)
        set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF)
        set(ALSOFT_BACKEND_PIPEWIRE OFF)
        set(ALSOFT_BACKEND_PULSEAUDIO OFF)
        set(ALSOFT_BACKEND_JACK OFF)
        set(ALSOFT_BACKEND_PORTAUDIO OFF)
        set(ALSOFT_RTKIT OFF)
        # EXCLUDE_FROM_ALL: built as the client's dependency only, and no install rules in the install prefix
        add_subdirectory(${INTERNAL_OPENAL_DIR} openal-soft EXCLUDE_FROM_ALL)
        # This project names shared libraries without the lib prefix (game.dylib) and with a debug postfix:
        # openal-soft keeps its own name, libopenal.1.dylib (the install name @rpath/libopenal.1.dylib)
        set_target_properties(OpenAL PROPERTIES PREFIX lib DEBUG_POSTFIX "")
        # The macOS 26 SDK declares CoreAudio's render callback type nonblocking (CA_REALTIME_API), and openal-soft
        # 1.25.2 makes -Wfunction-effects an error by default: its two callback lambdas (alc/backends/coreaudio.cpp)
        # would stop the build. A warning again, as with older SDKs; the attribute only drives diagnostics
        target_compile_options(OpenAL PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=function-effects>)
        set(OPENAL_LIBRARY OpenAL)
    endif()
    find_package(Threads REQUIRED)
    list(APPEND CLIENT_LIBRARIES Threads::Threads ${OPENAL_LIBRARY})
endif()
