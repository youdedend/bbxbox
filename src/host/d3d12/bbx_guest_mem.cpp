// SPDX-License-Identifier: GPL-3.0-or-later
// The guest's two-level page table and slab table: see bbx_guest_mem.h.
#include "host/d3d12/bbx_guest_mem.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace bbx::d3d12 {

namespace {

// The L1 dword for a table at `byte_offset`: the index of its first 8-byte entry,
// which is what the shader adds the page index to before multiplying by 8.
std::uint32_t EntryIndex(std::uint64_t byte_offset) { return static_cast<std::uint32_t>(byte_offset / 8); }

std::uint32_t RegionOf(std::uint64_t va) { return static_cast<std::uint32_t>(std::min<std::uint64_t>(va >> 32, kPageL1Entries - 1)); }

std::uint32_t PageInRegion(std::uint64_t va) { return static_cast<std::uint32_t>((va >> kPageShift) & (kPageL2Entries - 1)); }

// Where guest page `page` lives in the plan's layout: slabs 1.. of the table,
// 4096 pages each, the guard page's slab 0 never handed out.
GuestSlot SlotForPage(std::uint64_t page) {
    return GuestSlot{kGuestFirstDataSlab + static_cast<std::uint32_t>(page / kSlabPages),
                     static_cast<std::uint32_t>((page % kSlabPages) * kPageBytes)};
}

}  // namespace

// ---- GuestPageTable ----------------------------------------------------------------

GuestPageTable::GuestPageTable() {
    zero_.assign(kPageL2Entries, GuestSlot{});
}

bool GuestPageTable::RegionMapped(std::uint32_t region) const { return tables_.count(region) != 0; }

std::uint64_t GuestPageTable::TableOffset(std::uint32_t region) const {
    // The L1, then the shared zero table (what every unmapped region points at),
    // then the used regions in ascending order - so the bytes do not depend on
    // the order the windows were mapped in.
    std::uint64_t at = kPageL1Bytes + kPageL2Bytes;
    for (const auto& [r, table] : tables_) {
        (void)table;
        if (r == region) return at;
        at += kPageL2Bytes;
    }
    return kPageL1Bytes;  // unmapped: the zero table, whose every entry is the guard page
}

bool GuestPageTable::FreeRange(std::uint64_t va, std::uint64_t bytes) const {
    const auto it = tables_.find(RegionOf(va));
    if (it == tables_.end()) return true;
    for (std::uint64_t at = va; at < va + bytes; at += kPageBytes) {
        if (!it->second.entries[PageInRegion(at)].guard()) return false;
    }
    return true;
}

bool GuestPageTable::MapRange(std::uint64_t va, std::uint64_t bytes, std::uint32_t slab, std::uint64_t slab_offset) {
    if (bytes == 0 || va % kPageBytes != 0 || bytes % kPageBytes != 0 || slab_offset % kPageBytes != 0) return false;
    // A guest VA is two dwords in the shader: no range may carry out of its
    // region, or the L1 index it names is the wrong one.
    if (RegionOf(va) != RegionOf(va + bytes - 1)) return false;
    if (slab >= kGuestSlabCount || slab == kGuestGuardSlab) return false;  // slab 0 is the guard page's
    if (slab_offset + bytes > kGuestSlabBytes) return false;
    if (!FreeRange(va, bytes)) return false;

    auto& table = tables_[RegionOf(va)];
    if (table.entries.empty()) table.entries.assign(kPageL2Entries, GuestSlot{});
    for (std::uint64_t at = 0; at < bytes; at += kPageBytes) {
        table.entries[PageInRegion(va + at)] =
            GuestSlot{slab, static_cast<std::uint32_t>(slab_offset + at)};
    }
    mapped_bytes_ += bytes;
    return true;
}

bool GuestPageTable::MapLinear(std::uint64_t va, std::uint64_t bytes, std::uint64_t first_page) {
    if (bytes == 0 || va % kPageBytes != 0 || bytes % kPageBytes != 0) return false;
    if (RegionOf(va) != RegionOf(va + bytes - 1)) return false;
    if (!FreeRange(va, bytes)) return false;
    const std::uint64_t pages = bytes / kPageBytes;
    const std::uint64_t last_slab = kGuestFirstDataSlab + (first_page + pages - 1) / kSlabPages;
    if (last_slab >= kGuestSlabCount) return false;  // past the 32-slab table

    auto& table = tables_[RegionOf(va)];
    if (table.entries.empty()) table.entries.assign(kPageL2Entries, GuestSlot{});
    for (std::uint64_t page = 0; page < pages; ++page) {
        const GuestSlot slot = SlotForPage(first_page + page);
        table.entries[PageInRegion(va + page * kPageBytes)] = GuestSlot{slot.slab, slot.offset};
    }
    mapped_bytes_ += bytes;
    return true;
}

bool GuestPageTable::MapAlias(std::uint64_t alias_va, std::uint64_t va, std::uint64_t bytes) {
    if (bytes == 0 || alias_va % kPageBytes != 0 || va % kPageBytes != 0 || bytes % kPageBytes != 0) return false;
    if (RegionOf(alias_va) != RegionOf(alias_va + bytes - 1)) return false;
    for (std::uint64_t at = 0; at < bytes; at += kPageBytes) {
        if (SlotAt(va + at).guard()) return false;  // nothing to mirror
    }
    if (!FreeRange(alias_va, bytes)) return false;

    auto& table = tables_[RegionOf(alias_va)];
    if (table.entries.empty()) table.entries.assign(kPageL2Entries, GuestSlot{});
    for (std::uint64_t at = 0; at < bytes; at += kPageBytes) {
        table.entries[PageInRegion(alias_va + at)] = SlotAt(va + at);
    }
    mapped_bytes_ += bytes;
    return true;
}

