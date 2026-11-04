// recomp.h as the generated code gets it: third_party/recomp.h, built in
// (cmake/embed_binary.cmake), the same bytes 3dsrecomp's abi/recomp.h has;
// and the bundled hints (hints.txt).

#include "codegen/codegen.h"

namespace {
#include "hints.inc"
#include "recomp_header.inc"
}

namespace recomp {

// build.h's
extern const std::string BUNDLED;

const std::string HEADER(reinterpret_cast<const char *>(RECOMP_HEADER), sizeof RECOMP_HEADER);
const std::string BUNDLED(reinterpret_cast<const char *>(RECOMP_HINTS), sizeof RECOMP_HINTS);

} // namespace recomp
