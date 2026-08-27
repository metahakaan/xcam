cmake_minimum_required(VERSION 3.20)
project(xcam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
    add_compile_definitions(
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
endif()

# ---- core: everything the app and (later) the DirectShow filter share --------

add_library(xcam-core STATIC
    core/log.cpp
    core/shared_frames.cpp
    core/face_finder.cpp
    core/mf_aac_encoder.cpp
    core/wasapi_capture.cpp
    core/protocol.cpp
    core/adb.cpp
    core/net_client.cpp
    core/mf_decoder.cpp
    core/mf_audio_decoder.cpp
    core/shared_audio.cpp
    core/settings.cpp
    core/discovery.cpp
    core/mp4_writer.cpp
    core/mf_encoder.cpp
)
target_include_directories(xcam-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xcam-core PUBLIC
    ws2_32
    mfplat mfuuid mfreadwrite mf wmcodecdspuuid
    d3d11 dxgi dxguid
    d2d1 dwrite
    shell32 ole32 comdlg32
    # The face detector that ships with Windows, reached through C++/WinRT.
    # No model to bundle and no runtime to link beyond this one import library.
    windowsapp
    # WASAPI: the microphone on this machine, for the second audio track.
    avrt ksuser
)

# ---- probe: console client, mirrors tools/dump_stream.py --info --------------
#
# Small on purpose. It exercises the protocol and adb layers against a real
# phone without dragging in the decoder or a window, so a handshake regression
# shows up here rather than three layers deep in the preview.

add_executable(xcam-probe app/probe_main.cpp)
target_link_libraries(xcam-probe PRIVATE xcam-core)

# ---- app: the preview window ------------------------------------------------

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/app/main.cpp)
    # The .rc carries the application icon. MSVC compiles it as part of the
    # target; a missing or malformed one fails the link rather than silently
    # producing an iconless exe, which is the failure worth having.
    add_executable(xcam-app WIN32
        app/main.cpp
        app/preview_d3d11.cpp
        app/lut.cpp
        app/pro_panel.cpp
        app/settings_panel.cpp
        app/takes_panel.cpp
        app/take_sidecar.cpp
        app/strings.cpp
        app/ui/ui_context.cpp
        app/xcam.rc
    )
    target_link_libraries(xcam-app PRIVATE xcam-core)
endif()

# ---- dsfilter: the virtual camera ------------------------------------------
#
# A COM DLL that Windows loads into whichever application opens the camera, so
# it shares nothing with xcam-app except the named section in core/shared_frames.
# Built against the DirectShow interfaces directly: the SDK ships strmbase.lib
# without its headers, and vendoring BaseClasses would cost this project its one
# hard property -- that it builds with nothing but MSVC and the Windows SDK.

add_library(xcam-dsfilter SHARED
    dsfilter/xcam_filter.cpp
    dsfilter/xcam_audio_filter.cpp
    dsfilter/filter_trace.cpp
    dsfilter/xcam_guids.cpp
    dsfilter/xcam_filter.def
)

# Anything that has ever enumerated cameras -- Chrome, NVIDIA Broadcast, a
# conferencing app -- keeps this DLL loaded, and Windows will not let the linker
# overwrite a loaded module. It will happily let it be *renamed*, though, and the
# registered path is what matters, so move the old one aside first. Processes
# still holding it carry on with the renamed file until they unload.
#
# The suffix is randomised because a previous cast-off may itself still be
# loaded, and everything here is best-effort: on the first build there is
# nothing to move, and a failure to tidy up must not fail the build.
add_custom_command(TARGET xcam-dsfilter PRE_LINK
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/dsfilter/prelink-move-aside.cmd"
            "$<TARGET_FILE_DIR:xcam-dsfilter>"
    COMMENT "Moving any in-use copy of the filter aside"
)
target_link_libraries(xcam-dsfilter PRIVATE xcam-core strmiids winmm)
set_target_properties(xcam-dsfilter PROPERTIES
    OUTPUT_NAME "xcam-dsfilter"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ---- setup: the installer ---------------------------------------------------
#
# Carries the other three binaries inside itself as RCDATA, so a release is one
# .exe instead of a zip and a script. See installer/setup_main.cpp for why this
# is hand-written rather than an Inno Setup or WiX script; the short version is
# that this project builds with nothing but MSVC and the Windows SDK, and an
# installer only some clones can produce is an installer that goes stale.
#
# The payload .rc is generated rather than checked in because it names build
# outputs, and only the build knows where those land -- Release and Debug do not
# agree, and neither does a multi-config generator with a single-config one.
# file(GENERATE) is what expands $<TARGET_FILE:...>; configure_file cannot.

# The payload is staged beside the script and named relatively.
#
# Not by absolute path, which is the obvious way and does not work: the resource
# compiler reads the script in the system ANSI codepage, so an absolute path
# containing anything outside it -- this repository has lived in a folder called
# "Yeni klasör (2)" since the day it was made -- arrives as mojibake and the file
# is reported missing. A relative name keeps the script itself pure ASCII, and
# the one path that is still non-ASCII reaches rc.exe on the command line, where
# it is encoded properly.
#
# The output name carries the configuration because a Visual Studio generator
# builds Debug and Release from one project, and $<TARGET_FILE> differs between
# them -- one shared output would have to hold two different contents, which is
# exactly what CMake refuses to do.

set(payload_dir ${CMAKE_CURRENT_BINARY_DIR}/payload_$<CONFIG>)
set(payload_rc ${CMAKE_CURRENT_BINARY_DIR}/xcam_payload_$<CONFIG>.rc)

file(GENERATE OUTPUT ${payload_rc} CONTENT
"// Generated by CMake. Do not edit; edit the CMakeLists that writes it.
#include \"installer/setup_resource.h\"

IDR_XCAM_APP      RCDATA \"payload_$<CONFIG>/xcam-app.exe\"
IDR_XCAM_PROBE    RCDATA \"payload_$<CONFIG>/xcam-probe.exe\"
IDR_XCAM_DSFILTER RCDATA \"payload_$<CONFIG>/xcam-dsfilter.dll\"
")

add_executable(xcam-setup WIN32
    installer/setup_main.cpp
    installer/setup.rc
    ${payload_rc}
)
target_include_directories(xcam-setup PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(xcam-setup xcam-app xcam-probe xcam-dsfilter)

# Staged immediately before the resource compiler runs.
#
# This is also what keeps the payload honest: rc.exe reads the binaries when it
# compiles the script, but MSBuild only watches the script's own timestamp, so
# without a copy every time a rebuilt application would be silently packaged
# with the previous build's bytes. This target only runs once all three are up
# to date, so copying here is copying the current ones.
add_custom_command(TARGET xcam-setup PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${payload_dir}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:xcam-app> ${payload_dir}/xcam-app.exe
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:xcam-probe> ${payload_dir}/xcam-probe.exe
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:xcam-dsfilter> ${payload_dir}/xcam-dsfilter.dll
    COMMAND ${CMAKE_COMMAND} -E touch ${payload_rc}
    COMMENT "Staging the installer payload"
)
