#pragma once

namespace rex::audio {

// One empty-mask context kick: wakes the decoder worker for a sweep.
void KickXmaDecoder();

// 1 ms sweep thread; the SDK's decoder worker has no re-poll timeout.
void StartXmaKicker();
void StopXmaKicker();

}  // namespace rex::audio
