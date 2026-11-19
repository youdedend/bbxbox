// SPDX-License-Identifier: GPL-3.0-or-later
// The guest's memory, as the console's compute shaders see it: a two-level page
// table and a bounded table of slabs.
//
// The contract is fixed by the shaders the prebake emits (gcn/hlsl.cpp
// console_memory_prelude, ARCHITECTURE_XBOX_D3D12.md §3.3), so it is written
// once here and every other file - the bake's notes, `pipeline.h`'s root
// signature, the console bridge - reads these constants instead of its own:
//
//   t0, space3  ByteAddressBuffer g_page_table
//       l1: 4096 dwords, one per 4 GiB region of the VA
//           l2_base = l1[min(va_hi, 4095)]           (a dword index? no: an
//                                                    8-byte entry index, the
//                                                    shader multiplies by 8)
//       l2: one 8-byte entry per 64 KiB page, 65536 per region
//           uint2(slab, byte offset of the page inside the slab)
//   u0..u31, space3  RWByteAddressBuffer g_guest_slabs[32]
//       32 slabs of 256 MiB (8 GiB), each under 4 GiB, `NonUniformResourceIndex`
//
// Two rules make the port's own use of it honest:
//
//  * A guest VA is two dwords in the shader, so a range may not cross a 4 GiB
//    boundary: there is no carry into `va_hi`'s region index. MapRange refuses
//    one rather than map half of it.
//  * Every entry the guest has not mapped reads the *guard page* - slab 0, page
//    0 - which is a 64 KiB buffer of its own, never guest data. A stray lane
//    that walks a page it has no business in reads and writes that page, not a
//    live one: the walk cannot fault (the shader has no fault to take) and
//    cannot corrupt the title's memory.
//
// The portable half below is what the build machine tests (tests/guest_mem_test.cpp).
// The D3D12 half - the page table's buffer and SRV, the slab buffers and their
// UAVs, lazily resident so 32 empty slabs cost nothing - is `#ifdef _WIN32`,
// because it needs the device; tools/syntax_check.sh type-checks it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace bbx::d3d12 {

// ---- the contract, in one place -------------------------------------------------

inline constexpr std::uint32_t kGuestSpace = 3;
inline constexpr std::uint32_t kGuestPageTableRegister = 0;  // t0: the walk
inline constexpr std::uint32_t kGuestSlabRegister = 0;       // u0..u31: the data
inline constexpr std::uint32_t kGuestSlabCount = 32;
inline constexpr std::uint32_t kGuestSlabBytes = 256u << 20;  // 256 MiB; 32 x this = 8 GiB
inline constexpr std::uint32_t kPageL1Entries = 4096;         // one dword per 4 GiB region
inline constexpr std::uint32_t kPageL2Entries = 65536;        // one 8-byte entry per 64 KiB page
inline constexpr std::uint32_t kPageShift = 16;               // 64 KiB pages: the console's granularity
inline constexpr std::uint64_t kPageBytes = 1ull << kPageShift;
inline constexpr std::uint64_t kSlabPages = kGuestSlabBytes >> kPageShift;  // 4096 pages per slab
inline constexpr std::uint64_t kL1RegionBytes = 1ull << 32;                 // 4 GiB per L1 entry

// The guard page: slab 0, page 0. Slab 0 keeps its first page for it and is
// never handed out to the guest.
inline constexpr std::uint32_t kGuestGuardSlab = 0;
inline constexpr std::uint64_t kGuestGuardOffset = 0;
inline constexpr std::uint32_t kGuestFirstDataSlab = 1;

// The guest's windows (hle/kernel.cpp: direct memory, and the same memory seen
// by the GPU at 0xff00000000 + its physical offset).
inline constexpr std::uint64_t kGuestRamBytes = 5ull << 30;              // 5 GiB of RAM
inline constexpr std::uint64_t kGuestGpuAliasBase = 0xff00000000ull;     // map_gpu_alias
inline constexpr std::uint64_t kGuestGpuAliasBytes = 4ull << 30;         // its window is 4 GiB
// What the 32 slabs can hold with the guard page taken out: 31 x 256 MiB.
inline constexpr std::uint64_t kGuestAddressableBytes = (kGuestSlabCount - kGuestFirstDataSlab) * std::uint64_t{kGuestSlabBytes};

// Size of the page table's buffer for `tables` L2 tables. The L1 is first, then
// one zero table (the guard's, shared by every unmapped region), then the used
// regions' tables in ascending order.
inline constexpr std::uint64_t kPageL1Bytes = std::uint64_t{kPageL1Entries} * 4;
inline constexpr std::uint64_t kPageL2Bytes = std::uint64_t{kPageL2Entries} * 8;

