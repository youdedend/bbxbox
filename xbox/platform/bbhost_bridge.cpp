// SPDX-License-Identifier: GPL-3.0-or-later
// The console's side of the guest/host seam.
//
// bbhost is a desktop program: `main.cpp` opens an SDL window, a Vulkan
// presenter and a pad, then hands the game a host API it calls into. This port
// has none of that. The window belongs to the launcher (a CoreWindow), the
// presenter is `d3d12_session`, and the pad arrives through SDL3-uwp. The
// archives are the same code, so they still call the API declared in their own
// headers - `host/window.h`, `host/options.h`, `host/gpu.h` and the rest - and
// something has to answer.
//
// That something is this file. It is the only translation unit in the app that
// includes the host headers, so it is also the only one that can be wrong about
// them without the launcher noticing: the defaults below are chosen so that a
// guest that asks about a display, a recording, a plugin page or a translucency
// report gets the answer of a machine that has none, and keeps running.
//
// The exceptions are the three that a running game cannot do without, and they
// are implemented for real further down:
//   * host_pad_state / host_pad_present - the player's controller;
//   * host_present - the flip: the guest's display buffer, described by the
//     attribute its registration stated (format, tiling, pitch), handed to
//     `d3d12_session`, which untiles and uploads it before the present pass.
//     The description is read here, on the guest's thread, and it is arithmetic:
//     nothing in it waits for the GPU.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <SDL3/SDL.h>

#include "engine/graphics_patch.h"
#include "engine/frame_rate.h"
#include "host/translation_cache.h"
#include "host/options.h"
#include "host/window.h"
#include "host/gpu.h"
#include "host/ingame_menu.h"
#include "engine/menu_pointer.h"
#include "engine/option_menu.h"
#include "engine/patch_manifest.h"
#include "host/plugin_panel.h"
#include "host/plugins.h"
#include "engine/render_scale.h"
#include "host/sampler.h"

#include "xbox/platform/log.h"
#include "xbox/platform/host_fs.h"
#include "xbox/platform/hostfs_extra.h"
#include "xbox/platform/storage.h"
#include "src/host/d3d12/bbx_scanout.h"
#include "src/host/d3d12/bbx_shader_cache.h"
#include "xbox/platform/guest_frames.h"
#include "xbox/platform/bbhost_bridge.h"

// The guest runtime. `main.cpp` is not compiled on this target - it opens a
// desktop window and a Vulkan presenter - so the boot it performs is here,
// step for step and in the same order (upstream/bbhost/src/main.cpp:1366-1442).
#include "core/elf.h"
#include "core/imports.h"
#include "core/legacy_cpu.h"
#include "core/portable.h"
#include "engine/addr.h"
#include "engine/debug_menu.h"
#include "guest_abi.h"
#include "hle/fs.h"
#include "hle/hle.h"
#include "engine/option_rows.h"
#include "hle/modules.h"
#include "host/frame_stats.h"
#include "host/gxvk_lists.h"
#include "host/perf_meter.h"
#include "host/settings.h"


namespace {

// The player's controller, handed in by the launcher's event loop. It is the
// one the launcher opens for its own menus, so the pad the game reads and the
// pad the launcher draws with can never be two different controllers.
SDL_Gamepad* g_pad = nullptr;
std::uint64_t g_flips = 0;

}  // namespace

namespace bbx::bridge {

void SetPad(SDL_Gamepad* pad) { g_pad = pad; }
std::uint64_t Flips() { return g_flips; }

}  // namespace bbx::bridge

// --- the controller --------------------------------------------------------
// ScePadButtonDataOffset bits, as the HLE reports them - the same word
// host/window.cpp builds from the desktop build's pad. The PS4's Cross is the
// button at the bottom of the diamond, which is where SDL puts South on both an
// Xbox and a Nintendo-layout pad, so South goes to Cross either way.
namespace {
constexpr std::uint32_t kShare = 0x1, kL3 = 0x2, kR3 = 0x4, kOptions = 0x8, kUp = 0x10,
                        kRight = 0x20, kDown = 0x40, kLeft = 0x80, kL2 = 0x100, kR2 = 0x200,
                        kL1 = 0x400, kR1 = 0x800, kTriangle = 0x1000, kCircle = 0x2000,
                        kCross = 0x4000, kSquare = 0x8000, kTouchPad = 0x100000;
}  // namespace

