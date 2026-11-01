# A text file as a C++ raw string literal, for #include: cmake -DIN=<file> -DOUT=<file> -P embed.cmake
file(READ ${IN} text)
file(WRITE ${OUT} "R\"glsl(${text})glsl\"\n")
