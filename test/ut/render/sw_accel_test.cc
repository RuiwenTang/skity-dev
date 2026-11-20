// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <gtest/gtest.h>

#ifdef SKITY_SW_ACCEL

#include "src/render/sw/accel/sw_accel.hpp"

TEST(SWAccel, HighwayDispatchWorks) {
  EXPECT_TRUE(skity::sw_accel::SelfTest());
}

#endif  // SKITY_SW_ACCEL