PadState host_pad_state() {
    PadState s;
    SDL_Gamepad* pad = g_pad;
    if (pad == nullptr) return s;
    s.connected = true;
    const auto held = [pad](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); };
    const auto bit = [&s](bool down, std::uint32_t value) {
        if (down) s.buttons |= value;
    };
    bit(held(SDL_GAMEPAD_BUTTON_SOUTH), kCross);
    bit(held(SDL_GAMEPAD_BUTTON_EAST), kCircle);
    bit(held(SDL_GAMEPAD_BUTTON_WEST), kSquare);
    bit(held(SDL_GAMEPAD_BUTTON_NORTH), kTriangle);
    bit(held(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), kL1);
    bit(held(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), kR1);
    bit(held(SDL_GAMEPAD_BUTTON_BACK), kShare);
    bit(held(SDL_GAMEPAD_BUTTON_START), kOptions);
    bit(held(SDL_GAMEPAD_BUTTON_LEFT_STICK), kL3);
    bit(held(SDL_GAMEPAD_BUTTON_RIGHT_STICK), kR3);
    bit(held(SDL_GAMEPAD_BUTTON_DPAD_UP), kUp);
    bit(held(SDL_GAMEPAD_BUTTON_DPAD_DOWN), kDown);
    bit(held(SDL_GAMEPAD_BUTTON_DPAD_LEFT), kLeft);
    bit(held(SDL_GAMEPAD_BUTTON_DPAD_RIGHT), kRight);

    // Sticks: SDL reports -32768..32767 with y down; the pad reports 0..255 with
    // 127 at rest, y down as well. The conversion is a shift, so the rest value
    // lands on 127 and not on 128 (host/window.h explains why that matters).
    const auto axis = [pad](SDL_GamepadAxis a) {
        const Sint16 raw = SDL_GetGamepadAxis(pad, a);
        const int scaled = (static_cast<int>(raw) + 32768) * 255 / 65535;
        return static_cast<std::uint8_t>(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
    };
    s.lx = axis(SDL_GAMEPAD_AXIS_LEFTX);
    s.ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    s.rx = axis(SDL_GAMEPAD_AXIS_RIGHTX);
    s.ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    // Triggers: SDL gives 0..32767, the pad 0..255 (host/window.cpp divides by
    // 128 and treats 8000 as held).
    const auto trigger = [pad](SDL_GamepadAxis a) {
        const Sint16 raw = SDL_GetGamepadAxis(pad, a);
        const int v = static_cast<int>(raw) / 128;
        return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    s.l2 = trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    s.r2 = trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    if (s.l2 > 62) s.buttons |= kL2;  // 8000 / 128, as the desktop build reads it
    if (s.r2 > 62) s.buttons |= kR2;

    // The touchpad: the controller's own surface, in its own pixels (kPadTouchW
    // by kPadTouchH). SDL gives a finger in 0..1.
    for (int finger = 0; finger < 2; ++finger) {
        bool down = false;
        float x = 0.0f, y = 0.0f, pressure = 0.0f;
        if (!SDL_GetGamepadTouchpadFinger(pad, 0, finger, &down, &x, &y, &pressure)) continue;
        if (!down) continue;
        if (s.touch_count == 0) s.buttons |= kTouchPad;
        PadState::Touch& t = s.touch[s.touch_count++];
        t.x = static_cast<std::uint16_t>(x * static_cast<float>(kPadTouchW));
        t.y = static_cast<std::uint16_t>(y * static_cast<float>(kPadTouchH));
        t.id = static_cast<std::uint8_t>(finger + 1);
        t.down = true;
    }
    s.timestamp = SDL_GetTicksNS() / 1000;  // the pad reports microseconds
    return s;
}

bool host_pad_present() { return g_pad != nullptr; }

// The guest arena, published for the flip (StartGuest sets it once the image is
// loaded; the guest is the only writer of what it points at).
std::atomic<std::uint8_t*> g_guest_base{nullptr};
std::atomic<std::uint64_t> g_guest_slide{0};
std::atomic<std::uint64_t> g_guest_size{0};

// The guest's scanout range, as this process can read it: false when the address
// (or the frame it addresses) is not inside the mapped arena.
bool GuestScanoutPixels(std::uint64_t va, std::uint64_t bytes, const std::uint8_t** pixels) {
    if (pixels != nullptr) *pixels = nullptr;
    const std::uint8_t* const base = g_guest_base.load(std::memory_order_acquire);
    const std::uint64_t slide = g_guest_slide.load(std::memory_order_relaxed);
    const std::uint64_t size = g_guest_size.load(std::memory_order_relaxed);
    if (base == nullptr || va < slide || va - slide >= size || bytes > size - (va - slide)) { return false; }
    if (pixels != nullptr) *pixels = base + (va - slide);
    return true;
}

// --- the flip --------------------------------------------------------------
// Called by the guest's video HLE once per frame, on the guest's thread. What it
// does is bounded: a bounds check, a memcpy into the session's pending frame and
// a counter - nothing that waits for the GPU or for the presenter, because the
// guest is between two EOP/label waits here and stalling it stalls the game.
void host_present(int buffer_index, std::uint64_t display_va, unsigned display_w, unsigned display_h,
                  const PresentFlip& flip) {
    (void)buffer_index; (void)flip;
    ++g_flips;
    if ((g_flips % 60) == 1) {
        bbx::platform::LogCrumb("gpu:flip");
    }
    if (display_va == 0 || display_w == 0 || display_h == 0) { return; }
    // The attribute the buffer was registered with. A flip of an address no port
    // registered is not a display buffer, and presenting it "as RGBA8" would be
    // three guesses about bytes the console described exactly - so it is dropped
    // and said once, not per frame.
    std::uint32_t format = 0, tiling = 0, pitch_pixels = 0;
    if (!hle_video_display_attribute(display_va, &format, &tiling, &pitch_pixels)) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            bbx::platform::LogMessage(bbx::platform::Level::kWarn,
                                      "video: a flip of %p, which no sceVideoOutRegisterBuffers registered; "
                                      "the frame is dropped (the attribute is what describes the pixels)",
                                      reinterpret_cast<void*>(static_cast<std::uintptr_t>(display_va)));
        }
        return;
    }
    bbx::d3d12::ScanoutFormat scanout_format = bbx::d3d12::ScanoutFormat::kA8R8G8B8Srgb;
    bbx::d3d12::ScanoutTiling scanout_tiling = bbx::d3d12::ScanoutTiling::kTile;
    if (!bbx::d3d12::ScanoutFormatFromWord(format, scanout_format) ||
        !bbx::d3d12::ScanoutTilingFromWord(tiling, scanout_tiling)) {
        static std::atomic<std::uint32_t> said_bits{0xffffffffu};
        if (said_bits.exchange(format << 1 | (tiling & 1u)) != (format << 1 | (tiling & 1u))) {
            bbx::platform::LogMessage(bbx::platform::Level::kWarn,
                                      "video: a display buffer with pixel format 0x%08x and tiling mode %u, which "
                                      "this port has no name for; the frame is dropped",
                                      format, tiling);
        }
        return;
    }
    bbx::d3d12::ScanoutDesc desc;
    desc.format = scanout_format;
    desc.tiling = scanout_tiling;
    desc.width = display_w;
    desc.height = display_h;
    desc.pitch_pixels = pitch_pixels;
    // What the copy will read, in the guest's own layout: a tiled buffer is the
    // same number of bytes, in micro tiles rather than rows.
    const bbx::d3d12::ScanoutState state = bbx::d3d12::DescribeScanout(desc);
    if (!state.ok) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            bbx::platform::LogMessage(bbx::platform::Level::kWarn, "video: no frame presented: %s", state.reason.c_str());
        }
        return;
    }
    // The scanout buffer, as this process can read it: the guest arena is mapped
    // in-process, so a guest virtual address is a pointer here - once the range is
    // known to be inside the image. sceVideoOutRegisterBuffers can be handed an
    // address the image no longer covers (a mode change, a title that reuses a
    // buffer after freeing it), and a flip must not fault the guest.
    const std::uint8_t* pixels = nullptr;
    if (!GuestScanoutPixels(display_va, state.bytes, &pixels)) { return; }
    bbx::platform::GuestFrame frame;
    frame.pixels = pixels;
    frame.width = display_w;
    frame.height = display_h;
    frame.format = format;
    frame.tiling = tiling;
    frame.pitch = pitch_pixels;
    frame.bytes = state.bytes;
    bbx::platform::SubmitGuestFrame(frame);
}

// The launcher owns the window: there is nothing here to pump, nothing to
// close, and it is always in front while it is running.
bool host_window_pump() { return true; }
void host_window_stop() {}
bool host_window_active() { return true; }

