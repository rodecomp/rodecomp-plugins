// Connection tweaks, each hook installed only for its setting:
// - NoNagle / Proxy: CConnection::Connect (virtual, hooked by name; every login/char/map connection goes through it).
//   NoNagle sets TCP_NODELAY on m_socket after a successful original. Proxy remembers the IPv4 address of the first
//   successful connection (the clientinfo.xml login server) and rewrites every later endpoint to that address: the
//   port the original would use (the override's ":port" if it has one, else endpoint.port) is kept and the override
//   host cleared, like WARP's patch of the sin_addr handed to connect().
// - SendMD5Hash: SendExecutableDigest is replaced by a version without the service-type gate and without the
//   hardcoded digests, so packet 0x204 always carries the MD5 of the running exe.
#include "RoPlugin.h"

#include "Client/Application/Launcher.h"
#include "Network/Connection/Connection.h"
#include "Network/Packets/ExecutableDigestPacket.h"
#include "Network/Packets/PacketId.h"
#include "Utility/Hash/Md5State.h"

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
  constexpr const char* kName = "NetworkTweaks";

  bool g_noNagle;
  bool g_proxy;
  in_addr g_proxyAddress;  // S_addr 0 until the first successful connection

  bool (CConnection::*g_connect)(const ConnectionEndpoint&);
  decltype(&SendExecutableDigest) g_sendExecutableDigest;

  int EndpointPort(const ConnectionEndpoint& endpoint) {
    if (const char* colon = std::strrchr(endpoint.overrideAddress, ':')) {
      return std::atoi(colon + 1);
    }
    return endpoint.port;
  }

  struct ConnectionHook : CConnection {
    bool ConnectDetour(const ConnectionEndpoint& endpoint) {
      const ConnectionEndpoint* target = &endpoint;
      ConnectionEndpoint redirected;
      if (g_proxy && g_proxyAddress.S_un.S_addr != 0) {
        redirected = endpoint;
        std::strncpy(redirected.address, inet_ntoa(g_proxyAddress), sizeof(redirected.address) - 1);
        redirected.address[sizeof(redirected.address) - 1] = '\0';
        redirected.port = EndpointPort(endpoint);
        redirected.overrideAddress[0] = '\0';
        target = &redirected;
      }
      if (!(this->*g_connect)(*target)) {
        return false;
      }
      if (g_proxy && g_proxyAddress.S_un.S_addr == 0) {
        g_proxyAddress = this->m_address.sin_addr;
      }
      if (g_noNagle) {
        const BOOL noDelay = TRUE;
        setsockopt(this->m_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
      }
      return true;
    }
  };

#pragma pack(push, 1)
  struct ExecutableDigestPacket {
    std::int16_t id;
    unsigned char digest[16];
  };
#pragma pack(pop)
  static_assert(sizeof(ExecutableDigestPacket) == 18);

  void SendExecutableDigestDetour() {
    char filename[MAX_PATH];
    GetModuleFileNameA(g_instance, filename, MAX_PATH);
    FILE* file = std::fopen(filename, "rb");
    if (!file) {
      return;
    }
    MD5State md5;
    md5.Init();
    unsigned char buffer[4096];
    for (std::size_t bytes; (bytes = std::fread(buffer, 1, sizeof(buffer), file)) != 0;) {
      md5.Append(buffer, static_cast<int>(bytes));
    }
    std::fclose(file);

    ExecutableDigestPacket packet;
    packet.id = ID_CA_EXE_HASHCHECK;
    md5.Finish(packet.digest);
    GetRagConnection().Send(GetRagConnection().GetPacketSize(packet.id), &packet);
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_noNagle = host->SettingBool(kName, "NoNagle");
  g_proxy = host->SettingBool(kName, "Proxy");
  const bool sendMd5Hash = host->SettingBool(kName, "SendMD5Hash");
  if (!g_noNagle && !g_proxy && !sendMd5Hash) {
    host->Log("%s: NoNagle, Proxy and SendMD5Hash are all off, nothing to do", kName);
    return true;
  }
  return (!(g_noNagle || g_proxy) ||
          ro::HookVirtual(host, "?Connect@CConnection@@UAE_NABUConnectionEndpoint@@@Z", &ConnectionHook::ConnectDetour, g_connect, kName)) &&
         (!sendMd5Hash || ro::Hook(host, &SendExecutableDigest, &SendExecutableDigestDetour, g_sendExecutableDigest, kName));
}
