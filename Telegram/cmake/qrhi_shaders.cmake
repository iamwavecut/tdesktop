# Compile QRhi shaders (.vert/.frag/.comp -> .qsb) at build time.
#
# Usage: include(cmake/qrhi_shaders.cmake)
# Requires: target "Telegram" and function "nice_target_sources" to exist.

if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    return()
endif()

if (QT_VERSION_MAJOR LESS 6)
    return()
endif()

set(_qsb_hints
    "${QT_DIR}/../../../libexec"
    "${QT_DIR}/../../../bin"
    "${QT_DIR}/../../qt6/libexec"
    "${QT_DIR}/../../qt6/bin"
    "${QT_DIR}/../../../share/qt/libexec"
    "${QT_DIR}/../../../opt/qtshadertools/bin")
find_program(QSB_EXECUTABLE qsb
    HINTS ${_qsb_hints}
    PATHS ENV PATH)

if (NOT QSB_EXECUTABLE)
    find_program(QTPATHS_EXECUTABLE
        NAMES qtpaths6 qtpaths
        HINTS "${QT_DIR}/../../../bin" "${QT_DIR}/../../../share/qt/bin"
        PATHS ENV PATH)
    if (QTPATHS_EXECUTABLE)
        execute_process(
            COMMAND ${QTPATHS_EXECUTABLE} --query QT_HOST_LIBEXECS
            OUTPUT_VARIABLE QT_HOST_LIBEXECS
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if (QT_HOST_LIBEXECS)
            find_program(QSB_EXECUTABLE qsb
                HINTS "${QT_HOST_LIBEXECS}"
                NO_DEFAULT_PATH)
        endif()
    endif()
endif()

if (NOT QSB_EXECUTABLE)
    message(FATAL_ERROR
        "QSB (Qt Shader Baker) was not found, but ${CMAKE_CURRENT_SOURCE_DIR}/shaders "
        "is present and must be compiled. Install Qt's shader tools or extend "
        "QSB_EXECUTABLE hints in cmake/qrhi_shaders.cmake.")
endif()

set(_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(_qsb_out_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${_qsb_out_dir})
file(GLOB _shader_sources
    "${_shader_dir}/*.vert"
    "${_shader_dir}/*.frag"
    "${_shader_dir}/*.comp")
set(_qsb_outputs)
set(_qrc_entries)
foreach(_src ${_shader_sources})
    get_filename_component(_name ${_src} NAME)
    get_filename_component(_ext ${_src} LAST_EXT)
    set(_qsb "${_qsb_out_dir}/${_name}.qsb")

    if("${_ext}" STREQUAL ".comp")
        set(_glsl_ver "310es,430")
    else()
        file(READ ${_src} _src_contents)
        string(FIND "${_src_contents}" "texelFetch" _has_texelfetch)
        if(NOT _has_texelfetch EQUAL -1)
            set(_glsl_ver "300es,150")
        else()
            set(_glsl_ver "100es,120,150")
        endif()
    endif()

    add_custom_command(
        OUTPUT ${_qsb}
        COMMAND ${QSB_EXECUTABLE}
            --glsl "${_glsl_ver}"
            --hlsl 50
            --msl 12
            -o ${_qsb}
            ${_src}
        DEPENDS ${_src}
        COMMENT "QSB: ${_name}"
        VERBATIM)
    list(APPEND _qsb_outputs ${_qsb})
    list(APPEND _qrc_entries "        <file>${_name}.qsb</file>")
endforeach()
list(SORT _qrc_entries)
list(JOIN _qrc_entries "\n" _qrc_body)
# Write shaders.qrc at configure time (so AUTORCC can scan its INPUTS) but
# only touch the file when content actually changes — otherwise AUTORCC sees
# a fresh mtime on every cmake re-generate and re-runs rcc unnecessarily.
set(_qrc_path "${_qsb_out_dir}/shaders.qrc")
set(_qrc_new "<RCC>\n    <qresource prefix=\"/shaders\">\n${_qrc_body}\n    </qresource>\n</RCC>\n")
set(_qrc_old "")
if (EXISTS "${_qrc_path}")
    file(READ "${_qrc_path}" _qrc_old)
endif()
if (NOT "${_qrc_new}" STREQUAL "${_qrc_old}")
    file(WRITE "${_qrc_path}" "${_qrc_new}")
endif()
add_custom_target(compile_shaders DEPENDS ${_qsb_outputs})
nice_target_sources(Telegram ${_qsb_out_dir}
PRIVATE
    shaders.qrc
)
add_dependencies(Telegram compile_shaders)
message(STATUS "QSB: found ${QSB_EXECUTABLE}, will compile ${_shader_dir}/*.vert/*.frag/*.comp")
