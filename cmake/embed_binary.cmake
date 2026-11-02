# A binary file as a C array, for #include:
# cmake -DIN=<file> -DOUT=<file> -DNAME=<array> -P embed_binary.cmake
file(READ ${IN} hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" bytes "${bytes}")
file(WRITE ${OUT} "static const unsigned char ${NAME}[] = {\n${bytes}};\n")
