#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <rex/ppc/context.h>

namespace rex::input { struct X_INPUT_GAMEPAD; }
namespace rex::ui { class Window; }

// Online lobbies from the JMstudios CC2 server. The multiplayer menu's Quick Match becomes JOIN MATCH: it lists the
// server's lobbies and joins the picked one there. The lobby host then creates the game's session through its Private
// Match path and publishes it; members accept a game invite to it (XN_LIVE_INVITE_ACCEPTED + XInviteGetAcceptedInfo).
namespace cc2::lobby {
void Initialize(rex::ui::Window* window);
void Shutdown();  // leaves the server lobby if still in one
void Tick(PPCContext& ctx, uint8_t* base);
void FilterInput(uint32_t user, rex::input::X_INPUT_GAMEPAD* pad);
bool Active();  // the lobby list is up and owns input
void SigninChanged();  // closes the list and leaves a joined lobby
// Runs a Lua chunk on the game's main Lua state and returns its number result: -1 on a Lua error, -2 when the state
// does not exist yet. state receives the state's address so callers can notice it being replaced.
int RunMainLua(PPCContext& ctx, uint8_t* base, const char* source, uint32_t* state = nullptr);
// The game's main Lua state, 0 before it exists.
uint32_t MainLuaState();
// Runs a chunk on Lua state L; thread receives a returned coroutine.
int RunLua(PPCContext& ctx, uint8_t* base, uint32_t L, const char* source, uint32_t* thread = nullptr);
// GUI-space pointer from the mouse layer; button 0 is a left press, 1 a right press, -1 a move only.
void Pointer(PPCContext& ctx, uint8_t* base, float x, float y, int button);

// Kernel-side session events from the XGI app: the game created or deleted its XSession. info is the 60-byte
// XSESSION_INFO (session id, host XNADDR, key exchange key) the game now uses.
void OnSessionCreated(bool host, const uint8_t* info);
void OnSessionDeleted();
// XSessionMigrateHost: the game moved its session to a new host. local_host means this machine hosts it now and info
// gets published to the lobby; otherwise info is the new host's session.
void OnHostMigrated(bool local_host, const uint8_t* info);
// The 60-byte XSESSION_INFO the game is using; false when it has none.
bool CurrentSessionInfo(uint8_t* info);

// For a lobby member: the host's published XSESSION_INFO, which the session search and the accepted invite return.
// False for the host, outside a lobby, or while the host has not published yet.
bool JoinTarget(uint8_t* info);
// The 8-byte XNKID of the session in use (zeros when there is none).
void CurrentSessionId(uint8_t* xnkid);

// The local console's network identity: the lobby pid's 36-byte XNADDR (cc2::relay), and host random bytes for
// XNetRandom and session keys.
void FillLocalXnAddr(uint8_t* xnaddr);
void RandomBytes(void* out, size_t size);

// The online XUID a Game Jolt username maps to (0x0009 + 48-bit FNV-1a of the lowercased name). Peers learn each
// other's XUIDs through the game's own session traffic, so both sides must derive it the same way.
uint64_t XuidForName(const std::string& name);

// Lobby members' avatar manifests (1000-byte X_AVATAR_METADATA), exchanged through the server's join and heartbeat
// replies because the game reads a remote player's avatar from the LIVE profile by XUID. False when not known yet.
constexpr size_t kAvatarManifestSize = 1000;
bool PeerAvatar(uint64_t xuid, uint8_t* manifest);
bool IsPeerXuid(uint64_t xuid);
// Asks the worker for an early heartbeat, e.g. when the relay reports a new member. Safe from any thread.
void RequestPeerRefresh();
// The cached manifest, else one heartbeat on the calling thread bounded by budget_ms. False when the member's avatar
// is still unknown; the caller then reports the setting as absent.
bool FetchPeerAvatar(uint64_t xuid, uint8_t* manifest, int budget_ms);
}  // namespace cc2::lobby
