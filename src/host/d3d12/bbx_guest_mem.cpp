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
        if (offset > kGuestSlabBytes) continue;
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

D3D12_RESOURCE_DESC RawBuffer(std::uint64_t bytes) {
    // A ByteAddressBuffer's view of a buffer: R32_TYPELESS, one element per dword.
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
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
    slabs_.clear();
    owner_ = nullptr;
    budget_ = nullptr;
    page_table_.Reset();
    guard_.Reset();
    upload_.Reset();
    readback_.Reset();
    heap_.Reset();
    device_ = nullptr;
    slabs_resident_ = 0;
    committed_bytes_ = 0;
    uploads_ = 0;
    upload_bytes_ = 0;
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

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    // The page table: one committed buffer, written once per boot, read by every
    // dispatch. The plan's tables are ~1 MiB and already in the shader's shape.
    const std::vector<std::uint8_t> bytes = plan.table.Bytes();
    if (bytes.size() > kStagingBytes) return Status::kInvalidArgument;
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
    const D3D12_RESOURCE_DESC guard_desc = RawBuffer(kGuardBytes);
    if (FAILED(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &guard_desc, D3D12_RESOURCE_STATE_COMMON,
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
    owner_->GraphicsQueue()->ExecuteCommandLists(1, lists);
    const std::uint64_t value = owner_->Signal();
    if (!owner_->WaitFor(value)) return owner_->DeviceLost() ? Status::kDeviceLost : Status::kUnavailable;

    committed_bytes_ = bytes.size() + kGuardBytes;
    slabs_resident_ = std::min(plan.slabs_resident, kGuestSlabCount);
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
    const D3D12_RESOURCE_DESC desc = RawBuffer(kGuestSlabBytes);
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
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
        << " slab(s) resident, " << uploads_ << " upload(s), " << (upload_bytes_ >> 10) << " KiB copied";
    return out.str();
}

// The one submission path both directions use: record, execute, wait. There is no
// ring because an upload is a boot-time or a one-off event, not a per-frame flow,
// and a wait that a frame would not tolerate is exactly what makes the write
// complete for the CPU when the call returns.
Status GuestMemory::Submit(ID3D12GraphicsCommandList* list, const std::function<void(ID3D12GraphicsCommandList*)>& record) {
    record(list);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    owner_->GraphicsQueue()->ExecuteCommandLists(1, lists);
    const std::uint64_t value = owner_->Signal();
    if (!owner_->WaitFor(value)) return owner_->DeviceLost() ? Status::kDeviceLost : Status::kUnavailable;
    return Status::kOk;
}

Status GuestMemory::Record(ID3D12GraphicsCommandList** list_out, ComPtr<ID3D12CommandAllocator>* allocator_out) {
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator_out->GetAddressOf())))) {
        return Status::kOutOfMemory;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_out->Get(), nullptr,
                                          IID_PPV_ARGS(list.GetAddressOf())))) {
        return Status::kOutOfMemory;
    }
    *list_out = list.Detach();
    return Status::kOk;
}

Status GuestMemory::WriteGuest(const GuestPageTable& table, std::uint64_t va, const void* data, std::size_t bytes) {
    std::vector<GuestRun> runs;
    if (!device_ || !data || bytes == 0 || bytes % 4 != 0 || bytes > kStagingBytes) return Status::kInvalidArgument;
    if (!SlabRuns(table, va, bytes, &runs)) return Status::kInvalidArgument;  // unmapped: never the guard page
    for (const GuestRun& run : runs) {
        if (const Status status = EnsureSlab(run.slab); status != Status::kOk) return status;
    }
    if (!upload_) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        const D3D12_RESOURCE_DESC desc = RawBuffer(kStagingBytes);
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(upload_.GetAddressOf())))) {
            return Status::kOutOfMemory;
        }
    }
    void* mapped = nullptr;
    if (FAILED(upload_->Map(0, nullptr, &mapped))) return Status::kOutOfMemory;
    std::memcpy(mapped, data, bytes);
    upload_->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> allocator;
    ID3D12GraphicsCommandList* raw = nullptr;
    if (const Status status = Record(&raw, &allocator); status != Status::kOk) return status;
    ComPtr<ID3D12GraphicsCommandList> list;
    list.Attach(raw);
    std::size_t from = 0;
    const Status status = Submit(list.Get(), [&](ID3D12GraphicsCommandList* l) {
        for (const GuestRun& run : runs) {
            ID3D12Resource* target = slabs_.at(run.slab).Get();
            const D3D12_RESOURCE_BARRIER before[] = {Transition(target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
            l->ResourceBarrier(1, &before[0]);
            l->CopyBufferRegion(target, run.offset, upload_.Get(), from, run.bytes);
            const D3D12_RESOURCE_BARRIER after[] = {Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)};
            l->ResourceBarrier(1, &after[0]);
            from += run.bytes;
        }
    });
    if (status != Status::kOk) return status;
    ++uploads_;
    upload_bytes_ += bytes;
    return Status::kOk;
}