// --- the seam's quiet half ------------------------------------------------
// Everything below answers "this machine has none of that": no options page, no
// plugin panel, no capture, no overlay, no translation cache, no second window.
// Each returns the value its own declaration calls neutral, so a guest that asks
// about one is answered instead of stopped at load. The declarations are copied
// from bbhost's headers and the bodies are the quiet answer; when a slice of the
// port needs one of them for real, it is implemented here and the entry is gone.

// engine/frame_rate.h
void frame_rate_atk_params_loaded() {  }
bool frame_rate_converted() { return {}; }
int frame_rate_game_fps() { return {}; }
void frame_rate_install(ElfImage* image) {  }
void frame_rate_install_late(ElfImage* image) {  }
float frame_rate_limit_fps() { return {}; }
std::string frame_rate_limiter_window() { return {}; }
bool frame_rate_live(int fps) { return {}; }
bool frame_rate_request(int fps) { return {}; }
void frame_rate_retime() {  }
void frame_rate_set_time_scale(float scale) {  }

// engine/graphics_patch.h
bool engine_prologue_hook_variadic(ElfImage* image, std::uint64_t at, const std::uint8_t* prologue, std::size_t n, void* host, std::uint64_t id) { return {}; }
std::uint64_t engine_scene_view_flip() { return {}; }
bool engine_site_hook(ElfImage* image, std::uint64_t at, const std::uint8_t* bytes, std::size_t n, void* host, std::uint64_t id) { return {}; }
std::string graphics_glare_report() { return {}; }
std::uint32_t graphics_look_available() { return {}; }
void graphics_look_set(const GraphicsLook& look) {  }
void graphics_patch_install(ElfImage* image) {  }
void graphics_scene_context_made(std::uint64_t ctx) {  }
void graphics_ssr_live_in() {  }

// engine/menu_pointer.h
void menu_pointer_click() {  }
void menu_pointer_install(ElfImage* image) {  }
std::uint32_t menu_pointer_take_press() { return {}; }
std::uint64_t menu_pointer_tick() { return {}; }
void menu_pointer_wheel() {  }
std::uint32_t menu_region_confirm_button() { return {}; }
void menu_sound_post(int id, int priority) {  }

// engine/option_menu.h
bool option_menu_click_decides(void* list, int index) { return {}; }
void option_menu_confirm_ask(std::uint32_t message) {  }
void option_menu_confirm_close() {  }
void option_menu_install(ElfImage* image) {  }
void option_menu_list_update(void* comp, bool focused) {  }
void option_menu_poll() {  }

// engine/patch_manifest.h
PatchLiveState patch_manifest_live_state(const std::string& name) { return {}; }
bool patch_manifest_off(const std::string& name) { return {}; }
bool patch_manifest_on(const std::string& name, bool enabled) { return {}; }
bool patch_manifest_option_known(const std::string& option) { return {}; }
const char* patch_manifest_owner(std::uint64_t address, std::size_t n) { return {}; }
bool patch_manifest_set_live(const std::string& name, bool on, std::string* why) { return {}; }
void patch_manifests_apply(ElfImage* image) {  }
bool patch_manifests_done() { return {}; }
std::vector<std::string> patch_manifests_gameplay() { return {}; }
std::vector<PatchListing> patch_manifests_list(const std::string& mods) { return {}; }
bool shadow_resolution_known(bool* applied) { return {}; }
std::uint64_t shadow_resolution_more_mib(int tier) { return {}; }
ShadowMapSizes shadow_resolution_sizes(int tier) { return {}; }

// engine/render_scale.h
bool render_scale_applied() { return {}; }
int render_scale_blur_shift(int shift) { return {}; }
void render_scale_flip(std::uint64_t flip) {  }
void render_scale_gx_init(std::uint32_t display_w, std::uint32_t display_h, std::uint64_t flip) {  }
void render_scale_install(ElfImage* image) {  }
bool render_scale_on(RenderScale* out) { return {}; }
bool render_scale_plan(RenderScale* out) { return {}; }
bool render_scale_retarget(std::uint32_t output_w, std::uint32_t output_h, std::uint32_t display_w, std::uint32_t display_h, std::uint64_t flip, RenderScale* before, RenderScale* after) { return {}; }
std::vector<patch_file::Site> render_scale_sites(const RenderScale& rs) { return {}; }
void render_scale_size(int quality, std::uint32_t output_w, std::uint32_t output_h, std::uint32_t* render_w, std::uint32_t* render_h) {  }

// host/gpu.h
//
// Only what the host layer does not itself define: `src/host/d3d12/host_gpu_d3d12.cpp`
// answers the rest of this header on the console (the timeline fence, the guest's
// slabs, the dispatch), and a second definition of anything it answers is a
// duplicate symbol at link, not a fallback. Four were removed here when the
// bridge landed: host_gpu_read_guest, host_gpu_mem_write_serial/_ordered and
// host_gpu_dispatch_user_reads.
void host_gpu_compare_cs_user_data(const GxStageRecords& st, const std::uint32_t* reg, std::uint32_t read_mask, CsUserCompare& out) {  }
std::uint64_t host_gpu_completed_submits() { return {}; }
std::uint64_t host_gpu_completed_submits_seen() { return {}; }
std::size_t host_gpu_copy_backs_captured(const std::uint64_t* dst, const std::uint64_t* src, const std::uint64_t* bytes, const std::uint32_t* at, std::uint32_t piece, std::size_t n) { return {}; }
bool host_gpu_copy_image_region(const GpuImageCopy& c) { return {}; }
void host_gpu_depth_clear(std::uint64_t base, std::uint32_t word, std::uint32_t stencil) {  }
bool host_gpu_device_lost() { return {}; }
void host_gpu_explain_value(std::uint64_t va, std::uint64_t value) {  }
bool host_gpu_fill(std::uint64_t va, std::size_t bytes, const float rgba[4], std::uint32_t depth_clear, std::uint32_t stencil_clear) { return {}; }
std::string host_gpu_fill_stats() { return {}; }
void host_gpu_hang_report() {  }
void host_gpu_lists_arrival(std::uint64_t id) {  }
std::string host_gpu_lists_times_report(int part, std::uint64_t parked_ticks, std::uint64_t parks) { return {}; }
void host_gpu_pipeline_time_us(std::uint64_t out[5]) {  }
void host_gpu_pipeline_work(std::uint64_t out[14]) {  }
void host_gpu_recorder_paced(std::uint64_t wait_ns, float cap_fps, bool loading) {  }
void host_gpu_refill_selftest() {  }
std::uint64_t host_gpu_ring_place(const void* data, std::size_t bytes, std::size_t align) { return {}; }
bool host_gpu_segment_flags_needed() { return {}; }
void host_gpu_segment_follows(bool follows) {  }
const std::uint64_t* host_gpu_segment_ids(std::uint64_t id, std::uint32_t* count, std::uint64_t* word) { return {}; }
void host_gpu_segment_refused(std::uint64_t id, std::uint64_t word) {  }
std::string host_gpu_segment_report(bool spliced_part) { return {}; }
void host_gpu_segment_skipped(std::uint64_t ns, bool lost) {  }
void host_gpu_splice_compare(std::uint64_t id) {  }
void host_gpu_splice_compare_begin(std::uint64_t id) {  }
std::string host_gpu_splice_report(bool window) { return {}; }
bool host_gpu_splice_segment(std::uint64_t id, std::uint64_t word) { return {}; }
void host_gpu_splice_unfed(std::uint32_t n) {  }
void host_gpu_stall_test(unsigned ms) {  }
std::uint64_t host_gpu_surface_replacements() { return {}; }
std::uint64_t host_gpu_texture_hash_us() { return {}; }
std::uint64_t host_gpu_texture_hashes() { return {}; }
void host_gpu_texture_retire(std::uint64_t base, std::size_t bytes, std::uint64_t after_submitted, std::uint64_t gen) {  }
void host_gpu_texture_time_us(std::uint64_t out[5]) {  }
std::uint64_t host_gpu_texture_uploads() { return {}; }
bool host_gpu_upload_region(std::uint64_t base, const std::uint32_t* tsharp, std::uint32_t mip, std::uint32_t layer, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, const void* data, std::size_t bytes, std::uint32_t row_bytes) { return {}; }
std::uint32_t host_gpu_vendor_id() { return {}; }
const char* host_gpu_vs_why_name(int why) { return {}; }
void host_gpu_wait_work_submitted(std::uint64_t need) {  }
void host_gpu_watch_display(std::uint64_t display_va) {  }
const char* host_present_step(double& seconds) { return {}; }

