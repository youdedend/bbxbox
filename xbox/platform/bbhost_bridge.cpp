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
//   * host_present - the flip, counted and logged, not yet drawn:
//     `d3d12_session` presents the launcher's HUD, and putting the guest's
//     display buffer on the television is the D3D12 backend's slice.

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
        if (!SDL_GetGamepadTouchpadFinger(pad, finger, &down, &x, &y, &pressure)) continue;
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

// --- the flip --------------------------------------------------------------
// Called by the guest's video HLE once per frame. The launcher's own loop is
// what runs now, so this only counts and reports: blocking here would stall the
// guest thread behind a presenter that is not listening to it yet.
void host_present(int buffer_index, std::uint64_t display_va, unsigned display_w, unsigned display_h,
                  const PresentFlip& flip) {
    (void)buffer_index; (void)display_va; (void)display_w; (void)display_h; (void)flip;
    ++g_flips;
    if ((g_flips % 60) == 1) {
        bbx::platform::LogCrumb("gpu:flip");
    }
}

// The launcher owns the window: there is nothing here to pump, nothing to
// close, and it is always in front while it is running.
bool host_window_pump() { return true; }
void host_window_stop() {}
bool host_window_active() { return true; }

// --- the seam's quiet half ------------------------------------------------
// Everything below answers "this machine has none of that": no options
// page, no plugin panel, no capture, no translation cache, no overlays, no
// second window. Each returns the declaration's own neutral value, so a
// guest that asks a question about one gets an answer it can act on
// instead of an unresolved symbol at load.


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

// host/translation_cache.h
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
void host_pad_rumble(std::uint8_t small_magnitude, std::uint8_t large_magnitude) {  }
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

// host/gpu.h
void host_gpu_compare_cs_user_data(const GxStageRecords& st, const std::uint32_t* reg, std::uint32_t read_mask, CsUserCompare& out) {  }
std::uint64_t host_gpu_completed_submits() { return {}; }
std::uint64_t host_gpu_completed_submits_seen() { return {}; }
std::size_t host_gpu_copy_backs_captured(const std::uint64_t* dst, const std::uint64_t* src, const std::uint64_t* bytes, const std::uint32_t* at, std::uint32_t piece, std::size_t n) { return {}; }
bool host_gpu_copy_image_region(const GpuImageCopy& c) { return {}; }
void host_gpu_depth_clear(std::uint64_t base, std::uint32_t word, std::uint32_t stencil) {  }
bool host_gpu_device_lost() { return {}; }
std::uint32_t host_gpu_dispatch_user_reads() { return {}; }
void host_gpu_explain_value(std::uint64_t va, std::uint64_t value) {  }
bool host_gpu_fill(std::uint64_t va, std::size_t bytes, const float rgba[4], std::uint32_t depth_clear, std::uint32_t stencil_clear) { return {}; }
std::string host_gpu_fill_stats() { return {}; }
void host_gpu_hang_report() {  }
void host_gpu_lists_arrival(std::uint64_t id) {  }
std::string host_gpu_lists_times_report(int part, std::uint64_t parked_ticks, std::uint64_t parks) { return {}; }
void host_gpu_pipeline_time_us(std::uint64_t out[5]) {  }
void host_gpu_pipeline_work(std::uint64_t out[14]) {  }
void host_gpu_read_guest(std::uint64_t va, std::size_t bytes, void* out) {  }
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
void next(int p) { if (weight) { const auto t = std::chrono::steady_clock::now() {  }
 static_assert(offsetof(GxStageRecords, desc) <= 64, "what the constructor clears is one line") {  }

// host/ingame_menu.h
bool ingame_menu_event(const SDL_Event& e, float pixel_ratio) { return {}; }
bool ingame_menu_init(VkInstance instance, VkPhysicalDevice phys, VkDevice device, std::uint32_t family, VkQueue queue, VkFormat format, std::uint32_t image_count) { return {}; }
void ingame_menu_pump() {  }
void ingame_menu_record(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent) {  }
bool ingame_menu_record_in_pass(VkCommandBuffer cmd, VkExtent2D extent) { return {}; }
void ingame_menu_shutdown() {  }

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

// host/sampler.h
void sampler_note_exit(const char* step) {  }
void sampler_note_main_thread() {  }
void sampler_note_pump() {  }
void sampler_recover() {  }
void sampler_start(std::uint64_t guest_lo, std::uint64_t guest_hi) {  }
void sampler_watchdog_start(std::uint64_t guest_lo, std::uint64_t guest_hi) {  }
