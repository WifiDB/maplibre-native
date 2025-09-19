# We need CMake 3.9 because the Xcode generator doesn't know about systems include paths before 3.9.
# See https://gitlab.kitware.com/cmake/cmake/issues/16795
cmake_minimum_required(VERSION 3.9)

if (NOT NODE_MODULE_MINIMUM_ABI)
    set(NODE_MODULE_MINIMUM_ABI 46) # Don't build node modules for versions earlier than Node 4
endif()
if (NOT NODE_MODULE_CACHE_DIR)
    set(NODE_MODULE_CACHE_DIR "${CMAKE_BINARY_DIR}")
endif()


function(_node_module_download _TYPE _URL _FILE)
    file(REMOVE_RECURSE "${_FILE}")
    string(RANDOM LENGTH 32 _TMP)
    set(_TMP "${CMAKE_BINARY_DIR}/${_TMP}")
    message(STATUS "[Node.js] Downloading ${_TYPE}...")
    file(DOWNLOAD "${_URL}" "${_TMP}" STATUS _STATUS TLS_VERIFY ON)
    list(GET _STATUS 0 _STATUS_CODE)
    if(NOT _STATUS_CODE EQUAL 0)
        file(REMOVE "${_TMP}")
        list(GET _STATUS 1 _STATUS_MESSAGE)
        message(FATAL_ERROR "[Node.js] Failed to download ${_TYPE}: ${_STATUS_MESSAGE}")
    else()
        get_filename_component(_DIR "${_FILE}" DIRECTORY)
        file(MAKE_DIRECTORY "${_DIR}")
        file(RENAME "${_TMP}" "${_FILE}")
    endif()
endfunction()


function(_node_module_unpack_tar_gz _TYPE _URL _PATH _DEST)
    string(RANDOM LENGTH 32 _TMP)
    set(_TMP "${CMAKE_BINARY_DIR}/${_TMP}")
    _node_module_download("${_TYPE}" "${_URL}" "${_TMP}.tar.gz")
    file(REMOVE_RECURSE "${_DEST}" "${_TMP}")
    file(MAKE_DIRECTORY "${_TMP}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xfz "${_TMP}.tar.gz"
        WORKING_DIRECTORY "${_TMP}"
        RESULT_VARIABLE _STATUS_CODE
        OUTPUT_VARIABLE _STATUS_MESSAGE
        ERROR_VARIABLE _STATUS_MESSAGE)
    if(NOT _STATUS_CODE EQUAL 0)
        message(FATAL_ERROR "[Node.js] Failed to unpack ${_TYPE}: ${_STATUS_MESSAGE}")
    endif()
    get_filename_component(_DIR "${_DEST}" DIRECTORY)
    file(MAKE_DIRECTORY "${_DIR}")
    file(RENAME "${_TMP}/${_PATH}" "${_DEST}")
    file(REMOVE_RECURSE "${_TMP}" "${_TMP}.tar.gz")
endfunction()