// host/ingame_menu.h
bool ingame_menu_event(const SDL_Event& e, float pixel_ratio) { return {}; }
bool ingame_menu_init(VkInstance instance, VkPhysicalDevice phys, VkDevice device, std::uint32_t family, VkQueue queue, VkFormat format, std::uint32_t image_count) { return {}; }
void ingame_menu_pump() {  }
void ingame_menu_record(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent) {  }
bool ingame_menu_record_in_pass(VkCommandBuffer cmd, VkExtent2D extent) { return {}; }
void ingame_menu_shutdown() {  }

// host/options.h
void host_account_action(int which, const std::string& name, const std::string& code) {  }
bool host_account_busy() { return {}; }
std::string host_account_detail() { return {}; }
std::string host_account_outcome() { return {}; }
std::string host_account_signed_in() { return {}; }
bool host_account_take_save() { return {}; }
int host_deck_default_profile() { return {}; }
int host_deck_profile() { return {}; }
HostEffectTimingRow host_effect_timing_row() { return {}; }
void host_frame_generation_warning(int cap_index, bool custom, int custom_fps) {  }
bool host_frame_rate_control(int* cap_index, bool* custom, int* custom_fps, float width) { return {}; }
bool host_frame_rate_fit(int* cap_index, bool* custom, int* custom_fps) { return {}; }
int host_frame_rate_warning_count(int cap_index, bool custom, int custom_fps) { return {}; }
void host_opt_choose_effect_timing(int index) {  }
int host_opt_default_frame_cap() { return {}; }
bool host_opt_default_get(const char* key) { return {}; }
int host_opt_default_index(const char* key) { return {}; }
int host_opt_default_resolution_index() { return {}; }
int host_opt_frame_cap() { return {}; }
bool host_opt_frame_cap_custom() { return {}; }
int host_opt_frame_cap_custom_fps() { return {}; }
int host_opt_frame_rate() { return {}; }
bool host_opt_get(const char* key) { return {}; }
int host_opt_index(const char* key) { return {}; }
void host_opt_menu_colour(float* hue_degrees, float* vibrance) {  }
float host_opt_menu_text_scale() { return {}; }
int host_opt_motion_rebuilt_bits() { return {}; }
bool host_opt_motion_wanted() { return {}; }
bool host_opt_output_size(int* w, int* h) { return {}; }
void host_opt_republish() {  }
bool host_opt_resolution(int* w, int* h) { return {}; }
int host_opt_resolution_index() { return {}; }
const char* const* host_opt_resolution_labels(int* n) { return {}; }
const char* host_opt_resolution_note() { return {}; }
bool host_opt_saved(const char* key) { return {}; }
std::uint64_t host_opt_serial() { return {}; }
void host_opt_set(const char* key, bool on) {  }
void host_opt_set_frame_cap(int fps) {  }
void host_opt_set_frame_cap_custom(bool on, int fps) {  }
void host_opt_set_index(const char* key, int index) {  }
void host_opt_set_resolution_index(int i) {  }
bool host_opt_upscaler_chosen() { return {}; }
void host_options_apply() {  }
void host_options_close() {  }
void host_options_draw_page(int page) {  }
void host_options_draw_setup_page(int page) {  }
void host_options_frame(float display_w, float display_h) {  }
void host_options_load() {  }
const char* host_options_page_blurb(int page) { return {}; }
int host_options_page_count() { return {}; }
const char* host_options_page_name(int page) { return {}; }
void host_options_post(std::function<void()> fn) {  }
void host_options_save_now() {  }
int host_options_selftest(const std::string& dir) { return {}; }
void host_options_set_open(bool open) {  }
void host_set_deck_profile(int profile) {  }
bool host_steam_deck() { return {}; }

