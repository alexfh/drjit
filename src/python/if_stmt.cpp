/*
    if_stmt.cpp -- Python implementation of drjit.if_stmt() based on the
    abstract interface ad_cond() provided by the drjit-extra library

    Dr.Jit: A Just-In-Time-Compiler for Differentiable Rendering
    Copyright 2023, Realistic Graphics Lab, EPFL.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include "if_stmt.h"
#include "pystate.h"
#include "misc.h"
#include "apply.h"
#include "base.h"
#include "shape.h"
#include <nanobind/stl/vector.h>
#include <nanobind/stl/string.h>

/// State object passed to callbacks that implement the Python interface around ad_cond().
struct IfState {
    nb::tuple args;
    nb::callable true_fn, false_fn;
    nb::object rv;
    std::vector<std::string> rv_labels;
    CopyMap copy_map;
    std::vector<StashRef> sr;

    IfState(nb::tuple &&args, nb::callable &&true_fn, nb::callable &&false_fn,
            std::vector<std::string> &&rv_labels)
        : args(std::move(args)), true_fn(std::move(true_fn)),
          false_fn(std::move(false_fn)), rv_labels(std::move(rv_labels)) { }

    void cleanup() {
        copy_map.clear();
        sr.clear();
    }
};


static void if_stmt_body_cb(void *p, bool value) {
    nb::gil_scoped_acquire guard;
    IfState *is = (IfState *) p;
    nb::callable &fn = value ? is->true_fn : is->false_fn;

    // Temporarily stash the reference counts of inputs. This influences the
    // behavior of copy-on-write (COW) operations like dr.scatter performed
    // within the symbolic region
    stash_ref(is->args, is->sr);

    // Copy the input arguments to prevent 'true_fn' from mutating values that
    // are subsequently accessed by 'false_fn'.
    nb::tuple args = nb::borrow<nb::tuple>(copy(is->args, &is->copy_map));

    // Run the operation
    nb::object rv = tuple_call(fn, args);

    // Propagate side effects back to 'args'
    for (auto [h1, h2] : is->copy_map.map) {
        nb::handle tp = h1.type();
        if (!is_drjit_type(tp))
            continue;

        const ArraySupplement &s = supp(tp);
        if (!s.index)
            continue;

        uint64_t i1 = s.index(inst_ptr(h1)),
                 i2 = s.index(inst_ptr(h2));

        if (i1 != i2 && jit_var_state((uint32_t) i1) == VarState::Dirty) {
            stash_ref(h1, is->sr);
            nb::inst_replace_copy(h2, h1);
        }
    }

    // Ensure that the output of 'true_fn' and 'false_fn' is consistent
    if (is->rv.is_valid()) {
        size_t l1 = is->rv_labels.size(), l2 = (size_t) -1, l3 = (size_t) -1;

        try {
           l2 = nb::len(is->rv);
           l3 = nb::len(rv);
        } catch (...) { }

        try {
            if (l1 == l2 && l2 == l3 && l3 > 0) {
                for (size_t i = 0; i < l1; ++i)
                    check_compatibility(is->rv[i], rv[i],
                                        is->rv_labels[i].c_str());
            } else {
                check_compatibility(is->rv, rv, "result");
            }
        } catch (const std::exception &e) {
            nb::raise("detected an inconsistency when comparing the return "
                      "values of 'true_fn' and 'false_fn':\n%s\n\nPlease review "
                      "the interface and assumptions of dr.if_stmt() as "
                      "explained in the Dr.Jit documentation.", e.what());
        }
    }

    is->rv = std::move(rv);
}

static void if_stmt_delete_cb(void *p) {
    if (!nb::is_alive())
        return;
    nb::gil_scoped_acquire guard;
    delete (IfState *) p;
}

static void if_stmt_read_cb(void *p, dr::dr_vector<uint64_t> &indices) {
    nb::gil_scoped_acquire guard;
    collect_indices(((IfState *) p)->rv, indices, true);
}

static void if_stmt_write_cb(void *p, const dr::dr_vector<uint64_t> &indices) {
    IfState *is = (IfState *) p;
    nb::gil_scoped_acquire guard;
    is->rv = update_indices(is->rv, indices, &is->copy_map);
}

nb::object if_stmt(nb::tuple args, nb::handle cond, nb::callable true_fn,
                   nb::callable false_fn, std::vector<std::string> &&rv_labels,
                   const std::string &name, const std::string &mode) {
    try {
        (void) rv_labels;
        JitBackend backend = JitBackend::None;
        uint32_t cond_index = 0;

        bool is_scalar = mode == "scalar",
             is_auto = mode == "auto";

        if (is_auto)
            is_scalar = cond.type().is(&PyBool_Type);

        if (!is_scalar) {
            nb::handle tp = cond.type();

            if (is_drjit_type(tp)) {
                const ArraySupplement &s = supp(tp);
                if ((VarType) s.type == VarType::Bool && s.ndim == 1 &&
                    (JitBackend) s.backend != JitBackend::None) {
                    backend = (JitBackend) s.backend;
                    cond_index = s.index(inst_ptr(cond));
                    if (!cond_index)
                        nb::raise("'cond' cannot be empty.");
                }
            }

            if (!cond_index)
                nb::raise("'cond' must either be a Jit-compiled 1D Boolean "
                          "array or a Python 'bool'.");
        }

        if (is_scalar) {
            // If so, process it directly
            if (nb::cast<bool>(cond))
                return tuple_call(true_fn, args);
            else
                return tuple_call(false_fn, args);
        }

        // General case: call ad_cond() with a number of callbacks that
        // implement an interface to Python
        int symbolic = -1;
        if (is_auto)
            symbolic = -1;
        else if (mode == "symbolic")
            symbolic = 1;
        else if (mode == "evaluated")
            symbolic = 0;
        else
            nb::raise("invalid 'mode' argument (must equal \"auto\", "
                      "\"scalar\", \"symbolic\", or \"evaluated\").");

        IfState *is =
            new IfState(std::move(args), std::move(true_fn),
                        std::move(false_fn), std::move(rv_labels));

        bool status = ad_cond(backend, symbolic, name.c_str(), is,
                              cond_index, if_stmt_read_cb, if_stmt_write_cb,
                              if_stmt_body_cb, if_stmt_delete_cb, true);

        nb::object result = uncopy(is->rv, is->copy_map);

        if (status) {
            delete is;
        } else {
            is->rv = reset(is->rv);
            is->cleanup();
        }

        return result;
    } catch (nb::python_error &e) {
        nb::raise_from(
            e, PyExc_RuntimeError,
            "dr.if_stmt(): encountered an exception (see above).");
    } catch (const std::exception &e) {
        nb::chain_error(PyExc_RuntimeError, "dr.if_stmt(): %s", e.what());
        throw nb::python_error();
    }
}

void export_if_stmt(nb::module_ &m) {
    m.def("if_stmt", &if_stmt, "args"_a, "cond"_a, "true_fn"_a, "false_fn"_a,
          "rv_labels"_a = nb::make_tuple(), "label"_a = "unnamed",
          "mode"_a = "auto", nb::raw_doc(doc_if_stmt));
}