function(add_node_module NAME)
    cmake_parse_arguments("" "" "MINIMUM_NODE_ABI;NODE_API_VERSION;INSTALL_PATH;CACHE_DIR" "EXCLUDE_NODE_ABIS" ${ARGN})
    if(NOT _MINIMUM_NODE_ABI)
        set(_MINIMUM_NODE_ABI "${NODE_MODULE_MINIMUM_ABI}")
    endif()
    if (NOT _CACHE_DIR)
        set(_CACHE_DIR "${NODE_MODULE_CACHE_DIR}")
    endif()
    if (NOT _INSTALL_PATH)
        set(_INSTALL_PATH "platform/node/lib/${NAME}.node")
    endif()
    get_filename_component(_CACHE_DIR "${_CACHE_DIR}" REALPATH BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(_UNPARSED_ARGUMENTS)
        message(WARNING "[Node.js] Unused arguments: '${_UNPARSED_ARGUMENTS}'")
    endif()

    # Require NODE_API_VERSION
    if(NOT _NODE_API_VERSION)
        message(FATAL_ERROR "[Node.js] NODE_API_VERSION must be specified")
    endif()

    # Create master target
    add_library(${NAME} INTERFACE)

    # For Node-API, we only need to build for one representative Node.js version
    # since the binary is ABI-stable across Node.js versions
    set(_TARGET_NODE_VERSION "v18.20.4") # Use a stable LTS version with mature Node-API support
    message(STATUS "[Node.js] Building Node-API ${_NODE_API_VERSION} module using Node.js ${_TARGET_NODE_VERSION} headers (ABI-stable)")

    # Download the Node.js headers if we don't have them yet
    if(NOT EXISTS "${_CACHE_DIR}/node/${_TARGET_NODE_VERSION}/node.h")
        _node_module_unpack_tar_gz(
            "headers for Node ${_TARGET_NODE_VERSION}"
            "https://nodejs.org/download/release/${_TARGET_NODE_VERSION}/node-${_TARGET_NODE_VERSION}-headers.tar.gz"
            "node-${_TARGET_NODE_VERSION}/include/node"
            "${_CACHE_DIR}/node/${_TARGET_NODE_VERSION}"
        )
    endif()

    if(WIN32)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_ARCH x64)
        else()
            set(_ARCH x86)
        endif()

        # Download the win-${_ARCH} libraries if we are compiling on Windows and don't have them yet
        if(NOT EXISTS "${_CACHE_DIR}/lib/node/${_TARGET_NODE_VERSION}/win-${_ARCH}/node.lib")
            _node_module_download(
                "win-${_ARCH} library for Node ${_TARGET_NODE_VERSION}"
                "https://nodejs.org/download/release/${_TARGET_NODE_VERSION}/win-${_ARCH}/node.lib"
                "${_CACHE_DIR}/lib/node/${_TARGET_NODE_VERSION}/win-${_ARCH}/node.lib"
            )
        endif()
    endif()

    # Generate the Node-API library target
    add_library(${NAME}.node SHARED "${_CACHE_DIR}/empty.cpp")

    # C identifiers can only contain certain characters (e.g. no dashes)
    string(REGEX REPLACE "[^a-z0-9]+" "_" NAME_IDENTIFIER "${NAME}")

    set_target_properties(${NAME}.node PROPERTIES
        OUTPUT_NAME "${NAME}"
        SOURCES "" # Removes the fake empty.cpp again
        PREFIX ""
        SUFFIX ".node"
        MACOSX_RPATH ON
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        POSITION_INDEPENDENT_CODE TRUE
    )

    target_compile_definitions(${NAME}.node PRIVATE
        "MODULE_NAME=${NAME_IDENTIFIER}"
        "BUILDING_NODE_EXTENSION"
        "_LARGEFILE_SOURCE"
        "_FILE_OFFSET_BITS=64"
        "NAPI_VERSION=${_NODE_API_VERSION}"
        "NAPI_CPP_EXCEPTIONS"
    )

    target_include_directories(${NAME}.node SYSTEM PRIVATE
        "${_CACHE_DIR}/node/${_TARGET_NODE_VERSION}"
    )

    target_link_libraries(${NAME}.node PRIVATE ${NAME} mbgl-compiler-options)

    if(WIN32)
        target_include_directories(${NAME}.node SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/platform/windows/include")
        target_compile_definitions(${NAME}.node PRIVATE NOMINMAX)
        target_link_libraries(${NAME}.node PRIVATE "${_CACHE_DIR}/lib/node/${_TARGET_NODE_VERSION}/win-${_ARCH}/node.lib")
    endif()

    if(APPLE)
        # Ensures that linked symbols are loaded when the module is loaded instead of causing
        # unresolved symbol errors later during runtime.
        set_target_properties(${NAME}.node PROPERTIES
            LINK_FLAGS "-undefined dynamic_lookup -bind_at_load"
        )
        target_compile_definitions(${NAME}.node PRIVATE
            "_DARWIN_USE_64_BIT_INODE=1"
        )
    elseif(WIN32)
        # Do nothing here
    else()
        # Ensures that linked symbols are loaded when the module is loaded instead of causing
        # unresolved symbol errors later during runtime.
        set_target_properties(${NAME}.node PROPERTIES
            LINK_FLAGS "-z now"
        )
    endif()

    # Copy the file to the installation directory.
    get_filename_component(_OUTPUT_PATH "${_INSTALL_PATH}" ABSOLUTE "${CMAKE_CURRENT_SOURCE_PATH}")
    add_custom_command(
        TARGET ${NAME}.node
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${NAME}.node>" "${_OUTPUT_PATH}"
    )

    if(WIN32)
        unset(_ARCH)
    endif()

    # Add a target that builds the Node-API module
    add_custom_target("${NAME}.all")
    add_dependencies("${NAME}.all" ${NAME}.node)

    # Add variables for compatibility (though simpler now with single target)
    set("${NAME}::targets" "${NAME}.node" PARENT_SCOPE)
endfunction()