bool GuestPageTable::ContiguousSlab(std::uint64_t va, std::uint64_t bytes, std::uint32_t* slab,
                                    std::uint64_t* offset) const {
    if (slab == nullptr || offset == nullptr || bytes == 0) return false;
    // 4-byte aligned: every view this port makes is dword addressed.
    if ((va & 3u) != 0) return false;
    if (va + bytes < va) return false;  // wrapped
    const GuestSlot first = SlotAt(va);
    if (first.guard()) return false;
    // Walk the pages the range touches. The first page may start before `va`
    // (a range at a page's middle), so its own offset is the page's.
    const std::uint64_t first_page = va & ~(kPageBytes - 1);
    std::uint64_t page = first_page;
    std::uint64_t expected = first.offset;  // where this page starts in the slab
    while (page < va + bytes) {
        const GuestSlot slot = SlotAt(page);
        if (slot.guard() || slot.slab != first.slab) return false;
        if (slot.offset != expected) return false;  // not adjacent in the slab
        expected += kPageBytes;
        if (page + kPageBytes < page) return false;
        page += kPageBytes;
    }
    *slab = first.slab;
    *offset = static_cast<std::uint64_t>(first.offset) + (va - first_page);
    // The last page's data must fit the slab too.
    if (*offset + bytes > kGuestSlabBytes) return false;
    return true;
}

GuestSlot GuestPageTable::SlotAt(std::uint64_t va) const {
    const auto it = tables_.find(RegionOf(va));
    if (it == tables_.end()) return GuestSlot{};
    return it->second.entries[PageInRegion(va)];
}

bool GuestPageTable::IsMapped(std::uint64_t va) const { return !SlotAt(va).guard(); }

std::vector<std::uint8_t> GuestPageTable::Bytes() const {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(BufferBytes()), 0);
    const auto put_u32 = [&out](std::uint64_t at, std::uint32_t v) { std::memcpy(out.data() + at, &v, 4); };
    const auto put_slot = [&put_u32](std::uint64_t at, const GuestSlot& slot) {
        put_u32(at, slot.slab);
        put_u32(at + 4, slot.offset);
    };
    // The L1: every region first points at the zero table, whose entries are all
    // the guard page.
    const std::uint32_t zero_index = EntryIndex(kPageL1Bytes);
    for (std::uint32_t r = 0; r < kPageL1Entries; ++r) put_u32(std::size_t{r} * 4, zero_index);
    for (const auto& [region, table] : tables_) {
        put_u32(std::size_t{region} * 4, EntryIndex(TableOffset(region)));
        const std::uint64_t base = TableOffset(region);
        for (std::uint32_t i = 0; i < kPageL2Entries; ++i) put_slot(base + std::size_t{i} * 8, table.entries[i]);
    }
    const std::uint64_t zero_base = kPageL1Bytes;
    for (std::uint32_t i = 0; i < kPageL2Entries; ++i) put_slot(zero_base + std::size_t{i} * 8, zero_[i]);
    return out;
}

// ---- the plan ---------------------------------------------------------------------

GuestPlan PlanGuestMemory(std::uint64_t ram_bytes, std::uint64_t alias_bytes) {
    GuestPlan plan;
    const auto refuse = [&plan](const std::string& why) {
        plan.refused = why;
        return plan;
    };
    ram_bytes = std::min(ram_bytes, kGuestRamBytes);
    if (ram_bytes == 0) return refuse("no RAM");
    if (ram_bytes > kGuestAddressableBytes) {
        return refuse("5 GiB of RAM does not fit 31 slabs of 256 MiB");
    }
    for (std::uint64_t at = 0; at < ram_bytes; at += kL1RegionBytes) {
        const std::uint64_t bytes = std::min(ram_bytes - at, kL1RegionBytes);
        const GuestSlot first = SlotForPage(at / kPageBytes);
        if (!plan.table.MapLinear(at, bytes, at / kPageBytes)) {
            return refuse("RAM does not map at 0x" + std::to_string(at >> 20) + " MiB");
        }
        plan.windows.push_back(GuestWindow{at, bytes, first.slab, first.offset, false, at ? "ram above 4 GiB" : "ram"});
    }
    // The GPU's alias is the same memory at a second address (hle/kernel.cpp's
    // map_gpu_alias): the same slabs, so GX's writes land where the guest reads.
    alias_bytes = std::min({alias_bytes, ram_bytes, kGuestGpuAliasBytes});
    if (alias_bytes != 0) {
        if (!plan.table.MapAlias(kGuestGpuAliasBase, 0, alias_bytes)) {
            return refuse("the GPU alias does not mirror the first 4 GiB of RAM");
        }
        plan.windows.push_back(
            GuestWindow{kGuestGpuAliasBase, alias_bytes, SlotForPage(0).slab, SlotForPage(0).offset, true, "gpu alias"});
    }
    plan.slabs_resident = kGuestFirstDataSlab + static_cast<std::uint32_t>((ram_bytes + kGuestSlabBytes - 1) / kGuestSlabBytes);
    return plan;
}