// host/plugin_panel.h
std::uint32_t plugin_panel_dialog_buttons() { return {}; }
void plugin_panel_draw(float scale, bool pad) {  }
bool plugin_panel_exclusive() { return {}; }
void plugin_panel_hide(const std::string& name) {  }
bool plugin_panel_key(int scancode) { return {}; }
std::vector<PluginMenuEntry> plugin_panel_menus(const std::string& name) { return {}; }
bool plugin_panel_native_centred() { return {}; }
bool plugin_panel_native_confirm(int row) { return {}; }
void plugin_panel_native_failed(const char* why) {  }
void plugin_panel_native_frame() {  }
void plugin_panel_native_gone(bool closed, const char* by) {  }
bool plugin_panel_native_key(int scancode) { return {}; }
void plugin_panel_native_list() {  }
bool plugin_panel_native_menus_any() { return {}; }
bool plugin_panel_native_on_subpage() { return {}; }
bool plugin_panel_native_page(plugin_dialog::Page& out) { return {}; }
int plugin_panel_native_press(int row, std::string* question) { return {}; }
bool plugin_panel_open(const std::string& name, const std::string& page) { return {}; }
bool plugin_panel_overlay_shown() { return {}; }
void plugin_panel_preload() {  }
void plugin_panel_preview_tick() {  }
bool plugin_panel_rows(const std::string& name, const std::string& page, const std::vector<PluginOptionDesc>& rows) { return {}; }
void plugin_panel_settle() {  }
bool plugin_panel_show(const std::string& name, const PluginPanelShow& how) { return {}; }
bool plugin_panel_shown() { return {}; }
void plugin_panel_state(const std::string& name, PluginPanelState& out) {  }
int plugin_panel_take_sound(int* priority) { return {}; }
bool plugin_panel_value(const std::string& name, const std::string& page, const std::string& key, const std::string& value) { return {}; }
void plugin_panel_world(std::uint32_t flags, std::int64_t hp) {  }

// host/plugins.h
int plugins_aa_claim() { return {}; }
std::string plugins_aa_owner() { return {}; }
std::string plugins_aa_owner_scanned() { return {}; }
std::string plugins_adopts() { return {}; }
std::vector<PluginEntry> plugins_catalog() { return {}; }
std::string plugins_clash(const std::vector<PluginEntry>& list, const std::string& name) { return {}; }
std::vector<std::string> plugins_dirs() { return {}; }
void plugins_enter_world(const std::string& host_rules) {  }
void plugins_event_flag(unsigned id, bool value) {  }
void plugins_frame() {  }
void plugins_image(ElfImage* image) {  }
bool plugins_live_switch(const PluginEntry& p) { return {}; }
void plugins_load() {  }
void plugins_menu_option(const std::string& name, const std::string& key, const std::string& value) {  }
std::string plugins_option_value(const PluginEntry& p, const PluginOptionDesc& o) { return {}; }
std::string plugins_ruleset() { return {}; }
void plugins_run_action(const std::string& name, const std::string& action) {  }
std::vector<PluginEntry> plugins_scan() { return {}; }
void plugins_set_enabled(const std::string& name, bool on) {  }
void plugins_set_option(const std::string& name, const std::string& key, const std::string& value) {  }

// host/sampler.h
void sampler_note_exit(const char* step) {  }
void sampler_note_main_thread() {  }
void sampler_note_pump() {  }
void sampler_recover() {  }
void sampler_start(std::uint64_t guest_lo, std::uint64_t guest_hi) {  }
void sampler_watchdog_start(std::uint64_t guest_lo, std::uint64_t guest_hi) {  }

// host/translation_cache.h // namespace gpu
namespace gpu {
void hlsl_counts(std::uint64_t& taken, std::uint64_t& compiled, std::uint64_t& kept, std::string& reasons) {  }
void hlsl_lane(int lane) {  }
int hlsl_lane_now() { return {}; }
bool hlsl_lift_of(const std::vector<std::uint32_t>& module, std::vector<std::uint32_t>& lift) { return {}; }
int hlsl_mode() { return {}; }
void hlsl_name_next(std::string name, std::uint64_t salt) {  }
void hlsl_note_drawn(std::uint64_t stage_key) {  }
void hlsl_note_lift_again(int stages) {  }
void hlsl_note_override_refused(const std::vector<std::uint32_t>& module) {  }
void hlsl_note_refused(const char* what, int stages, bool rebuilt) {  }
bool hlsl_on() { return {}; }
void hlsl_set_busy_probe(bool (*busy)()) {  }
void hlsl_set_folders(const std::string& bbhost_dir) {  }
void hlsl_set_inline(bool on) {  }
void hlsl_set_name_lookup(std::string (*lookup)(const std::string& name)) {  }
void hlsl_set_overrides(bool on, const std::string& why_off) {  }
void hlsl_set_setting(bool hlsl) {  }
void hlsl_tag_stage(std::uint64_t stage_key) {  }
bool hlsl_took_override() { return {}; }
gcn::LiftResult lift_cached(bool vertex, const gcn::Program& prog, const gcn::TranslateOptions& o, const gcn::TranslateResult& reference) { return {}; }
gcn::LiftResult lift_compute_cached(const gcn::Program& prog, const gcn::TranslateOptions& o, const gcn::TranslateResult& reference) { return {}; }
gcn::LiftResult lift_gs_cached(TranslationUse use, const gcn::Program& prog, const gcn::TranslateOptions& o, const gcn::TranslateResult& reference, const gcn::GsLift& gs) { return {}; }
gcn::LiftResult lift_tess_cached(TranslationUse use, const gcn::Program& prog, const gcn::TranslateOptions& o, const gcn::TranslateResult& reference, const gcn::TessLift& tess) { return {}; }
gcn::TranslateResult translate_cached(const gcn::Program& prog, const gcn::TranslateOptions& o, TranslationUse use) { return {}; }
void translation_cache_bounded(bool on) {  }
std::uint64_t translation_cache_bounded_left_out() { return {}; }
bool translation_cache_cold() { return {}; }
void translation_cache_load(const std::string& path) {  }
void translation_cache_report() {  }
void translation_cache_save(const std::string& path, bool force) {  }
void translation_cache_save_async(const std::string& path) {  }
std::string translation_cache_window() { return {}; }
const char* translator_fingerprint() { return {}; }
bool velo_cloth_vs(const std::string& name) { return {}; }
}  // namespace gpu

