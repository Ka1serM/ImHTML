#pragma once

// Version of the ImHTML public interface. Semantic versioning applies to the
// headers under include/imhtml only; anything in src/ is implementation detail
// and may change in any release.
#define IMHTML_VERSION_MAJOR 0
#define IMHTML_VERSION_MINOR 1
#define IMHTML_VERSION_PATCH 0

#define IMHTML_VERSION_STRING "0.1.0"

// Numeric form for comparisons: IMHTML_VERSION >= IMHTML_VERSION_CHECK(0, 2, 0)
#define IMHTML_VERSION_CHECK(major, minor, patch) ((major) * 10000 + (minor) * 100 + (patch))
#define IMHTML_VERSION \
    IMHTML_VERSION_CHECK(IMHTML_VERSION_MAJOR, IMHTML_VERSION_MINOR, IMHTML_VERSION_PATCH)

namespace ImHTML {

// Version of the library the binary was actually built from, which is not
// necessarily the header the caller compiled against.
const char* RuntimeVersion();

}  // namespace ImHTML
