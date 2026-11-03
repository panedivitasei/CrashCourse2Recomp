#pragma once
#include <cstdint>
#include <string>

namespace rex {
class Runtime;
}  // namespace rex

// Online play on the Guide's Game Jolt account: CC2 server calls and trophy forwarding. Off with online_mode = false.
namespace cc2::online {

// Starts the trophy worker and hooks achievement unlocks; call once the kernel state exists.
void Start(rex::Runtime* runtime);
void Stop();

// XamShowSigninUI: opens the Guide's Game Jolt sign-in; closing it without an account goes offline.
void RequestSignIn(uint32_t flags, uint32_t mask);
// Opens the Guide's Game Jolt sign-in.
void ShowSignIn();
std::string SignedInName();  // the Game Jolt username once signed in, else empty
// Authenticated JSON call to the CC2 server with the signed-in session. Returns the HTTP status, 0 when the server
// can't be reached and -1 when no account is signed in; blocks, so call it off the guest thread unless timeout_ms
// bounds it (0 keeps the default timeouts).
int ServerRequest(const char* verb, const std::string& path, const std::string& json, std::string& body,
                  int timeout_ms = 0);

// Live once signed in to Game Jolt or gone offline; always with online_mode off.
bool ReportLive();

}  // namespace cc2::online
