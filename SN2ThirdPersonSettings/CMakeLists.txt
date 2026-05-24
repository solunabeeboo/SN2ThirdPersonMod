set(TARGET SN2ThirdPersonSettings)

add_library(${TARGET} SHARED dllmain.cpp)

target_compile_features(${TARGET} PRIVATE cxx_std_23)

target_compile_definitions(${TARGET} PRIVATE
    RC_DYNAMIC_OUTPUT_BUILD_STATIC
    RC_INPUT_BUILD_STATIC
    RC_FILE_BUILD_STATIC
)

target_include_directories(${TARGET} PRIVATE ${UE4SS_INCLUDE_DIRS})

target_link_libraries(${TARGET} PRIVATE
    "${CMAKE_SOURCE_DIR}/UE4SS.lib"
    imgui_static
    minhook
    d3d12
    dxgi
    dxguid
)

set(INSTALL_DIR "c:/Games/Subnautica 2/Subnautica2/Binaries/Win64/ue4ss/Mods/SN2ThirdPersonMod/dlls")

add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${INSTALL_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${TARGET}> "${INSTALL_DIR}/main.dll"
    COMMENT "Installing ${TARGET} -> ${INSTALL_DIR}/main.dll"
)
