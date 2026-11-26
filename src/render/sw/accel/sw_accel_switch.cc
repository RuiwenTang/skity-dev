// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

// The accelerator runtime switch is compiled into the skity target itself,
// NOT into the skity_sw_accel archive. Test and benchmark executables link
// the archive statically; a switch defined there shadows the shared
// library's copy, leaving those binaries with two independent mirrors - the
// A/B arms toggled a state the renderer (inside libskity) never read, so
// every legacy arm silently measured the accelerated path. Keeping the
// switch entry in skity leaves exactly one mirror per process.
//
// The state itself now lives in the process-wide SWContext singleton; these
// entry points are thin delegates over it (see sw_context.hpp).

#include <skity/render/sw_context.hpp>

#include "src/render/sw/accel/sw_accel.hpp"

namespace skity {
namespace sw_accel {

void SetEnabled(bool enabled) { SWContext::Get()->SetAccelEnabled(enabled); }

bool IsEnabled() { return SWContext::Get()->AccelEnabled(); }

}  // namespace sw_accel
}  // namespace skity