// What one L2 entry means to the shader: uint2(slab, page byte offset).
struct GuestSlot {
    std::uint32_t slab = kGuestGuardSlab;
    std::uint32_t offset = static_cast<std::uint32_t>(kGuestGuardOffset);
    [[nodiscard]] bool operator==(const GuestSlot& other) const { return slab == other.slab && offset == other.offset; }
    [[nodiscard]] bool operator!=(const GuestSlot& other) const { return !(*this == other); }
    [[nodiscard]] bool guard() const { return slab == kGuestGuardSlab && offset == kGuestGuardOffset; }
};

// ---- the page table (portable) ---------------------------------------------------

// One guest range the plan mapped, for the report and for the tests.
struct GuestWindow {
    std::uint64_t va = 0;          // where the guest sees it
    std::uint64_t bytes = 0;
    std::uint32_t slab = 0;        // its first slab
    std::uint64_t slab_offset = 0;  // ... and the byte in it
    bool mirror = false;           // true: the same slabs at a second VA (the GPU alias)
    const char* name = "";
};

// The two tables, built on the build machine and uploaded as they are.
class GuestPageTable {
public:
    GuestPageTable();

    // Maps [va, va + bytes) to `slab` starting at `slab_offset`. False (and
    // nothing changed) when the range crosses a 4 GiB boundary, is not a whole
    // number of pages, runs past the slab, names a slab that does not exist,
    // or touches a page that is already mapped.
    bool MapRange(std::uint64_t va, std::uint64_t bytes, std::uint32_t slab, std::uint64_t slab_offset);

    // Maps [va, va + bytes) into the plan's own layout: page `first_page` of the
    // guest at slab kGuestFirstDataSlab + first_page / kSlabPages. This is what a
    // boot uses (PlanGuestMemory); MapRange is the explicit form, one slab at a
    // time. Same refusals, plus one range may span several slabs by design here.
    bool MapLinear(std::uint64_t va, std::uint64_t bytes, std::uint64_t first_page);

    // Maps the second VA of an already mapped range onto the same slabs: the
    // GPU's alias reads and writes the title's own memory, not a copy of it.
    bool MapAlias(std::uint64_t alias_va, std::uint64_t va, std::uint64_t bytes);

    // The entry the shader reads for `va` (the guard page when unmapped). The
    // region index is clamped exactly as the shader clamps it.
    [[nodiscard]] GuestSlot SlotAt(std::uint64_t va) const;

    // One guest range that a single D3D12 buffer view can describe: the slab it
    // lives in and its byte offset there. False when the range is not mapped, is
    // empty, or spans two slabs or two pages that are not adjacent in one slab -
    // a view can only name one resource, and binding the wrong one is the failure
    // this whole path exists to avoid. `offset` is the byte offset of `va`
    // inside `slab`.
    [[nodiscard]] bool ContiguousSlab(std::uint64_t va, std::uint64_t bytes, std::uint32_t* slab,
                                      std::uint64_t* offset) const;
    [[nodiscard]] bool IsMapped(std::uint64_t va) const;
    [[nodiscard]] bool RegionMapped(std::uint32_t region) const;

    [[nodiscard]] std::uint64_t MappedBytes() const { return mapped_bytes_; }
    [[nodiscard]] std::uint32_t L2Tables() const { return static_cast<std::uint32_t>(tables_.size()); }
    // The tables as the page-table buffer holds them: the L1 (16 KiB), the shared
    // zero table (512 KiB), then each used region's table, ascending.
    [[nodiscard]] std::vector<std::uint8_t> Bytes() const;
    [[nodiscard]] std::uint64_t BufferBytes() const { return kPageL1Bytes + kPageL2Bytes * (1 + tables_.size()); }
    // Where region `r`'s L2 table sits, in bytes, for the tests and the log.
    [[nodiscard]] std::uint64_t TableOffset(std::uint32_t region) const;

private:
    struct Table {
        std::vector<GuestSlot> entries;
        std::uint64_t offset = 0;
    };
    // Assigns the byte offset of a region's table (L1, then zero table, then
    // each used region) and rewrites the L1's dword for it.
    void Place(std::uint32_t region, Table& table);
    [[nodiscard]] bool FreeRange(std::uint64_t va, std::uint64_t bytes) const;

    std::vector<std::uint32_t> l1_;             // kPageL1Entries, 8-byte-entry index of the region's table
    std::vector<GuestSlot> zero_;               // kPageL2Entries guard entries, shared by every unused region
    std::map<std::uint32_t, Table> tables_;     // region -> its table
    std::uint64_t mapped_bytes_ = 0;
};

// ---- the guest's windows ---------------------------------------------------------

