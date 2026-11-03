// Edge layer: the GPU-facing side of the XDK D3D runtime with no GPU behind it.

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

#include <rex/system/xthread.h>
#include <rex/types.h>

#include "gpu/guest/d3d_device.h"
#include "gpu/gpu_cmdbuf.h"
#include "gpu/gpu_ring_cursor.h"

extern "C" REX_FUNC(__imp__sub_828D6A70);
extern "C" REX_FUNC(__imp__sub_82545C80);

REXCVAR_DEFINE_BOOL(gpu_skip_alu_ring, true, "GPU", "Skip audited ALU ring stores for runtime profiling");

namespace {

using cc2::gpu::guest::D3DDevice;

template <typename T>
T* Guest(uint32_t address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<T*>(address);
}

}  // namespace

// Skips ALU ring stores at the two TU1 DrawIndexedVertices call sites (lr 0x82544514 and 0x82544534).
// Recording, split handling and the cursor consumed by fence bookkeeping are preserved.
REX_HOOK_RAW(sub_82545C80) {
  if (!REXCVAR_GET(gpu_skip_alu_ring)) {
    __imp__sub_82545C80(ctx, base);
    return;
  }
  const uint32_t address = ctx.r3.u32;
  const bool vertex = ctx.lr == 0x82544514 && ctx.r5.u32 == 0x4000 &&
                      uint64_t(address) + 0x780 == ctx.r6.u64;
  const bool pixel = ctx.lr == 0x82544534 && ctx.r5.u32 == 0x4400 &&
                     uint64_t(address) + 0x1780 == ctx.r6.u64;
  if ((!vertex && !pixel) || cc2::gpu::cmdbuf::Recording(address)) {
    __imp__sub_82545C80(ctx, base);
    return;
  }
  auto* device = Guest<D3DDevice>(address);
  const uint32_t cursor = device->m_pRing;
  uint32_t next = 0;
  if (!cc2::gpu::edge::AluRingCursor(ctx.r4.u64, cursor, device->m_pRingLimit, next) ||
      (uint64_t(cursor) < uint64_t(address) + sizeof(D3DDevice) && address < next)) {
    __imp__sub_82545C80(ctx, base);
    return;
  }
  device->m_pRing = next;
}

// TU1 0x828D6A70 copies r5 bytes from r4 to r3 and saves its return pointer at r1-8.
// Keep the recompiled path for overlapping or wrapping ranges.
REX_HOOK_RAW(sub_828D6A70) {
  const uint32_t destination = ctx.r3.u32;
  const uint32_t source = ctx.r4.u32;
  const uint32_t size = ctx.r5.u32;
  const uint64_t destination_end = uint64_t(destination) + size;
  const uint64_t source_end = uint64_t(source) + size;
  const uint32_t saved_pointer = ctx.r1.u32 - 8;
  if (destination_end > UINT32_MAX || source_end > UINT32_MAX ||
      (uint64_t(destination) < source_end && uint64_t(source) < destination_end) ||
      (destination < ctx.r1.u32 && saved_pointer < destination_end)) {
    __imp__sub_828D6A70(ctx, base);
    return;
  }
  // Match the generated accessors, including the write-combined mapping offset.
  const auto bytes = [base](uint32_t address) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
    return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
    return base + address;
#endif
  };
  *reinterpret_cast<rex::be<uint64_t>*>(bytes(saved_pointer)) = ctx.r3.u64;
  if (size) std::memcpy(bytes(destination), bytes(source), size);
}

// CDevice::SetFence 0x82548570, at the store of the advanced counter (0x825486A4): r6 is the fence
// being issued, r5 the tagged ring position it marks, r11 the device. Writes the same two words as
// SetFence's own CPU-completion branch at 0x82548690, so fence and ring-space waits never block.
void Cc2Edge_FenceIssued(PPCRegister& r5, PPCRegister& r6, PPCRegister& r11) {
  const auto* device = Guest<const D3DDevice>(r11.u32);
  const uint32_t writeback = device->writeback_page;
  if (writeback) {
    auto* words = Guest<rex::be<uint32_t>>(writeback);
    words[0] = r6.u32;
    words[1] = r5.u32;
  }
}

