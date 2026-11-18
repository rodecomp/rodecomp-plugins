// CLoginMode::OnChangeState builds the 0x64 login packet with DecryptAES (ServicePhilippine) or DecryptRijndael192
// (ServiceTaiwan) applied to the password; every other service type copies it as typed. Both functions are only
// called there, so each is hooked to copy its input unchanged, as WARP's patch forced the plain-copy branch.
// DecryptRijndael192 writes one 24-byte block (the packet's password field).
#include "RoPlugin.h"

#include "Utility/Cipher/AES.h"
#include "Utility/Cipher/Rijndael.h"

#include <cstring>

namespace {
  constexpr const char* kName = "PlainPassword";
  constexpr unsigned kRijndaelBlock = 24;

  decltype(&DecryptAES) g_decryptAes;
  decltype(&DecryptRijndael192) g_decryptRijndael192;

  void DecryptAESDetour(const char* input, char* output, unsigned int length) {
    std::memcpy(output, input, length);
  }

  void DecryptRijndael192Detour(const char* input, char* output) {
    std::memcpy(output, input, kRijndaelBlock);
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &DecryptAES, &DecryptAESDetour, g_decryptAes, kName) &&
         ro::Hook(host, &DecryptRijndael192, &DecryptRijndael192Detour, g_decryptRijndael192, kName);
}