GuestPlan PlanConsoleGuestMemory() {
    GuestPlan plan;
    const auto refuse = [&plan](const std::string& why) {
        plan.refused = why;
        return plan;
    };
    // Direct memory identity window [0x100000000, 0x240000000) -> slabs 1..20 (5 GiB).
    constexpr std::uint64_t kDmemBase = 0x100000000ull;
    for (std::uint64_t at = 0; at < kGuestRamBytes; at += kL1RegionBytes) {
        const std::uint64_t va = kDmemBase + at;
        const std::uint64_t bytes = std::min(kGuestRamBytes - at, kL1RegionBytes);
        const GuestSlot first = SlotForPage(at / kPageBytes);
        if (!plan.table.MapLinear(va, bytes, at / kPageBytes)) {
            return refuse("dmem does not map at 0x" + std::to_string(va >> 20) + " MiB");
        }
        plan.windows.push_back(
            GuestWindow{va, bytes, first.slab, first.offset, false, at ? "dmem above 8 GiB" : "dmem"});
    }
    // GPU alias window [0xff00000000, 0xff00000000 + 4 GiB) mirrors the first 4 GiB of dmem (slabs 1..16).
    if (!plan.table.MapAlias(kGuestGpuAliasBase, kDmemBase, kGuestGpuAliasBytes)) {
        return refuse("the GPU alias does not mirror the first 4 GiB of dmem");
    }
    plan.windows.push_back(
        GuestWindow{kGuestGpuAliasBase, kGuestGpuAliasBytes, SlotForPage(0).slab, SlotForPage(0).offset, true, "gpu alias"});
    // Low window [0, 2.75 GiB) -> slabs 21..31 (11 slabs) for eboot / flexible mappings below 4 GiB.
    constexpr std::uint64_t kLowFirstPage = kGuestRamBytes / kPageBytes;
    constexpr std::uint64_t kLowBytes = kGuestAddressableBytes - kGuestRamBytes;
    if (kLowBytes != 0) {
        const GuestSlot low_first = SlotForPage(kLowFirstPage);
        if (!plan.table.MapLinear(0, kLowBytes, kLowFirstPage)) {
            return refuse("low guest window does not map at 0x0");
        }
        plan.windows.push_back(GuestWindow{0, kLowBytes, low_first.slab, low_first.offset, false, "low window"});
    }
    plan.slabs_resident = kGuestSlabCount;
    return plan;
}

std::string GuestPlan::Report() const {
    std::ostringstream out;
    if (!ok()) return "guest memory: refused: " + refused;
    out << "guest memory: " << windows.size() << " window(s), " << (table.MappedBytes() >> 20) << " MiB mapped, "
        << slabs_resident << "/" << kGuestSlabCount << " slab(s) resident (" << (table.BufferBytes() >> 10) << " KiB of page table)";
    for (const GuestWindow& w : windows) {
        out << "\n  " << w.name << ": 0x" << std::hex << w.va << std::dec << " + " << (w.bytes >> 20) << " MiB -> slabs "
            << w.slab << ".. at offset " << w.slab_offset << (w.mirror ? " (mirror)" : "");
    }
    return out.str();
}

// ---- slab runs --------------------------------------------------------------------

bool SlabRuns(const GuestPageTable& table, std::uint64_t va, std::size_t bytes, std::vector<GuestRun>* out) {
    out->clear();
    for (std::size_t done = 0; done < bytes;) {
        const std::uint64_t at = va + done;
        const GuestSlot slot = table.SlotAt(at);
        if (slot.guard()) return false;  // the guard page is not a place for the title's data
        const std::uint64_t in_page = at & (kPageBytes - 1);
        const std::uint64_t offset = slot.offset + in_page;
        if (offset >= kGuestSlabBytes) return false;
        const std::size_t take = static_cast<std::size_t>(
            std::min<std::uint64_t>(bytes - done, std::min<std::uint64_t>(kPageBytes - in_page, kGuestSlabBytes - offset)));
        if (take == 0) return false;
        if (!out->empty() && out->back().slab == slot.slab && out->back().offset + out->back().bytes == offset) {
            out->back().bytes += take;
        } else {
            if (out->size() >= kGuestSlabCount) return false;
            out->push_back(GuestRun{slot.slab, static_cast<std::uint32_t>(offset), take});
        }
        done += take;
    }
    return !out->empty();
}

}  // namespace bbx::d3d12

#ifdef _WIN32

// ---- the device half ---------------------------------------------------------------
// Everything below is Windows-only and type-checked by tools/syntax_check.sh; the
// rules it implements are the portable half's, applied to real buffers.
#include "src/host/d3d12/device.h"

