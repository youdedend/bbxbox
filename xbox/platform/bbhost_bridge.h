// SPDX-License-Identifier: GPL-3.0-or-later
// What the launcher asks of the guest's half of the port.
//
// Two things cross this line. The controller, because the launcher opens the one
// the player holds and the game has to read the same pad; and the boot, because
// the launcher's Start is the moment the guest is loaded, patched and given its
// thread. Everything else in `platform/bbhost_bridge.cpp` answers bbhost's own
// headers and is never named here.
#pragma once

#include <cstdint>
#include <string>

struct SDL_Gamepad;

namespace bbx::platform {
class D3d12Session;
}

namespace bbx::bridge {

// The window's GPU session, so a flip can reach the swapchain: the launcher
// sets it once the device is up, and host_present hands it the guest's scanout
// buffer without ever waiting for it.
void AttachFrameSink(bbx::platform::D3d12Session* session);

// The pad the launcher's event loop is reading. Called once when a controller
// arrives; the game's reads translate it (host/window.h's PadState).
void SetPad(SDL_Gamepad* pad);

// Where the dump is, in the terms the HLE's file system wants: the folder with
// eboot.bin, the update overlay when the player is running a patched build, the
// writable temporary directory, and the mods overlay.
struct GuestRequest {
    std::string app0;
    std::string data;
    std::string tmp;
    std::string eboot;
    std::string mods;
    // The launcher's "Allow modded v1.09 eboot" switch: without it an eboot whose
    // sha256 is not 1.09's is refused before anything is patched.
    bool modded_eboot_ok = false;
};

// Loads, patches and starts the guest on its own thread. True when the thread
// is running (the launcher then keeps drawing while the game boots); false when
// the eboot could not be used, with the reason in the log.
bool StartGuest(const GuestRequest& req);

// True between StartGuest's success and the guest's `_start` returning.
bool Running();

// Frames the guest has asked to present (host_present), for the launcher's HUD.
std::uint64_t Flips();

// The baked console shaders (<dump>/bbx_shaders.cache), once StartGuest has
// loaded them: one line for the HUD, and a look-up by interface hash for the
// pipeline that needs the program. False when the cache did not validate, with
// the reason in the line.
std::string ShaderCacheLine();
bool ShaderCacheLookup(std::uint64_t key, const std::uint8_t** bytes, std::uint32_t* size,
                       std::uint32_t* raw_size);

// The baked console shaders (<dump>/bbx_shaders.cache), once StartGuest has
// loaded them: one line for the HUD, and a look-up by interface hash for the
// pipeline that needs the program. False when the cache did not validate, with
// the reason in the line.
std::string ShaderCacheLine();
bool ShaderCacheLookup(std::uint64_t key, const std::uint8_t** bytes, std::uint32_t* size,
                       std::uint32_t* raw_size);

}  // namespace bbx::bridge
