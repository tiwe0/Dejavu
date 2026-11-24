# Embed the Lua resource management helpers (Java ref GC/<close>/scope + dejavu.scope).
if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "generate-lua-resources.cmake requires -DINPUT=... -DOUTPUT=...")
endif()
file(READ "${INPUT}" content)
string(FIND "${content}" ")DEJAVU_RES\"" delimiter)
if(NOT delimiter EQUAL -1)
    message(FATAL_ERROR "Lua resources bootstrap contains the embedding delimiter")
endif()
get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${OUTPUT}"
"// Generated from zygisk/config/resources.lua. Do not edit.\n"
"constexpr char kLuaResourcesBootstrap[] = R\"DEJAVU_RES(\n"
"${content}"
")DEJAVU_RES\";\n")
