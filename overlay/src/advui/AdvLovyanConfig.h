#pragma once

// LovyanGFX's LGFXBase.hpp changes the LovyanGFX base class depending on whether
// lgfx_filesystem_support.hpp was seen first. The library's own .cpp files include
// LGFXBase.hpp directly while consumers normally include LovyanGFX.hpp, which made
// LTO see two incompatible class definitions. Force the lightweight support
// template to exist before every C++ translation unit; optional FS/HTTP overloads
// stay disabled because their headers have not been included at this point.
#if defined(__cplusplus) && __has_include(<lgfx/v1/lgfx_filesystem_support.hpp>)
#include <lgfx/v1/lgfx_filesystem_support.hpp>
#endif
