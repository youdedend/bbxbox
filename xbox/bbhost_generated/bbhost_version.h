// SPDX-License-Identifier: GPL-3.0-or-later
// The version string bbhost's build normally generates.
//
// Upstream's build writes `bbhost_version.h` from its git tag at configure time.
// This tree vendors bbhost without its build files (`UPSTREAM-PIN` names the
// commit), so the port stamps its own: `src/core/config.cpp` uses it as the
// settings-file version inside BBHOST_CONFIG_VERSION, and `src/hle/http.cpp`
// sends it as the user agent, which is the one place it is visible outside the
// process. There is no updater on a console, so a tag comparison has nothing to
// compare against and the string only has to be stable and true.
#pragma once

#define BBHOST_VERSION "0.9-bbxbox"
#define BBHOST_CONFIG_VERSION_STRING "0.9-bbxbox"