Status GuestMemory::FillGuest(const GuestPageTable& table, std::uint64_t va, std::uint32_t value, std::size_t bytes) {
    // A fill is a write of one dword repeated, through the same upload path so
    // there is one rule for what reaches the slabs.
    std::vector<std::uint8_t> pattern(static_cast<std::size_t>(bytes), 0);
    for (std::size_t at = 0; at + 4 <= bytes; at += 4) std::memcpy(pattern.data() + at, &value, 4);
    return WriteGuest(table, va, pattern.data(), bytes);
}

Status GuestMemory::ReadGuest(const GuestPageTable& table, std::uint64_t va, void* out, std::size_t bytes) {
    std::vector<GuestRun> runs;
    if (!device_ || !out || bytes == 0 || bytes > kStagingBytes) return Status::kInvalidArgument;
    if (!SlabRuns(table, va, bytes, &runs)) return Status::kInvalidArgument;
    if (!readback_) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        const D3D12_RESOURCE_DESC desc = RawBuffer(kStagingBytes);
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(readback_.GetAddressOf())))) {
            return Status::kOutOfMemory;
        }
    }
    // A slab the guest never made resident holds the guard page's contents, which
    // is no data at all: those runs read as zero rather than as a walk's leftovers.
    std::memset(out, 0, bytes);
    ComPtr<ID3D12CommandAllocator> allocator;
    ID3D12GraphicsCommandList* raw = nullptr;
    if (const Status status = Record(&raw, &allocator); status != Status::kOk) return status;
    ComPtr<ID3D12GraphicsCommandList> list;
    list.Attach(raw);
    std::size_t at = 0;
    bool copied = false;
    const Status status = Submit(list.Get(), [&](ID3D12GraphicsCommandList* l) {
        for (const GuestRun& run : runs) {
            const auto it = slabs_.find(run.slab);
            if (it != slabs_.end()) {
                ID3D12Resource* source = it->second.Get();
                const D3D12_RESOURCE_BARRIER before[] = {
                    Transition(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)};
                l->ResourceBarrier(1, &before[0]);
                l->CopyBufferRegion(readback_.Get(), at, source, run.offset, run.bytes);
                const D3D12_RESOURCE_BARRIER after[] = {
                    Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON)};
                l->ResourceBarrier(1, &after[0]);
                copied = true;
            }
            at += run.bytes;
        }
    });
    if (status != Status::kOk || !copied) return status;
    void* mapped = nullptr;
    if (FAILED(readback_->Map(0, nullptr, &mapped))) return Status::kOutOfMemory;
    std::memcpy(out, mapped, bytes);
    readback_->Unmap(0, nullptr);
    return Status::kOk;
}

Status GuestMemory::CopyGuest(const GuestPageTable& table, std::uint64_t dst_va, std::uint64_t src_va, std::size_t bytes) {
    // Read back and write again: the source's bytes are the ones the GPU wrote,
    // and a shadow copy here would be a second answer to what guest memory holds.
    std::vector<std::uint8_t> staging(static_cast<std::size_t>(bytes));
    if (const Status status = ReadGuest(table, src_va, staging.data(), bytes); status != Status::kOk) return status;
    return WriteGuest(table, dst_va, staging.data(), bytes);
}

}  // namespace bbx::d3d12

#endif  // _WIN32
