// SPDX-License-Identifier: GPL-3.0-or-later
// The detile compute pass: GCN's micro-tiled image layout to a linear texture.
//
// Guest textures arrive in the tiling the GCN tiling unit wrote (bbx_untile's
// Tiling/micro-tile arithmetic is the reference, and its round trip is tested
// portably). Nothing on the console untiles in hardware for a texture the port
// creates itself, so P2 does it with a compute dispatch: one group per 8x8 micro
// tile, 64 threads, one thread per element.
//
// Scope: 4-byte elements (the RGBA8 family, which is what the stream's transfers
// carry). Other element sizes are copied linearly by the caller
// (bbx_untile::CanCopyLinear) rather than pretending this shader handles them.
//
// Compiled at build time: dxc -T cs_6_4 -E UntileCs -O3 -WX. 64 * 4 bytes of
// groupshared is 256 bytes, far inside guard G15's 32 KiB; no
// ResourceDescriptorHeap, no 16-bit types.

#define THREADS 64

cbuffer UntileParams : register(b0) {
    uint2 tile_counts;     // micro tiles across, and down
    uint2 pixels;          // image size in pixels
    uint swizzle;          // 1 = thin 1D, 2 = thin 2D, 3 = thick 2D
    uint bytes_per_tile;   // bytes of one micro tile in the tiled source
    uint src_origin;       // byte offset of this dispatch's first micro tile
    uint dst_row_pitch;    // bytes per row of the linear destination
};

ByteAddressBuffer tiled_in : register(t0);
RWByteAddressBuffer linear_out : register(u0);

// One micro tile, 8x8 elements of 4 bytes.
groupshared uint sTile[THREADS];

// GCN's thin-tile swizzle: 8x8 elements indexed by the bit-interleaved
// (x, y) -> (y[2:0], x[2:0]) permutation. MicroTileIndex in bbx_untile.cpp is the
// same arithmetic on the CPU side, and tests/untile_test.cpp asserts the round
// trip there; this must stay the same permutation.
uint SwizzledIndex(uint x, uint y) {
    // index = (y & 7) * 8 + (x & 7) for the thin modes.
    return (y & 7u) * 8u + (x & 7u);
}

[numthreads(THREADS, 1, 1)]
void UntileCs(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    const uint flat = thread.x;
    const uint tile_index = group.y * tile_counts.x + group.x;

    // Stage the whole tile through groupshared: 64 threads reading 64 elements
    // coalesces the tiled source, which is the point of doing this on the GPU.
    sTile[flat] = tiled_in.Load(src_origin + tile_index * bytes_per_tile + flat * 4u);
    GroupMemoryBarrierWithGroupSync();

    // Write it out in linear order: this thread owns one element of the tile.
    const uint x = thread.x & 7u;
    const uint y = thread.x >> 3u;
    const uint pixel_x = group.x * 8u + x;
    const uint pixel_y = group.y * 8u + y;
    if (pixel_x >= pixels.x || pixel_y >= pixels.y) {
        return;
    }
    // The swizzle applies to the tiled source order, so read with it and write
    // linear: value at (x, y) is sTile[SwizzledIndex(...)] for the tile modes and
    // sTile[flat] for linear.
    const uint source_index = swizzle == 0u ? flat : SwizzledIndex(x, y);
    linear_out.Store(pixel_y * dst_row_pitch + pixel_x * 4u, sTile[source_index]);
}
