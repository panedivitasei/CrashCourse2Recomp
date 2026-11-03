#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Render-only copies of lobby peers' Avatar Marketplace items. A peer's manifest can name items this install's closet
// lacks; they are fetched from the avatar marketplace server into userdata\Avatar\Cache and never listed or
// owned. Request: GET <server>/live/<titleid>/avataritems/<guid>.acp.
namespace cc2::avatar_fetch {

// Queues every item in a 1000-byte X_AVATAR_METADATA that the closet, the stock pack and the cache can't serve.
// True when something new was queued. Cheap; never touches the network.
bool QueueManifest(const uint8_t* manifest);
bool HasPending();
// Fetches one queued item and stores it in the cache. Blocks on the network, so call it off the guest thread.
bool FetchNext();

// The cached YTGR/STRB blob for an item guid ("aaaaaaaa-bbbb-..."), in the same form the closet stores.
bool ReadCachedItem(const std::string& guid, std::vector<uint8_t>& out);
// Waits up to timeout_ms while the item is queued or downloading, so a build that races the prefetch still gets it.
void WaitForItem(const std::string& guid, int timeout_ms);

}  // namespace cc2::avatar_fetch