// host/window.h
float host_display_refresh_hz() { return {}; }
void host_fps_frame_start() {  }
InputDevice host_input_device() { return {}; }
std::uint64_t host_input_serial() { return {}; }
bool host_key_strong() { return {}; }
void host_message_show(const char* text, float seconds) {  }
void host_mouse_describe(char* out, std::size_t n) {  }
bool host_mouse_position(float& x, float& y) { return {}; }
void host_mouse_set_relative(bool on) {  }
void host_mouse_set_visible(bool visible) {  }
bool host_mouse_stage_position(float& x, float& y) { return {}; }
MouseState host_mouse_state() { return {}; }
void host_pad_rumble(std::uint8_t small, std::uint8_t large) {  }
bool host_plugin_input(const char* reader, PluginInput& out) { return {}; }
void host_plugin_input_take(const char* owner, const std::uint16_t* scancodes, std::size_t n, std::uint32_t buttons, bool wheel, bool motion) {  }
void host_plugin_pad(PluginPad& out) {  }
void host_plugin_pad_frame() {  }
void host_plugin_pad_take(const char* owner, std::uint32_t buttons, std::uint32_t axes) {  }
std::uint32_t host_plugin_pad_taken_axes() { return {}; }
std::uint32_t host_plugin_pad_taken_buttons() { return {}; }
void host_plugin_pad_test(std::uint32_t buttons, std::uint8_t lx, std::uint8_t ly, std::uint8_t rx, std::uint8_t ry) {  }
void host_plugin_take_drop(const char* owner) {  }
std::uint32_t host_plugin_taken_buttons() { return {}; }
bool host_plugin_taken_wheel() { return {}; }
std::uint64_t host_present_last_ns(std::uint64_t* serial) { return {}; }
bool host_test_keys_play(const char* path) { return {}; }
void host_text_entry_begin(const char* initial_utf8, unsigned max_chars, bool hidden, const char* label, TextCharset charset, bool select_initial) {  }
void host_text_entry_end() {  }
int host_text_entry_poll(std::string& text_utf8) { return {}; }
void host_text_show(const char* text, float seconds, float x, float y, float scale, std::uint32_t align) {  }
bool host_window_desktop_size(int* w, int* h) { return {}; }
void host_window_error_box(const char* title, const char* text) {  }
void host_window_on_pixels(void (*fn)(int, int)) {  }
bool host_window_pixels(int* w, int* h) { return {}; }
void host_window_recreate_swapchain() {  }
float host_window_refresh_hz() { return {}; }
void host_window_request_quit() {  }
void host_window_set_mode(int mode, int width, int height) {  }
void host_window_set_stay_borderless(bool on) {  }
void host_window_set_vsync(bool on) {  }
bool host_window_start(int width, int height, const char* title) { return {}; }


// The list pool's resting flag (`gxvk_lists_pool.cpp` is not compiled, and the GX
// walk reads it as "no list is being built", which is what it is here).
namespace gxvk_lists {
std::atomic<bool> g_resting{false};
}  // namespace gxvk_lists

// The event interpreter's registration (`decomp/events/interpreter.cpp` is not
// compiled: it includes a header the vendored tree does not carry).
void decomp_event_interpreter_add() {}


// --- the rest of the residue ----------------------------------------------
// What the census found next: the hooks, the option rows' drawing, the list
// pool, the settings listener and the two arm calls. Same quiet answer.

bool engine_prologue_hook(ElfImage* image, std::uint64_t at, const std::uint8_t* prologue, std::size_t n, void* host, std::uint64_t id) { return {}; }
bool engine_prologue_hook_exec(ElfImage* image, std::uint64_t at, const std::uint8_t* prologue, std::size_t n, const std::uint8_t* exec, std::size_t m, void* host, std::uint64_t id) { return {}; }
bool engine_call_site_hook(ElfImage* image, std::uint64_t at, std::uint64_t target, void* host) { return {}; }
bool menu_pointer_in_menu() { return {}; }
void hle_watch_arm(std::uint64_t lo, std::uint64_t hi) {  }
void hle_bp_arm(std::uint64_t va) {  }
bool host_gpu_list_committed(std::uint64_t id) { return {}; }
bool host_gpu_writes_pending(std::uint64_t va, std::size_t bytes) { return {}; }
bool host_gpu_copy_guest(std::uint64_t dst, std::uint64_t src, std::size_t bytes, std::uint64_t src_gen, std::uint64_t dst_gen, std::uint32_t cap_at, std::uint32_t cap_piece) { return {}; }
bool host_gpu_texture_create_ahead(const std::uint32_t tsharp[8], std::uint64_t gen) { return {}; }
bool plugin_panel_native_step(int row, int dir) { return {}; }
bool plugin_panel_native_set(int row, int value) { return {}; }
bool plugin_panel_native_turn(int dir) { return {}; }
bool plugin_panel_native_back() { return {}; }
bool plugins_active(const std::string& name) { return {}; }
void host_settings_listen(HostSettingsListener fn) {  }

namespace option_rows {
void* fn(std::uint64_t bn) { return {}; }
std::int64_t open_section(void* root, void* params, const char* name, void* handler) { return {}; }
void handler_begin() {  }
void add_text_row(void* dialog, std::uint8_t* value, std::uint32_t id, const char16_t* label, const char16_t* help, const char16_t* text) {  }
void add_toggle_row(void* dialog, std::uint8_t* value, std::uint32_t id, std::uint8_t def, const char16_t* label, const char16_t* help) {  }
void add_pair_row(void* dialog, std::uint8_t* value, std::uint32_t id, const char16_t* first, const char16_t* second, std::uint8_t def, const char16_t* label, const char16_t* help) {  }
void add_slider_row(void* dialog, std::uint8_t* value, std::uint32_t id, std::uint8_t def, const char16_t* label, const char16_t* help) {  }
std::uint8_t* rows_list(void* dialog) { return {}; }
std::uint8_t* value_list(std::uint8_t* widget) { return {}; }
void redraw(std::uint8_t* list) {  }
void set_cursor(std::uint8_t* list, int index) {  }
int cursor_of(std::uint8_t* list) { return {}; }
const char16_t* dialog_message(std::uint32_t id) { return {}; }
std::uint64_t ask(std::uint8_t* dialog, std::uint32_t message, void* yes) { return {}; }
void release(std::uint64_t obj) {  }
std::int32_t refs(std::uint64_t obj) { return {}; }
void add_command_row(void* builder, std::uint32_t id, void* open) {  }
void* section_screen(void* out, void* ctx, void* opener) { return {}; }
}  // namespace option_rows


// --- the second census ------------------------------------------------------

std::uint32_t menu_confirm_button() { return {}; }
std::uint32_t menu_back_button() { return {}; }
void hle_watch_arm_reads(std::uint64_t lo, std::uint64_t hi) {  }
void hle_watch_report() {  }
bool host_gpu_splice(std::uint64_t id) { return {}; }
bool ingame_menu_open() { return {}; }
bool host_options_open() { return {}; }
bool plugin_panel_native_corner_wanted() { return {}; }
const HostSettings& host_settings_ref() { static const HostSettings none{}; return none; }
bool host_mouse_relative() { return {}; }
bool host_plugin_taken_motion() { return {}; }
bool host_text_entry_open() { return {}; }

namespace gxvk_lists {
std::string run_duty_report() { return {}; }
void cp_busy(std::uint64_t ns) {  }
void cp_held(std::uint64_t ns) {  }
void cp_thread_note(std::uintptr_t clock) {  }
void keep_off_pool_cpus() {  }
void draw_stored(std::uint64_t id, std::uint64_t ctx, std::uint64_t token_va, bool feed) {  }
void cp_look_begin(std::uint64_t id) {  }
void cp_look_end() {  }
void pool_report(bool at_exit) {  }
}  // namespace gxvk_lists