namespace bbx::d3d12 {

namespace {

constexpr std::uint64_t kGuardBytes = kPageBytes;
// host_gpu_mem_write's documented ceiling (host/gpu.h): one upload at a time.
constexpr std::uint64_t kStagingBytes = 64ull << 10;

D3D12_RESOURCE_DESC RawBuffer(std::uint64_t bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    // D3D12 requires DXGI_FORMAT_UNKNOWN on BUFFER resources; R32_TYPELESS is
    // specified on the raw SRV/UAV descriptors created over the buffer.
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

}  // namespace

GuestMemory::~GuestMemory() { Destroy(); }

void GuestMemory::Destroy() {
    if (owner_ != nullptr) {
        if (batch_open_) {
            (void)Flush();
        }
        (void)owner_->WaitIdle();
    }
    if (upload_ != nullptr && upload_mapped_ != nullptr) {
        upload_->Unmap(0, nullptr);
        upload_mapped_ = nullptr;
    }
    slabs_.clear();
    free_allocators_.clear();
    pending_allocators_.clear();
    active_list_.Reset();
    active_allocator_.Reset();
    copy_scratch_.Reset();
    owner_ = nullptr;
    budget_ = nullptr;
    page_table_.Reset();
    guard_.Reset();
    upload_.Reset();
    readback_.Reset();
    readback_bytes_ = 0;
    heap_.Reset();
    device_ = nullptr;
    upload_slice_ = 0;
    upload_slice_cursor_ = 0;
    upload_slice_fence_.fill(0);
    batch_open_ = false;
    batch_ops_ = 0;
    slab_batch_state_.fill(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    slab_batch_dirty_mask_ = 0;
    slabs_resident_ = 0;
    committed_bytes_ = 0;
    uploads_ = 0;
    upload_bytes_ = 0;
    flushes_ = 0;
}

D3D12_CPU_DESCRIPTOR_HANDLE GuestMemory::SlabUav(std::uint32_t slab) const {
    // Descriptor 0 is the page table's SRV; u0..u31 follow it.
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(1 + (slab < kGuestSlabCount ? slab : 0)) * slab_uav_stride_;
    return handle;
}

Status GuestMemory::Create(Device& device, Budget& budget, const GuestPlan& plan, std::uint32_t visible_base) {
    Destroy();
    if (!device.Get()) return Status::kUnavailable;
    if (!plan.ok()) return Status::kInvalidArgument;
    owner_ = &device;
    device_ = device.Get();
    budget_ = &budget;
    table_ = plan.table;
    slab_batch_state_.fill(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    // The page table: one committed buffer, written once per boot, read by every
    // dispatch. The plan's tables are ~1-2 MiB and uploaded via a dedicated staging buffer.
    const std::vector<std::uint8_t> bytes = plan.table.Bytes();
    if (bytes.empty() || bytes.size() % 4 != 0) return Status::kInvalidArgument;
    D3D12_RESOURCE_DESC desc = RawBuffer(bytes.size());
    if (FAILED(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                nullptr, IID_PPV_ARGS(page_table_.GetAddressOf())))) {
        return Status::kOutOfMemory;
    }
    ComPtr<ID3D12Resource> upload;
    if (FAILED(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, IID_PPV_ARGS(upload.GetAddressOf())))) {
        return Status::kOutOfMemory;
    }
    {
        void* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, &mapped))) return Status::kOutOfMemory;
        std::memcpy(mapped, bytes.data(), bytes.size());
        upload->Unmap(0, nullptr);
    }
    // The guard page: 64 KiB that is never the title's data. Slab 0's UAV and
    // every slab not yet resident point at it, so all 32 descriptors are valid
    // from the first dispatch while an untouched table costs 64 KiB, not 8 GiB.
    const D3D12_RESOURCE_DESC guard_desc = RawBuffer(kGuardBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (FAILED(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &guard_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                nullptr, IID_PPV_ARGS(guard_.GetAddressOf())))) {
        return Status::kOutOfMemory;
    }
    // The views are made here, in a heap the CPU owns: CopyDescriptorsSimple
    // refuses a shader-visible source heap, so the source and the destination of
    // the one copy that publishes them are two heaps, not one.
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = GuestMemory::kDescriptorCount;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(heap_.GetAddressOf())))) return Status::kOutOfMemory;
    // Where those descriptors are published: the device's one shader-visible
    // CBV/SRV/UAV heap, at the offset the arena reserved for space 3.
    if (device.CbvSrvUavHeap() == nullptr) return Status::kInvalidArgument;
    if (visible_base + GuestMemory::kDescriptorCount > Device::kCbvSrvUavDescriptors) return Status::kInvalidArgument;
    visible_base_ = visible_base;
    visible_stride_ = device.CbvSrvUavStride();
    slab_uav_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    page_table_srv_ = heap_->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = static_cast<UINT>(bytes.size() / 4);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device_->CreateShaderResourceView(page_table_.Get(), &srv, page_table_srv_);
    for (std::uint32_t slab = 0; slab < kGuestSlabCount; ++slab) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = static_cast<UINT>(kGuardBytes / 4);
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device_->CreateUnorderedAccessView(guard_.Get(), nullptr, &uav, SlabUav(slab));
    }
    // One copy publishes all 33: the page table's SRV and the guard UAV every
    // slab starts as. A slab that turns resident later copies its own slot
    // again (EnsureSlab), so the shader-visible table always names the buffer
    // the table's entry implies.
    device_->CopyDescriptorsSimple(GuestMemory::kDescriptorCount, VisibleCpu(0), page_table_srv_,
                                   D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // The copy that fills the page table, and the fence value that says it is
    // done: the tables are right before any dispatch can name them.
    ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf())))) {
        return Status::kOutOfMemory;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(list.GetAddressOf())))) {
        return Status::kOutOfMemory;
    }
    list->CopyBufferRegion(page_table_.Get(), 0, upload.Get(), 0, bytes.size());
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    const std::uint64_t value = owner_->ExecuteAndSignal(lists, 1);
    if (!owner_->WaitFor(value)) return owner_->DeviceLost() ? Status::kDeviceLost : Status::kUnavailable;

    // Keep the boot allocator and list in the pool so future async batches reuse them.
    active_list_ = std::move(list);
    free_allocators_.push_back(std::move(allocator));

    committed_bytes_ = bytes.size() + kGuardBytes;
    slabs_resident_ = 1;  // only the guard slab is resident until EnsureSlab commits a data slab
    return Status::kOk;
}

