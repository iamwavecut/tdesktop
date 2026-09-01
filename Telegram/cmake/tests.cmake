# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

add_executable(test_text WIN32)
init_target(test_text "(tests)")

add_executable(test_mcp)
init_target(test_mcp "(tests)")

add_executable(test_mcp_conformance_server)
init_target(test_mcp_conformance_server "(tests)")

target_include_directories(test_mcp PRIVATE ${src_loc})

nice_target_sources(test_mcp ${src_loc}
PRIVATE
    core/mcp/mcp_dispatcher.cpp
    core/mcp/mcp_dispatcher.h
    core/mcp/mcp_http_parser.cpp
    core/mcp/mcp_http_parser.h
    core/mcp/mcp_http_server.cpp
    core/mcp/mcp_http_server.h
    core/mcp/mcp_protocol.cpp
    core/mcp/mcp_protocol.h
    core/mcp/mcp_tl_registry.cpp
    core/mcp/mcp_tl_registry.h
    core/mcp/mcp_tl_codec.cpp
    core/mcp/mcp_tl_codec.h
    tests/test_mcp.cpp
)

target_link_libraries(test_mcp
PRIVATE
    desktop-app::lib_base
    desktop-app::external_qt
)

set_target_properties(test_mcp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_include_directories(test_mcp_conformance_server PRIVATE ${src_loc})

nice_target_sources(test_mcp_conformance_server ${src_loc}
PRIVATE
    core/mcp/mcp_dispatcher.cpp
    core/mcp/mcp_dispatcher.h
    core/mcp/mcp_http_parser.cpp
    core/mcp/mcp_http_parser.h
    core/mcp/mcp_http_server.cpp
    core/mcp/mcp_http_server.h
    core/mcp/mcp_protocol.cpp
    core/mcp/mcp_protocol.h
    tests/test_mcp_conformance_server.cpp
)

target_link_libraries(test_mcp_conformance_server
PRIVATE
    desktop-app::lib_base
    desktop-app::external_qt
)

set_target_properties(test_mcp_conformance_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_include_directories(test_text PRIVATE ${src_loc})

nice_target_sources(test_text ${src_loc}
PRIVATE
    tests/test_main.cpp
    tests/test_main.h
    tests/test_text.cpp
)

nice_target_sources(test_text ${res_loc}
PRIVATE
    qrc/emoji_1.qrc
    qrc/emoji_2.qrc
    qrc/emoji_3.qrc
    qrc/emoji_4.qrc
    qrc/emoji_5.qrc
    qrc/emoji_6.qrc
    qrc/emoji_7.qrc
    qrc/emoji_8.qrc
)

target_link_libraries(test_text
PRIVATE
    desktop-app::lib_base
    desktop-app::lib_crl
    desktop-app::lib_ui
    desktop-app::external_qt
    desktop-app::external_qt_static_plugins
)

set_target_properties(test_text PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(Telegram test_text)

target_prepare_qrc(test_text)

if (APPLE)
    add_custom_command(TARGET test_text POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:test_text>/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/test_text.rcc"
            "${CMAKE_BINARY_DIR}/lib_ui.rcc"
            "$<TARGET_FILE_DIR:test_text>/Contents/Resources/"
    )
endif()
