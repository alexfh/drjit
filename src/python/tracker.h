/*
    tracker.h -- Helper class to track variables representing arguments and
    return values in symbolic operations such as dr.while_loop, dr.if_stmt, etc.

    Dr.Jit: A Just-In-Time-Compiler for Differentiable Rendering
    Copyright 2023, Realistic Graphics Lab, EPFL.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#pragma once

#include "common.h"

extern void export_tracker(nb::module_ &);
