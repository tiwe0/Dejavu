# Generate a C++ header embedding include/dejavu_jit.h as a raw string.
if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "generate-jit-prelude.cmake requires -DINPUT=... -DOUTPUT=...")
endif()
file(READ "${INPUT}" content)
file(WRITE "${OUTPUT}"
"// Generated from include/dejavu_jit.h. Do not edit.\n"
"constexpr char kJitPrelude[] = R\"DEJAVU_JIT(\n"
"${content}"
")DEJAVU_JIT\";\n")
