// SPDX-License-Identifier: GPL-3.0-or-later
// The guest's fixed address windows, reserved before anything else runs.
#pragma once

namespace bbx::platform {

// Reserves the guest's three fixed ranges as placeholders, through bbhost's
// own virtual-memory layer (core/win_vm.cpp, which under BBX_UWP is
// `VirtualAlloc2FromApp` + `MEM_RESERVE_PLACEHOLDER`):
//
//   0x0000000100000000 + 6.94 GiB  the CPU's direct-memory window
//   0x000000FF00000000 + 4 GiB     the GPU alias window (the same pages)
//   0x0000000030000000 + 256 MiB   a second home for the eboot image
//
// Called first thing in `main.cpp`. A desktop build relaunches itself when the
// reservation loses (the heap or a DLL took an address); a UWP title cannot, so
// the reservation is made before SDL's threads and D3D12's own reservations
// exist. False when a window was already taken: the guest then maps best-effort
// and the boot report says so.
bool ReserveGuestWindows();

}  // namespace bbx::platform
