#pragma once

#include <cstdint>

class PS2Runtime;

// [DoD] IOP thread -> EE thread work (threaded IOP mode, DOD_IOP_THREAD; KillzoneRecomp patch 0015). The IOP thread
// must not touch EE scheduler state, so what it wants the EE to do (SIF commands to the EE's registered handlers) is
// queued by the host adapter and a wake-up event with this id (EeEventType::Dmac, otherwise a no-op) is posted;
// EeScheduler::processEvent then calls ps2IopDrainPosted() on the EE thread, which applies the queue in order.
constexpr uint32_t kIopPostedWorkEventId = 0x494F5000u; // 'IOP\0'

void ps2IopDrainPosted(PS2Runtime &runtime);
