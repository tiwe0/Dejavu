# Embed the Lua RPC API without requiring a runtime filesystem script.
if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "generate-lua-rpc.cmake requires -DINPUT=... -DOUTPUT=...")
endif()
file(READ "${INPUT}" content)
string(FIND "${content}" ")DEJAVU_RPC\"" delimiter)
if(NOT delimiter EQUAL -1)
    message(FATAL_ERROR "Lua RPC bootstrap contains the embedding delimiter")
endif()
get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${OUTPUT}"
"// Generated from zygisk/config/rpc.lua. Do not edit.\n"
"constexpr char kLuaRpcBootstrap[] = R\"DEJAVU_RPC(\n"
"${content}"
")DEJAVU_RPC\";\n")
