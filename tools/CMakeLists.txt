# ---------------------------------------------------------------------------
# Developer tools — standalone executables linking the mc_engine library.
# Per project decision (docs/animation.md §10): editors/debuggers NEVER live in
# the game exe; each tool is its own program here.
# ---------------------------------------------------------------------------

# anim_studio (T1): preview + scrub authored clips on the player rig, orbit camera,
# and (later) the CS2/GTA-style state/event timeline over the graph snapshot ring.
# You author clips in Blockbench (Bedrock export -> import_bedrock); this previews them.
add_executable(anim_studio anim_studio/main.cpp)
add_dependencies(anim_studio shaders)
target_link_libraries(anim_studio PRIVATE mc_engine)
if(MSVC)
    target_compile_options(anim_studio PRIVATE /permissive-)
endif()

# Copy compiled shaders + assets next to the tool (it loads the same model/shaders/clips).
add_custom_command(TARGET anim_studio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_BINARY_DIR}/shaders $<TARGET_FILE_DIR:anim_studio>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:anim_studio>/assets
    COMMENT "Copying shaders and assets next to anim_studio"
    VERBATIM)