// CDevice::AddCallsToPrimaryBuffer 0x82548090, at the store of the new primary write index
// (0x82548300): r11 is the index, r27 the device. The ring has no reader, so its read-pointer
// writeback (+0x3C) follows the write index and BlockOnPrimaryRange never waits.
void Cc2Edge_PrimaryKick(PPCRegister& r11, PPCRegister& r27) {
  const auto* device = Guest<const D3DDevice>(r27.u32);
  const uint32_t writeback = device->writeback_page;
  if (writeback) {
    Guest<rex::be<uint32_t>>(writeback)[0x3C / 4] = r11.u32;
  }
}

// D3DDevice_InsertCallback 0x82549370 (device, flags, callback, context): on hardware the callback
// address travels in the ring and a GPU interrupt runs it. Nothing reads the ring, so it runs here.
REX_HOOK_RAW(sub_82549370) {
  const uint32_t callback = ctx.r5.u32;
  if (!callback) return;
  ctx.r3.u64 = ctx.r6.u32;
  rex::runtime::ResolveIndirectFunction(callback)(ctx, base);
}

// KeInsertQueueDpc: the SDK queues DPCs and never runs them. Its only callers are D3D's QueueCommands
// 0x82548E48 and KickOffOnWorkerQueue 0x82550F40, whose routines submit a chunk and drop the pending
// count BlockUntilIdle 0x825492F8 spins on, so the routine runs on insert.
REX_HOOK_RAW(__imp__KeInsertQueueDpc) {
  const uint32_t dpc_address = ctx.r3.u32;
  auto* dpc = Guest<rex::system::XDPC>(dpc_address);
  const uint32_t routine = dpc->routine;
  dpc->arg1 = ctx.r4.u32;
  dpc->arg2 = ctx.r5.u32;
  if (routine) {
    ctx.r6.u64 = dpc->arg2;
    ctx.r5.u64 = dpc->arg1;
    ctx.r4.u64 = dpc->context;
    ctx.r3.u64 = dpc_address;
    rex::runtime::ResolveIndirectFunction(routine)(ctx, base);
  }
  ctx.r3.u64 = 1;
}

// After each add to queued_chunks in QueueCommands 0x82548E48 and QueueIndirectBuffer 0x82548AC0,
// r31 the device. The GPU subtracts as it retires buffers, so the count drops straight back to idle.
void Cc2Edge_ChunkQueued(PPCRegister& r31) {
  Guest<D3DDevice>(r31.u32)->queued_chunks = 0;
}

// Vd* ring and interrupt setup, which the SDK only warns about without a GPU plugin. The edge layer
// owns them; the ring and writeback stay in the guest memory D3D allocated.
namespace cc2::gpu::edge {

std::atomic<uint32_t> g_interrupt_callback{0};
std::atomic<uint32_t> g_interrupt_user_data{0};

void VdSetGraphicsInterruptCallback_entry(rex::u32 callback, rex::u32 user_data) {
  g_interrupt_callback.store(callback);
  g_interrupt_user_data.store(user_data);
}

void VdInitializeRingBuffer_entry(rex::u32 ring, rex::i32 size_log2) {
  (void)ring;
  (void)size_log2;
}

void VdEnableRingBufferRPtrWriteBack_entry(rex::u32 writeback, rex::i32 block_size_log2) {
  (void)writeback;
  (void)block_size_log2;
}

}  // namespace cc2::gpu::edge

REX_EXPORT(__imp__VdSetGraphicsInterruptCallback, cc2::gpu::edge::VdSetGraphicsInterruptCallback_entry)
REX_EXPORT(__imp__VdInitializeRingBuffer, cc2::gpu::edge::VdInitializeRingBuffer_entry)
REX_EXPORT(__imp__VdEnableRingBufferRPtrWriteBack, cc2::gpu::edge::VdEnableRingBufferRPtrWriteBack_entry)