// The plan a boot makes: RAM in slabs 1.., the GPU alias over the same slabs,
// the guard page reserved, and how much of the 32-slab table it came to.
struct GuestPlan {
    GuestPageTable table;
    std::vector<GuestWindow> windows;
    std::uint32_t slabs_resident = 1;  // the guard page's slab, always
    std::string refused;               // non-empty: the plan did not fit, and why
    [[nodiscard]] bool ok() const { return refused.empty(); }
    [[nodiscard]] std::string Report() const;
};

// `ram_bytes` of direct memory from 0, and the alias window over its first
// `alias_bytes` bytes. SlotFor() places page n at slab 1 + n / kSlabPages.
GuestPlan PlanGuestMemory(std::uint64_t ram_bytes = kGuestRamBytes, std::uint64_t alias_bytes = kGuestGpuAliasBytes);

// ---- slab runs, for host_gpu_mem_write / fill / copy -----------------------------

// A guest range, cut where it moves to another slab: one buffer, one offset,
// one byte count, so an upload is a list of copies rather than a walk.
struct GuestRun {
    std::uint32_t slab = 0;
    std::uint32_t offset = 0;
    std::size_t bytes = 0;
};

// Cuts [va, va + bytes) into runs. False when any page of it is unmapped (the
// guard page is not a place to write the title's data) - the caller counts the
// refusal instead of writing somewhere harmless but wrong.
bool SlabRuns(const GuestPageTable& table, std::uint64_t va, std::size_t bytes, std::vector<GuestRun>* out);

}  // namespace bbx::d3d12

#ifdef _WIN32

#include "src/host/d3d12/bbx_d3d12.h"

namespace bbx::d3d12 {

class Budget;
class Device;

// The guest's memory on the device: the page table's buffer and its SRV, and the
// 32 slab UAVs. A slab's 256 MiB is created when the guest first touches it, and
// an untouched slot holds the guard buffer's UAV instead, so all 32 descriptors
// are valid from the first dispatch while the empty table costs 64 KiB rather
// than 8 GiB of the console's 5,120 MiB budget.
//
// The views are made in a heap of this object's own that is *not* shader-visible
// (D3D12_DESCRIPTOR_HEAP_FLAG_NONE), and copied into the device's single
// shader-visible CBV/SRV/UAV heap at `visible_base`: a command list takes one
// CBV/SRV/UAV heap, and CopyDescriptorsSimple refuses a shader-visible source
// heap, so "many heaps" is not an option the API leaves open. The copy is why
// the guest arena is an index space (bbx_guest_root.h's rule 4) rather than a
// heap per subsystem.
class GuestMemory {
public:
    GuestMemory() = default;
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    // `plan` decides what is resident and what the tables say; the buffer is
    // created here and never rewritten, because the plan's windows are fixed
    // for a boot (a mapping that changes is a new plan).
    // `budget` is the device's own (Device::Memory()): a slab asks it first.
    // `visible_base` is where this object's kDescriptorCount descriptors sit in
    // the device's shader-visible CBV/SRV/UAV heap; the caller reserves them
    // (GuestHeapLayout::kSpace3Count, the arena's first region).
    Status Create(Device& device, Budget& budget, const GuestPlan& plan, std::uint32_t visible_base = 0);
    void Destroy();
    [[nodiscard]] bool Ready() const { return page_table_ != nullptr; }

    // The views as this object made them (a heap the CPU writes and the GPU
    // cannot see): what a copy names as its source.
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE PageTableSrv() const { return page_table_srv_; }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE SlabUav(std::uint32_t slab) const;
    // The same descriptors where a command list can see them (the device's
    // shader-visible heap at `visible_base`): the guest root signature's space-3
    // table points here.
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE TableHandle() const;
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE VisibleHandle(std::uint32_t index) const;
    [[nodiscard]] std::uint32_t VisibleBase() const { return visible_base_; }
    // How many descriptors the space-3 region takes (the page table plus every
    // slab), which is what the caller had to reserve.
    static constexpr std::uint32_t kDescriptorCount = 1 + kGuestSlabCount;

    // The plan's page table, as the guest's memory is laid out: what a run list
    // is cut with, and what an indirect dispatch's three dwords are located by.
    [[nodiscard]] const GuestPageTable& Table() const { return table_; }
    // The buffer a slab is, once it is resident: a dispatch whose arguments (or
    // whose whole access) name a slab through the page table can hand D3D12 the
    // slab itself rather than a copy.
    [[nodiscard]] ID3D12Resource* SlabBuffer(std::uint32_t slab) const;

    // Makes `slab` resident (a 256 MiB committed buffer, counted in the budget)
    // and points its UAV at it. Called the first time an upload touches it.
    Status EnsureSlab(std::uint32_t slab);

