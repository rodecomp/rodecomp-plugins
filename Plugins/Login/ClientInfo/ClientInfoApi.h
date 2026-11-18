// ClientInfo's API for dependent plugins (dependencies: ClientInfo = "^1.1.0").
#ifndef RAGEXE_PLUGIN_CLIENTINFO_CLIENTINFOAPI_H
#define RAGEXE_PLUGIN_CLIENTINFO_CLIENTINFOAPI_H

#ifdef CLIENTINFO_EXPORTS
#define CLIENTINFO_API extern "C" __declspec(dllexport)
#else
#define CLIENTINFO_API extern "C" __declspec(dllimport)
#endif

// The login server ClientInfo connects the account login to: the <address> of the applied <connection> (connection 0
// at start-up, then the one picked from the server list), copied NUL-terminated into `address` (`size` bytes,
// truncated), and its <port> into `*port` (0 when the clientinfo.xml has none and the game's port is used). False,
// with nothing written, while no clientinfo.xml address is applied (the game's own login server is used). Valid from
// the end of InitializeServiceStrings on; `port` may be null.
CLIENTINFO_API bool ClientInfo_LoginServer(char* address, int size, int* port);

#endif
