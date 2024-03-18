/*
    common.h -- Common definitions used by the Dr.Jit Python bindings

    Dr.Jit: A Just-In-Time-Compiler for Differentiable Rendering
    Copyright 2023, Realistic Graphics Lab, EPFL.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#pragma once

#include <drjit/python.h>
#include <nanobind/stl/pair.h>
#include "docstr.h"

namespace nb = nanobind;
namespace dr = drjit;

using namespace nb::literals;

using dr::ArrayMeta;
using dr::ArraySupplement;
using dr::ArrayBinding;
using dr::ArrayOp;
using dr::ArrayBase;
using dr::vector;

inline const ArraySupplement &supp(nb::handle h) {
    return nb::type_supplement<ArraySupplement>(h);
}

inline ArrayBase* inst_ptr(nb::handle h) {
    return nb::inst_ptr<ArrayBase>(h);
}

/// Helper function to perform a tuple-based function call directly using the
/// CPython API. nanobind lacks a nice abstraction for this.
inline nb::object tuple_call(nb::handle callable, nb::handle tuple) {
    nb::object result = nb::steal(PyObject_CallObject(callable.ptr(), tuple.ptr()));
    if (!result.is_valid())
        nb::raise_python_error();
    return result;
}

#define raise_if(expr, ...)                                                    \
    do {                                                                       \
        if (NB_UNLIKELY(expr))                                                 \
            nb::raise(__VA_ARGS__);                                    \
    } while (false)
