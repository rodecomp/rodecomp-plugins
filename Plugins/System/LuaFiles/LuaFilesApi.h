// LuaFiles: one hook on LuaScript::Load shared by the plugins that change which Lua files the client loads.
// Names are LuaScript::Load names: no extension (.lub, then .lua), e.g. "Lua Files\\DataInfo\\jobName"; compared
// without case. Register from RoPlugin_Init; the names are copied.
#ifndef LUAFILES_API_H
#define LUAFILES_API_H

#ifdef LUAFILES_EXPORTS
#define LUAFILES_API extern "C" __declspec(dllexport)
#else
#define LUAFILES_API extern "C" __declspec(dllimport)
#endif

// Loads `replacement` instead of `original`. A later call for the same original wins.
LUAFILES_API void LuaFiles_Replace(const char* original, const char* replacement);

// Loads `extra` into the same Lua state right after every load of `original` (whether it succeeded or not), with the
// same search options. Several files after one original load in registration order.
LUAFILES_API void LuaFiles_LoadAfter(const char* original, const char* extra);

#endif
