// Force-included (/FI) in every plugin source by scripts/build.py, never in the exe or RoPluginHost.
//
// Makes every member of the exe's classes public, so a detour (a member of a struct derived from the exe class)
// reads `this->m_state` and calls private methods as the class's own code would. The standard, Windows and
// third-party headers rodecomp's Source/ headers include come first, untouched: their classes keep their access
// (and their decorated names), only rodecomp's own and the plugin's classes see the redefinition.
//
// MSVC decorates access into a member's name (?f@C@@AAEXXZ private, ?f@C@@QAEXXZ public), so a plugin refers to a
// private member by its public-form name: scripts/plugin_exe.py exports each private/protected function under that
// name as well (an alias, Ragexe.def) and lists each private/protected static under it (Ragexe.globals).
//
// A member declared before any access label of a `class` is private by default, with no keyword to redefine: those
// stay inaccessible (reach them through a function of the class).
#ifndef RAGEXE_PLUGIN_SDK_ROPLUGINACCESS_H
#define RAGEXE_PLUGIN_SDK_ROPLUGINACCESS_H

// C and C++ standard library (the set rodecomp's Source/ headers and the plugins include).
#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <compare>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <deque>
#include <exception>
#include <float.h>
#include <format>
#include <forward_list>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <intrin.h>
#include <io.h>
#include <direct.h>
#include <istream>
#include <iterator>
#include <limits>
#include <list>
#include <malloc.h>
#include <map>
#include <mbctype.h>
#include <mbstring.h>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Windows SDK (winsock2.h before anything that could pull winsock.h).
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <imm.h>
#include <objbase.h>
#include <objidl.h>
#include <ole2.h>
#include <docobj.h>
#include <exdisp.h>
#include <mshtmhst.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <sqltypes.h>
#include <timeapi.h>
#include <VersionHelpers.h>
// Not ddraw.h/d3d.h/d3d9.h: their interfaces are access-free structs, and d3d.h (DirectX 7) before d3d9.h hides the
// DirectX 9 types (DIRECT3D_VERSION) a plugin such as RendererDX12 needs.
#include <atlconv.h>
// As Render/Text/FontCache.h: GDI+ needs min/max, which NOMINMAX removes.
namespace Gdiplus {
  using std::max;
  using std::min;
}
#include <gdiplus.h>
// rpcndr.h (through the COM headers above) defines `small` as `char`; a plugin that never asked for those headers
// would see its `small` variables break (WindowIcon).
#undef small

// Third-party libraries on rodecomp's include path.
#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <jsoncons/json.hpp>

#define private public
#define protected public

#endif