    // host_gpu_mem_write / mem_fill / mem_copy: batched asynchronously through an
    // 8 MiB persistently-mapped upload ring (or pure GPU-to-GPU for CopyGuest) so
    // hot-path guest writes never stall the CPU. Call Flush() before compute,
    // graphics, readback, or fence signal to preserve strict queue order.
    Status WriteGuest(const GuestPageTable& table, std::uint64_t va, const void* data, std::size_t bytes);
    Status FillGuest(const GuestPageTable& table, std::uint64_t va, std::uint32_t value, std::size_t bytes);
    Status CopyGuest(const GuestPageTable& table, std::uint64_t dst_va, std::uint64_t src_va, std::size_t bytes);
    // The other way: flushes pending writes, then reads a slab run into host memory.
    Status ReadGuest(const GuestPageTable& table, std::uint64_t va, void* out, std::size_t bytes);

    // Flushes any open batched upload/copy command list to the graphics queue.
    Status Flush();
    [[nodiscard]] bool HasPending() const { return batch_open_; }
    // Reclaims completed command allocators once the GPU fence passes them.
    void OnFrame(std::uint64_t completed);

    [[nodiscard]] std::uint32_t SlabsResident() const { return slabs_resident_; }
    [[nodiscard]] std::uint64_t BytesCommitted() const { return committed_bytes_; }
    [[nodiscard]] std::string Describe() const;

private:
    static constexpr std::uint32_t kUploadSlices = 4;
    static constexpr std::uint64_t kUploadSliceBytes = 2ull << 20;  // 4 x 2 MiB = 8 MiB ring
    static constexpr std::uint64_t kUploadRingBytes = kUploadSlices * kUploadSliceBytes;
    static constexpr std::uint64_t kCopyScratchBytes = 256ull << 10;
    static constexpr std::uint32_t kMaxBatchOps = 256;

    struct PendingAllocator {
        ComPtr<ID3D12CommandAllocator> allocator;
        std::uint64_t fence = 0;
    };

    D3D12_CPU_DESCRIPTOR_HANDLE VisibleCpu(std::uint32_t index) const;
    Status EnsureUploadRing();
    Status EnsureCopyScratch();
    Status ReserveUpload(std::size_t bytes, std::uint64_t* out_offset);
    Status BeginBatch();
    void TransitionSlab(std::uint32_t slab, D3D12_RESOURCE_STATES target_state);

    Device* owner_ = nullptr;  // the port's Device: the queue and the fence
    ID3D12Device* device_ = nullptr;
    Budget* budget_ = nullptr;
    ComPtr<ID3D12Resource> page_table_;
    ComPtr<ID3D12Resource> guard_;
    ComPtr<ID3D12Resource> upload_;        // kUploadRingBytes, HEAP_TYPE_UPLOAD, persistently mapped
    std::uint8_t* upload_mapped_ = nullptr;
    std::uint32_t upload_slice_ = 0;
    std::uint64_t upload_slice_cursor_ = 0;
    std::array<std::uint64_t, kUploadSlices> upload_slice_fence_{};
    ComPtr<ID3D12Resource> copy_scratch_;  // kCopyScratchBytes, HEAP_TYPE_DEFAULT (same-slab copy)
    ComPtr<ID3D12Resource> readback_;      // HEAP_TYPE_READBACK
    std::uint64_t readback_bytes_ = 0;
    std::vector<ComPtr<ID3D12CommandAllocator>> free_allocators_;
    std::vector<PendingAllocator> pending_allocators_;
    ComPtr<ID3D12CommandAllocator> active_allocator_;
    ComPtr<ID3D12GraphicsCommandList> active_list_;
    bool batch_open_ = false;
    std::uint32_t batch_ops_ = 0;
    std::array<D3D12_RESOURCE_STATES, kGuestSlabCount> slab_batch_state_{};
    std::uint32_t slab_batch_dirty_mask_ = 0;
    std::vector<GuestRun> runs_scratch_;
    std::vector<GuestRun> dst_runs_scratch_;
    std::map<std::uint32_t, ComPtr<ID3D12Resource>> slabs_;
    GuestPageTable table_;               // the plan's table, copied: the run lists' source
    ComPtr<ID3D12DescriptorHeap> heap_;  // 1 SRV + 32 UAVs, FLAG_NONE (a copy's source)
    D3D12_CPU_DESCRIPTOR_HANDLE page_table_srv_{};
    std::uint32_t slab_uav_stride_ = 0;
    std::uint32_t visible_base_ = 0;   // where the copies live in the device's heap
    std::uint32_t visible_stride_ = 0; // the device heap's descriptor size
    std::uint32_t slabs_resident_ = 0;
    std::uint64_t committed_bytes_ = 0;
    std::uint64_t uploads_ = 0, upload_bytes_ = 0;
    std::uint64_t flushes_ = 0;
};

}  // namespace bbx::d3d12

#endif  // _WIN32