ID3D12Resource* GuestMemory::SlabBuffer(std::uint32_t slab) const {
    if (slab == kGuestGuardSlab) return guard_.Get();
    const auto it = slabs_.find(slab);
    return it == slabs_.end() ? nullptr : it->second.Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE GuestMemory::VisibleCpu(std::uint32_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = owner_->CbvSrvUavHeap()->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(visible_base_ + index) * visible_stride_;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE GuestMemory::TableHandle() const { return VisibleHandle(0); }

D3D12_GPU_DESCRIPTOR_HANDLE GuestMemory::VisibleHandle(std::uint32_t index) const {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = owner_->CbvSrvUavHeap()->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(visible_base_ + index) * visible_stride_;
    return handle;
}

Status GuestMemory::EnsureSlab(std::uint32_t slab) {
    if (!device_ || slab >= kGuestSlabCount) return Status::kInvalidArgument;
    if (slab == kGuestGuardSlab || slabs_.count(slab) != 0) return Status::kOk;  // the guard buffer is slab 0's
    // The budget answers first: 256 MiB out of the console's 5,120 MiB is not
    // this file's decision to make.
    if (!budget_->Allocate(Owner::kGuest, kGuestSlabBytes).ok) {
        return Status::kOutOfMemory;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> buffer;
    const D3D12_RESOURCE_DESC desc = RawBuffer(kGuestSlabBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(buffer.GetAddressOf())))) {
        budget_->Free(Owner::kGuest, kGuestSlabBytes);
        return Status::kOutOfMemory;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = static_cast<UINT>(kGuestSlabBytes / 4);
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device_->CreateUnorderedAccessView(buffer.Get(), nullptr, &uav, SlabUav(slab));
    // Publish it: a slot the shader-visible table still pointed at the guard
    // buffer would read zeros where the guest wrote its data.
    device_->CopyDescriptorsSimple(1, VisibleCpu(1 + slab), SlabUav(slab), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    slabs_.emplace(slab, std::move(buffer));
    slabs_resident_ = static_cast<std::uint32_t>(slabs_.size() + 1);
    committed_bytes_ += kGuestSlabBytes;
    return Status::kOk;
}

std::string GuestMemory::Describe() const {
    std::ostringstream out;
    out << "guest memory: " << (committed_bytes_ >> 20) << " MiB committed, " << slabs_resident_ << "/" << kGuestSlabCount
        << " slab(s) resident, " << uploads_ << " upload(s) (" << flushes_ << " batch(es)), " << (upload_bytes_ >> 10)
        << " KiB copied";
    return out.str();
}

void GuestMemory::OnFrame(std::uint64_t completed) {
    if (pending_allocators_.empty()) return;
    std::size_t write = 0;
    for (std::size_t read = 0; read < pending_allocators_.size(); ++read) {
        if (pending_allocators_[read].fence <= completed) {
            free_allocators_.push_back(std::move(pending_allocators_[read].allocator));
        } else {
            if (write != read) pending_allocators_[write] = std::move(pending_allocators_[read]);
            ++write;
        }
    }
    pending_allocators_.resize(write);
}

Status GuestMemory::EnsureUploadRing() {
    if (upload_ != nullptr && upload_mapped_ != nullptr) return Status::kOk;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const D3D12_RESOURCE_DESC desc = RawBuffer(kUploadRingBytes);
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, IID_PPV_ARGS(upload_.GetAddressOf())))) {
        return Status::kOutOfMemory;
    }
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    if (FAILED(upload_->Map(0, &no_read, &mapped)) || mapped == nullptr) {
        upload_.Reset();
        return Status::kOutOfMemory;
    }
    upload_mapped_ = static_cast<std::uint8_t*>(mapped);
    upload_slice_ = 0;
    upload_slice_cursor_ = 0;
    upload_slice_fence_.fill(0);
    return Status::kOk;
}

Status GuestMemory::EnsureCopyScratch() {
    if (copy_scratch_ != nullptr) return Status::kOk;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_RESOURCE_DESC desc = RawBuffer(kCopyScratchBytes);
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(copy_scratch_.GetAddressOf())))) {
        return Status::kOutOfMemory;
    }
    return Status::kOk;
}