namespace option_rows {
bool ready() { return {}; }
std::uint64_t addr(std::uint64_t bn) { return {}; }
int row_count(void* dialog) { return {}; }
std::uint8_t* row_widget(void* dialog, int row) { return {}; }
std::uint64_t value_target(std::uint8_t* widget) { return {}; }
void layout(void* dialog) {  }
void clear_rows(void* dialog) {  }
}  // namespace option_rows

// The list pool's sampling interval: every draw, as its `BBHOST_GXVK_LISTS_SAMPLE`
// default says.
namespace gxvk_lists {
const unsigned g_look_every = 1;
}  // namespace gxvk_lists

// The launcher's text entry has no editable field on the console yet, so the
// guest's player-name prompt is a widget nobody can type into.
void TextEntry::begin(const char* initial_utf8, unsigned max, TextCharset cs, bool select_initial) {
    text = initial_utf8 == nullptr ? std::string() : std::string(initial_utf8);
    max_chars = max;
    charset = cs;
    // The suggestion shows selected, so the first character the guest "types"
    // replaces it - which is what a name box's preset wants.
    selected = select_initial;
}

// --- the 128-bit division the compiler emits --------------------------------
// One guest translation unit divides a 128-bit value (the GX walk's byte
// counters), and clang-cl asks the C runtime for `__udivti3`, which MSVC's
// libraries do not ship. Restoring division, written without any division, so
// the helper cannot call itself.
extern "C" unsigned __int128 __udivti3(unsigned __int128 n, unsigned __int128 d) {
    if (d == 0) return 0;
    unsigned __int128 q = 0, r = 0;
    for (int bit = 127; bit >= 0; --bit) {
        r = (r << 1) | ((n >> bit) & 1);
        if (r >= d) {
            r -= d;
            q |= (static_cast<unsigned __int128>(1) << bit);
        }
    }
    return q;
}

extern "C" unsigned __int128 __umodti3(unsigned __int128 n, unsigned __int128 d) {
    if (d == 0) return 0;
    unsigned __int128 r = 0;
    for (int bit = 127; bit >= 0; --bit) {
        r = (r << 1) | ((n >> bit) & 1);
        if (r >= d) r -= d;
    }
    return r;
}

