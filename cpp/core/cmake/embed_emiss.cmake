# cmake -DIN=<binary file> -DOUT=<C++ file> -P embed_emiss.cmake
# Writes the bytes of IN as uti120::detail::kEmissCurveBytes.
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
math(EXPR bytes "${n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
file(WRITE "${OUT}"
  "// Generated from ${IN} by embed_emiss.cmake; do not edit.\n"
  "#include <cstddef>\n"
  "namespace uti120::detail {\n"
  "extern const unsigned char kEmissCurveBytes[] = {${body}};\n"
  "extern const std::size_t kEmissCurveSize = ${bytes};\n"
  "}  // namespace uti120::detail\n")