Status GuestMemory::ReserveUpload(std::size_t bytes, std::uint64_t* out_offset) {
    if (bytes == 0 || bytes > kUploadSliceBytes || out_offset == nullptr) return Status::kInvalidArgument;
    const std::uint64_t aligned = (static_cast<std::uint64_t>(bytes) + 15ull) & ~15ull;
    if (upload_slice_cursor_ + aligned > kUploadSliceBytes) {
        if (batch_open_) {
            const Status flushed = Flush();
            if (flushed != Status::kOk) return flushed;
        }
        upload_slice_fence_[upload_slice_] = owner_->SubmittedValue();
        upload_slice_ = (upload_slice_ + 1u) % kUploadSlices;
        upload_slice_cursor_ = 0;
        const std::uint64_t wait_fence = upload_slice_fence_[upload_slice_];
        if (wait_fence != 0 && owner_->CompletedValue() < wait_fence) {
            if (!owner_->WaitFor(wait_fence)) {
                return owner_->DeviceLost() ? Status::kDeviceLost : Status::kUnavailable;
            }
            OnFrame(owner_->CompletedValue());
        }
    }
    *out_offset = static_cast<std::uint64_t>(upload_slice_) * kUploadSliceBytes + upload_slice_cursor_;
    upload_slice_cursor_ += aligned;
    return Status::kOk;
}

Status GuestMemory::BeginBatch() {
    if (batch_open_) return Status::kOk;
    OnFrame(owner_->CompletedValue());
    ComPtr<ID3D12CommandAllocator> allocator;
    if (!free_allocators_.empty()) {
        allocator = std::move(free_allocators_.back());
        free_allocators_.pop_back();
        if (FAILED(allocator->Reset())) return Status::kUnavailable;
    } else {
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(allocator.GetAddressOf())))) {
            return Status::kOutOfMemory;
        }
    }
    if (!active_list_) {
        if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(active_list_.GetAddressOf())))) {
            free_allocators_.push_back(std::move(allocator));
            return Status::kOutOfMemory;
        }
    } else {
        if (FAILED(active_list_->Reset(allocator.Get(), nullptr))) {
            free_allocators_.push_back(std::move(allocator));
            return Status::kUnavailable;
        }
    }
    active_allocator_ = std::move(allocator);
    slab_batch_state_.fill(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    slab_batch_dirty_mask_ = 0;
    batch_open_ = true;
    batch_ops_ = 0;
    return Status::kOk;
}

void GuestMemory::TransitionSlab(std::uint32_t slab, D3D12_RESOURCE_STATES target_state) {
    if (slab >= kGuestSlabCount) return;
    const D3D12_RESOURCE_STATES current = slab_batch_state_[slab];
    if (current == target_state) return;
    ID3D12Resource* target = SlabBuffer(slab);
    if (target == nullptr) return;
    const D3D12_RESOURCE_BARRIER b = Transition(target, current, target_state);
    active_list_->ResourceBarrier(1, &b);
    slab_batch_state_[slab] = target_state;
    if (target_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        slab_batch_dirty_mask_ &= ~(1u << slab);
    } else {
        slab_batch_dirty_mask_ |= (1u << slab);
    }
}