// --- the boot --------------------------------------------------------------
#if defined(BBX_GUEST_BOOT)
namespace bbx::bridge {
namespace {

// The guest's memory, the entry point and the argv block all outlive this call:
// `_start` runs on its own thread until the game either finishes or the player
// closes the window, so none of it may live in a stack frame.
ElfImage g_image;
std::string g_eboot_path;

// The baked console shaders, loaded once at boot: the dump's copy, else the one
// a developer put in LocalState. Every blob in it was compiled for this build's
// DXIL target (dxil-6.4, tables, 11_0) by the bake, and ParseCache validated
// every entry before one was trusted.
bbx::d3d12::ShaderCache g_shader_cache;
alignas(16) std::uint64_t g_argv[4] = {0, 0, 0, 0};
std::atomic<bool> g_running{false};

// The guest's main thread. Eight MiB, reserved rather than committed (the
// launcher's own stack budget), and entered through the thunk that puts the
// guest's TCB in this thread's TLS slot - the rewritten `fs:[0]` reads it.
constexpr std::size_t kGuestStack = 8ull * 1024 * 1024;

struct GuestLaunch {
    GuestStart fn = nullptr;
    void* argv = nullptr;
};

DWORD WINAPI GuestMain(void* p) {
    auto* launch = static_cast<GuestLaunch*>(p);
    void* tcb = hle_thread_enter_guest();
    launch->fn(launch->argv, nullptr);
    hle_thread_leave_guest(tcb);
    g_running.store(false);
    bbx::platform::LogF("guest: _start returned");
    return 0;
}

}  // namespace

namespace {

// <dump>/bbx_shaders.cache and its sidecar, else the LocalState copy of both.
// Both are read through the host-fs seam: a dump arrives through a
// FutureAccessList token, which fopen cannot open on the console.
void LoadShaderCache(const std::string& eboot) {
    std::string dir;
    const std::size_t slash = eboot.find_last_of("\\/");
    if (slash != std::string::npos) dir = eboot.substr(0, slash + 1);
    std::vector<std::string> candidates;
    if (!dir.empty()) candidates.push_back(dir + "bbx_shaders.cache");
    candidates.push_back(bbx::platform::Local("bbx_shaders.cache"));
    for (const std::string& path : candidates) {
        std::vector<std::uint8_t> bytes;
        if (!bbx::hostfs::ReadFileAll(path, &bytes)) continue;
        std::vector<std::uint8_t> stamp;
        // The sidecar is `<cache>.stamp.txt`: the bake's record of who compiled
        // it, which is what ExpectedConsoleStamp pairs with this build's target.
        if (!bbx::hostfs::ReadFileAll(path + ".stamp.txt", &stamp)) {
            bbx::platform::LogMessage(bbx::platform::Level::kWarn,
                                      "shader_cache: %s has no .stamp.txt beside it; not used", path.c_str());
            continue;
        }
        const std::string stamp_text(reinterpret_cast<const char*>(stamp.data()), stamp.size());
        const bbx::d3d12::CacheStatus status = g_shader_cache.Load(bytes, stamp_text);
        bbx::platform::LogF("shader_cache: status=%s entries=%zu bytes=%zu path=%s detail=%s",
                            bbx::d3d12::CacheStatusName(status), g_shader_cache.entries(), g_shader_cache.bytes(),
                            path.c_str(), g_shader_cache.detail().c_str());
        return;
    }
    bbx::platform::LogMessage(bbx::platform::Level::kInfo,
                              "shader_cache: none in the dump or LocalState; every program is lifted on this machine");
}

}  // namespace

bool StartGuest(const GuestRequest& req) {
    if (req.eboot.empty()) {
        bbx::platform::LogMessage(bbx::platform::Level::kError, "boot: no eboot.bin in the dump");
        return false;
    }
    if (g_running.load()) {
        bbx::platform::LogMessage(bbx::platform::Level::kWarn, "boot: already running");
        return true;
    }
    g_eboot_path = req.eboot;

    // The HLE's file system: which folder is app0, which overlay is the update,
    // where saves and temporaries go, and where the player's loose files are.
    bbx::platform::LogCrumb("boot:roots");
    hle_fs_set_roots(req.app0.c_str(), req.data.empty() ? nullptr : req.data.c_str(),
                     req.tmp.empty() ? nullptr : req.tmp.c_str(), g_eboot_path.c_str(),
                     req.mods.empty() ? nullptr : req.mods.c_str());
    // The import table: every sce* the guest can call, bound to the HLE.
    bbx::platform::LogCrumb("boot:register_hle");
    register_hle();

    bbx::platform::LogCrumb("boot:load_elf");
    g_image = ElfImage{};
    // The loader unwraps the dump's fake SELF (4F 15 3D 1D) itself, so a dumped
    // eboot.bin and a flat ELF both work.
    if (!load_orbis_elf(g_eboot_path.c_str(), &g_image, legacy_cpu_arena_size())) {
        bbx::platform::LogMessage(bbx::platform::Level::kError, "boot: %s did not load", g_eboot_path.c_str());
        return false;
    }
    bbx::platform::LogF("image: sha256 %s entry 0x%llx base 0x%llx size 0x%zx",
                        g_image.sha256.c_str(), static_cast<unsigned long long>(g_image.entry),
                        static_cast<unsigned long long>(g_image.mem.slide),
                        static_cast<std::size_t>(g_image.mem.size));
    // The flip's view of the arena, before any guest instruction runs: the
    // display buffers are registered once the game is up, and host_present
    // resolves them here rather than trusting them.
    g_guest_base.store(static_cast<std::uint8_t*>(g_image.mem.base), std::memory_order_release);
    g_guest_slide.store(g_image.mem.slide, std::memory_order_relaxed);
    g_guest_size.store(g_image.mem.size, std::memory_order_relaxed);
    const bool is_109 = g_image.sha256 == kEboot109Sha256;
    if (!is_109 && !req.modded_eboot_ok) {
        // Every patch, hook and engine address below is an address in 1.09; a
        // different build would fail far from the cause, on a pointer read out
        // of a struct that moved. Refused here, with the digest in the log.
        bbx::platform::LogMessage(bbx::platform::Level::kError,
                                  "boot: this eboot is not 1.09 (sha256 %s, expected %s); enable "
                                  "\"Allow modded v1.09 eboot\" in the launcher to start it anyway",
                                  g_image.sha256.c_str(), kEboot109Sha256);
        unload_elf(&g_image);
        return false;
    }
    if (!is_109) {
        bbx::platform::LogMessage(bbx::platform::Level::kWarn,
                                  "boot: starting an eboot that is not 1.09; the patches may write "
                                  "into the wrong places");
    }

    // The baked shaders, if the dump carries the cache: compiled for this
    // target on a PC, so the guest's own programs do not have to be lifted and
    // compiled at the frame they are first drawn.
    bbx::platform::LogCrumb("boot:shader_cache");
    LoadShaderCache(g_eboot_path);

    bbx::platform::LogCrumb("boot:patch_guest");
    // The instructions this processor cannot run (BMI1, LZCNT, MOVBE), rewritten
    // before anything else touches the code.
    legacy_cpu_rewrite(&g_image);
    perf_meter_start(g_image.mem.slide, g_image.mem.slide + g_image.mem.size, is_109);
    main_wait_guest_base(g_image.mem.slide);
    // The launcher has the window: the guest must not try to open one, and the
    // display buffers are sized for the television the launcher already has.
    host_set_headless(false);
    hle_patch_guest(&g_image);
    debug_menu_install(&g_image);
    legacy_cpu_verify("before the guest starts");

    // The guest sees no host flags: argc = 1, argv[0] = the eboot's path, and
    // the 16-byte alignment the SysV ABI's prologues assume.
    g_argv[0] = 1;
    g_argv[1] = reinterpret_cast<std::uint64_t>(g_eboot_path.c_str());
    g_argv[2] = 0;

    static GuestLaunch launch;
    launch.fn = reinterpret_cast<GuestStart>(g_image.entry);
    launch.argv = g_argv;
    bbx::platform::LogCrumb("boot:guest_start");
    bbx::platform::LogF("guest: calling _start 0x%llx on an %zu MiB stack",
                        static_cast<unsigned long long>(g_image.entry), kGuestStack >> 20);
    g_running.store(true);
    const HANDLE thread = CreateThread(nullptr, kGuestStack, GuestMain, &launch,
                                       STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread == nullptr) {
        g_running.store(false);
        bbx::platform::LogMessage(bbx::platform::Level::kError,
                                  "guest: CreateThread with an %zu MiB stack failed (error %lu)",
                                  kGuestStack >> 20, static_cast<unsigned long>(GetLastError()));
        unload_elf(&g_image);
        return false;
    }
    CloseHandle(thread);  // the guest runs to _start's return; the handle is not needed
    return true;
}

std::string ShaderCacheLine() { return g_shader_cache.Report(); }

bool ShaderCacheLookup(std::uint64_t key, const std::uint8_t** bytes, std::uint32_t* size,
                       std::uint32_t* raw_size) {
    bbx::d3d12::BakedBlob blob;
    if (!g_shader_cache.Lookup(key, &blob)) return false;
    if (bytes != nullptr) *bytes = blob.data;
    if (size != nullptr) *size = blob.size;
    if (raw_size != nullptr) *raw_size = blob.raw_size;
    return true;
}

bool Running() { return g_running.load(); }

}  // namespace bbx::bridge
#else
// The link is off (xbox/CMakeLists.txt, BBX_GUEST_LINK): the archives are
// compiled but not part of this binary, so there is no loader to call and no
// guest to start. The launcher says so and stays up. This arm is for renderer
// iteration only: CMake refuses `BBX_GUEST_LINK=OFF` without `BBX_GUESTLESS=ON`,
// and the workflow's "guest link evidence" step fails any package that reached
// this branch, so an app that boots nothing cannot be shipped by accident.
namespace bbx::bridge {
bool StartGuest(const GuestRequest& req) {
    (void)req;
    bbx::platform::LogMessage(bbx::platform::Level::kWarn,
                              "boot: the guest runtime is not linked into this build (BBX_GUEST_LINK=OFF)");
    return false;
}
bool Running() { return false; }
std::string ShaderCacheLine() { return "no cache: the guest runtime is not linked into this build"; }
bool ShaderCacheLookup(std::uint64_t key, const std::uint8_t** bytes, std::uint32_t* size,
                       std::uint32_t* raw_size) {
    (void)key;
    if (bytes != nullptr) *bytes = nullptr;
    if (size != nullptr) *size = 0;
    if (raw_size != nullptr) *raw_size = 0;
    return false;
}
}  // namespace bbx::bridge
#endif
