// SPDX-License-Identifier: GPL-3.0-or-later
// The boot-time GPU self-test: a compute dispatch whose output the CPU can verify
// without knowing anything about the GPU.
//
// On a console a device can be created and a command list executed while nothing
// actually runs - a queue that silently drops work, a fence that never advances,
// a readback buffer that is never written. This shader makes that distinguishable
// from a slow frame: the CPU writes a seed, the GPU writes
// `index * 2654435761 + seed` to a UAV, the CPU reads the buffer back through a
// READBACK heap and compares. Pass or fail is a single row on the launcher.
//
// Compiled at build time: dxc -T cs_6_4 -E SelftestCs -O3 -WX.

#define THREADS 64

cbuffer SelftestParams : register(b0) {
    uint count;  // elements to write, <= the dispatch's thread count
    uint seed;
    uint2 padding;
};

RWByteAddressBuffer target : register(u0);

// The CPU's copy of this function is in replay.cpp; both must agree, and
// tests/stream_test.cpp pins the arithmetic.
uint Pattern(uint index, uint seed_value) {
    // Knuth's multiplicative hash constant: cheap, and every bit of the index
    // affects the result, so a partial write is visible.
    return index * 2654435761u + seed_value;
}

[numthreads(THREADS, 1, 1)]
void SelftestCs(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) {
        return;
    }
    target.Store(id.x * 4u, Pattern(id.x, seed));
}