Status GuestMemory::Flush() {
    if (!batch_open_) return Status::kOk;
    D3D12_RESOURCE_BARRIER barriers[kGuestSlabCount];
    UINT barrier_count = 0;
    std::uint32_t mask = slab_batch_dirty_mask_;
    for (std::uint32_t slab = 0; mask != 0 && slab < kGuestSlabCount; ++slab, mask >>= 1u) {
        if ((mask & 1u) == 0u) continue;
        ID3D12Resource* res = SlabBuffer(slab);
        if (res != nullptr && slab_batch_state_[slab] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            barriers[barrier_count++] =
                Transition(res, slab_batch_state_[slab], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        slab_batch_state_[slab] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    slab_batch_dirty_mask_ = 0;
    if (barrier_count > 0) {
        active_list_->ResourceBarrier(barrier_count, barriers);
    }
    const HRESULT closed = active_list_->Close();
    batch_open_ = false;
    batch_ops_ = 0;
    if (FAILED(closed)) {
        free_allocators_.push_back(std::move(active_allocator_));
        return FromHresult(closed);
    }
    ID3D12CommandList* lists[] = {active_list_.Get()};
    const std::uint64_t fence = owner_->ExecuteAndSignal(lists, 1);
    pending_allocators_.push_back(PendingAllocator{std::move(active_allocator_), fence});
    ++flushes_;
    return owner_->DeviceLost() ? Status::kDeviceLost : Status::kOk;
}

Status GuestMemory::WriteGuest(const GuestPageTable& table, std::uint64_t va, const void* data, std::size_t bytes) {
    if (!device_ || !data || bytes == 0 || bytes % 4 != 0) return Status::kInvalidArgument;
    if (const Status ring = EnsureUploadRing(); ring != Status::kOk) return ring;

    const auto* src = static_cast<const std::uint8_t*>(data);
    std::size_t done = 0;
    while (done < bytes) {
        const std::size_t chunk = std::min<std::size_t>(bytes - done, static_cast<std::size_t>(kUploadSliceBytes));
        if (!SlabRuns(table, va + done, chunk, &runs_scratch_)) return Status::kInvalidArgument;
        for (const GuestRun& run : runs_scratch_) {
            if (const Status status = EnsureSlab(run.slab); status != Status::kOk) return status;
        }
        std::uint64_t ring_offset = 0;
        if (const Status reserved = ReserveUpload(chunk, &ring_offset); reserved != Status::kOk) return reserved;
        std::memcpy(upload_mapped_ + ring_offset, src + done, chunk);

        if (const Status begun = BeginBatch(); begun != Status::kOk) return begun;
        std::size_t from = 0;
        for (const GuestRun& run : runs_scratch_) {
            ID3D12Resource* target = slabs_.at(run.slab).Get();
            TransitionSlab(run.slab, D3D12_RESOURCE_STATE_COPY_DEST);
            active_list_->CopyBufferRegion(target, run.offset, upload_.Get(), ring_offset + from, run.bytes);
            from += run.bytes;
        }
        ++batch_ops_;
        if (batch_ops_ >= kMaxBatchOps) {
            if (const Status flushed = Flush(); flushed != Status::kOk) return flushed;
        }
        done += chunk;
    }
    ++uploads_;
    upload_bytes_ += bytes;
    return Status::kOk;
}

Status GuestMemory::FillGuest(const GuestPageTable& table, std::uint64_t va, std::uint32_t value, std::size_t bytes) {
    if (!device_ || bytes == 0 || bytes % 4 != 0) return Status::kInvalidArgument;
    if (const Status ring = EnsureUploadRing(); ring != Status::kOk) return ring;

    std::size_t done = 0;
    while (done < bytes) {
        const std::size_t chunk = std::min<std::size_t>(bytes - done, static_cast<std::size_t>(kUploadSliceBytes));
        if (!SlabRuns(table, va + done, chunk, &runs_scratch_)) return Status::kInvalidArgument;
        for (const GuestRun& run : runs_scratch_) {
            if (const Status status = EnsureSlab(run.slab); status != Status::kOk) return status;
        }
        std::uint64_t ring_offset = 0;
        if (const Status reserved = ReserveUpload(chunk, &ring_offset); reserved != Status::kOk) return reserved;
        // Write the 32-bit pattern directly into the mapped upload ring (zero heap alloc).
        auto* dst_words = reinterpret_cast<std::uint32_t*>(upload_mapped_ + ring_offset);
        std::fill_n(dst_words, chunk / 4u, value);

        if (const Status begun = BeginBatch(); begun != Status::kOk) return begun;
        std::size_t from = 0;
        for (const GuestRun& run : runs_scratch_) {
            ID3D12Resource* target = slabs_.at(run.slab).Get();
            TransitionSlab(run.slab, D3D12_RESOURCE_STATE_COPY_DEST);
            active_list_->CopyBufferRegion(target, run.offset, upload_.Get(), ring_offset + from, run.bytes);
            from += run.bytes;
        }
        ++batch_ops_;
        if (batch_ops_ >= kMaxBatchOps) {
            if (const Status flushed = Flush(); flushed != Status::kOk) return flushed;
        }
        done += chunk;
    }
    ++uploads_;
    upload_bytes_ += bytes;
    return Status::kOk;
}

Status GuestMemory::ReadGuest(const GuestPageTable& table, std::uint64_t va, void* out, std::size_t bytes) {
    if (!device_ || !out || bytes == 0 || bytes > kUploadSliceBytes) return Status::kInvalidArgument;
    if (!SlabRuns(table, va, bytes, &runs_scratch_)) return Status::kInvalidArgument;
    // Flush any queued async uploads/copies before reading back so readback observes them.
    if (batch_open_) {
        if (const Status flushed = Flush(); flushed != Status::kOk) return flushed;
    }
    const std::uint64_t needed = std::max<std::uint64_t>(kStagingBytes, bytes);
    if (!readback_ || readback_bytes_ < needed) {
        readback_.Reset();
        readback_bytes_ = 0;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        const D3D12_RESOURCE_DESC desc = RawBuffer(needed);
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(readback_.GetAddressOf())))) {
            return Status::kOutOfMemory;
        }
        readback_bytes_ = needed;
    }
    // A slab the guest never made resident holds the guard page's contents, which
    // is no data at all: those runs read as zero rather than as a walk's leftovers.
    std::memset(out, 0, bytes);

    bool any_resident = false;
    for (const GuestRun& run : runs_scratch_) {
        if (slabs_.find(run.slab) != slabs_.end()) {
            any_resident = true;
            break;
        }
    }
    if (!any_resident) return Status::kOk;

    if (const Status begun = BeginBatch(); begun != Status::kOk) return begun;
    std::size_t at = 0;
    for (const GuestRun& run : runs_scratch_) {
        const auto it = slabs_.find(run.slab);
        if (it != slabs_.end()) {
            TransitionSlab(run.slab, D3D12_RESOURCE_STATE_COPY_SOURCE);
            active_list_->CopyBufferRegion(readback_.Get(), at, it->second.Get(), run.offset, run.bytes);
        }
        at += run.bytes;
    }
    if (const Status flushed = Flush(); flushed != Status::kOk) return flushed;
    const std::uint64_t fence = owner_->SubmittedValue();
    if (!owner_->WaitFor(fence)) return owner_->DeviceLost() ? Status::kDeviceLost : Status::kUnavailable;

    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, bytes};
    if (FAILED(readback_->Map(0, &read_range, &mapped))) return Status::kOutOfMemory;
    const auto* src = static_cast<const std::uint8_t*>(mapped);
    auto* dst = static_cast<std::uint8_t*>(out);
    std::size_t out_at = 0;
    for (const GuestRun& run : runs_scratch_) {
        if (slabs_.find(run.slab) != slabs_.end()) {
            std::memcpy(dst + out_at, src + out_at, run.bytes);
        }
        out_at += run.bytes;
    }
    const D3D12_RANGE no_write{0, 0};
    readback_->Unmap(0, &no_write);
    return Status::kOk;
}

Status GuestMemory::CopyGuest(const GuestPageTable& table, std::uint64_t dst_va, std::uint64_t src_va, std::size_t bytes) {
    if (!device_ || bytes == 0 || bytes % 4 != 0) return Status::kInvalidArgument;
    if (dst_va == src_va) return Status::kOk;
    if (!SlabRuns(table, src_va, bytes, &runs_scratch_)) return Status::kInvalidArgument;
    if (!SlabRuns(table, dst_va, bytes, &dst_runs_scratch_)) return Status::kInvalidArgument;
    for (const GuestRun& run : dst_runs_scratch_) {
        if (const Status status = EnsureSlab(run.slab); status != Status::kOk) return status;
    }

    // Pure GPU-to-GPU copy across runs: no CPU readback and no WaitFor() stall.
    std::size_t si = 0, di = 0;
    std::size_t s_off = 0, d_off = 0;
    std::uint64_t dst_cursor = dst_va;
    while (si < runs_scratch_.size() && di < dst_runs_scratch_.size()) {
        const GuestRun& sr = runs_scratch_[si];
        const GuestRun& dr = dst_runs_scratch_[di];
        const std::size_t step = std::min(sr.bytes - s_off, dr.bytes - d_off);
        const auto src_it = slabs_.find(sr.slab);
        if (src_it == slabs_.end()) {
            // Unallocated source slab is logically zero; zero-fill destination via ring.
            if (const Status ring = EnsureUploadRing(); ring != Status::kOk) return ring;
            std::size_t z_done = 0;
            while (z_done < step) {
                const std::size_t chunk = std::min<std::size_t>(step - z_done, static_cast<std::size_t>(kUploadSliceBytes));
                std::uint64_t ring_offset = 0;
                if (const Status res = ReserveUpload(chunk, &ring_offset); res != Status::kOk) return res;
                std::memset(upload_mapped_ + ring_offset, 0, chunk);
                if (const Status begun = BeginBatch(); begun != Status::kOk) return begun;
                TransitionSlab(dr.slab, D3D12_RESOURCE_STATE_COPY_DEST);
                active_list_->CopyBufferRegion(slabs_.at(dr.slab).Get(), dr.offset + d_off + z_done,
                                               upload_.Get(), ring_offset, chunk);
                ++batch_ops_;
                if (batch_ops_ >= kMaxBatchOps) {
                    if (const Status flushed = Flush(); flushed != Status::kOk) return flushed;
                }
                z_done += chunk;
            }
        } else if (sr.slab != dr.slab) {
            if (const Status begun = BeginBatch(); begun != Status::kOk) return begun;
            TransitionSlab(sr.slab, D3D12_RESOURCE_STATE_COPY_SOURCE);
            TransitionSlab(dr.slab, D3D12_RESOURCE_STATE_COPY_DEST);
            active_list_->CopyBufferRegion(slabs_.at(dr.slab).Get(), dr.offset + d_off,
                                           src_it->second.Get(), sr.offset + s_off, step);
            ++batch_ops_;
            if (batch_ops_ >= kMaxBatchOps) {
                if (const Status flushed = Flush(); flushed != Status::kOk) return flushed;
            }
        } else {
            // Same-slab copy: bounce through DEFAULT-heap copy_scratch_ so the
            // slab is never simultaneously COPY_SOURCE and COPY_DEST.
            if (const Status sc = EnsureCopyScratch(); sc != Status::kOk) return sc;
            ID3D12Resource* slab_res = src_it->second.Get();
            std::size_t c_done = 0;
            while (c_done < step) {
                const std::size_t sub = std::min<std::size_t>(step - c_done, static_cast<std::size_t>(kCopyScratchBytes));
                if (const Status begun = BeginBatch(); begun != Status::kOk) return begun;
                TransitionSlab(sr.slab, D3D12_RESOURCE_STATE_COPY_SOURCE);
                active_list_->CopyBufferRegion(copy_scratch_.Get(), 0, slab_res, sr.offset + s_off + c_done, sub);
                const D3D12_RESOURCE_BARRIER mid[2] = {
                    Transition(copy_scratch_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
                    Transition(slab_res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                };
                active_list_->ResourceBarrier(2, mid);
                slab_batch_state_[sr.slab] = D3D12_RESOURCE_STATE_COPY_DEST;
                slab_batch_dirty_mask_ |= (1u << sr.slab);
                active_list_->CopyBufferRegion(slab_res, dr.offset + d_off + c_done, copy_scratch_.Get(), 0, sub);
                const D3D12_RESOURCE_BARRIER restore =
                    Transition(copy_scratch_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                active_list_->ResourceBarrier(1, &restore);
                ++batch_ops_;
                if (batch_ops_ >= kMaxBatchOps) {
                    if (const Status flushed = Flush(); flushed != Status::kOk) return flushed;
                }
                c_done += sub;
            }
        }
        s_off += step;
        d_off += step;
        dst_cursor += step;
        (void)dst_cursor;
        if (s_off >= sr.bytes) { ++si; s_off = 0; }
        if (d_off >= dr.bytes) { ++di; d_off = 0; }
    }
    ++uploads_;
    upload_bytes_ += bytes;
    return Status::kOk;
}

}  // namespace bbx::d3d12

#endif  // _WIN32
