/*
    extra/call.cpp -- Logic to dispatch virtual function calls, dr.switch(),
    and dr.dispatch() through one common interface with support for symbolic
    and evaluated execution styles along with automatic differentiation.

    Dr.Jit is a C++ template library for efficient vectorization and
    differentiation of numerical kernels on modern processor architectures.

    Copyright (c) 2023 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a BSD-style
    license that can be found in the LICENSE file.
*/

#include <drjit/autodiff.h>
#include <drjit/custom.h>
#include <algorithm>
#include <string>
#include "common.h"

namespace dr = drjit;
using dr::dr_vector;

/// RAII helper to temporarily push a mask onto the Dr.Jit mask stack
struct scoped_set_mask {
    scoped_set_mask(JitBackend backend, uint32_t index) : backend(backend) {
        jit_var_mask_push(backend, index);
        jit_var_dec_ref(index);
    }

    ~scoped_set_mask() {
        jit_var_mask_pop(backend);
    }

    JitBackend backend;
};

/// RAII helper to temporarily record symbolic computation
struct scoped_record {
    scoped_record(JitBackend backend, const char *name) : backend(backend) {
        checkpoint = jit_record_begin(backend, name);
        scope = jit_new_scope(backend);
    }

    uint32_t checkpoint_and_rewind() {
        jit_set_scope(backend, scope);
        return jit_record_checkpoint(backend);
    }

    void disarm() { cleanup = false; }

    ~scoped_record() {
        jit_record_end(backend, checkpoint, cleanup);
    }

    JitBackend backend;
    uint32_t checkpoint, scope;
    bool cleanup = true;
};

/// RAII helper to temporarily set the 'self' instance
struct scoped_set_self {
    scoped_set_self(JitBackend backend, uint32_t value, uint32_t self_index = 0)
        : m_backend(backend) {
        jit_var_self(backend, &m_self_value, &m_self_index);
        jit_var_inc_ref(m_self_index);
        jit_var_set_self(m_backend, value, self_index);
    }

    ~scoped_set_self() {
        jit_var_set_self(m_backend, m_self_value, m_self_index);
        jit_var_dec_ref(m_self_index);
    }

private:
    JitBackend m_backend;
    uint32_t m_self_value;
    uint32_t m_self_index;
};

using JitVar = GenericArray<void>;

// Forward declaration of a helper function full of checks (used by all strategies)
static void ad_call_check_rv(JitBackend backend, size_t size,
                             size_t callable_index,
                             dr_vector<uint64_t> &rv,
                             const dr_vector<uint64_t> &rv2);

// Strategy 1: this is a getter. turn the call into a gather operation
static void ad_call_getter(JitBackend backend, const char *domain,
                           const char *name, size_t size, uint32_t index,
                           uint32_t mask_, size_t callable_count,
                           const dr_vector<uint64_t> args,
                           dr_vector<uint64_t> &rv, dr_vector<bool> &rv_ad,
                           ad_call_func func, void *payload) {

    dr_index64_vector args2; // unused
    dr_vector<uint64_t> rv2;
    dr_index32_vector rv3;
    dr_index32_vector cleanup;
    (void) args;

    JitVar null_instance = JitVar::steal(jit_var_u32(backend, 0)),
           is_non_null = JitVar::steal(jit_var_neq(index, null_instance.index())),
           mask = JitVar::steal(jit_var_and(mask_, is_non_null.index()));

    const char *domain_or_empty = domain ? domain : "",
               *separator = domain ? "::" : "";

    jit_log(LogLevel::InfoSym,
            "ad_call_getter(\"%s%s%s\", index=r%u, mask=r%u)", domain_or_empty,
            separator, name, index, mask.index());

    for (size_t i = 0; i < callable_count; ++i) {
        rv2.clear();

        void *ptr;
        if (domain) {
            ptr = jit_registry_ptr(backend, domain, (uint32_t) i + 1);
            if (!ptr)
                continue;
        } else {
            ptr = (void *) (uintptr_t) i;
        }

        {
            scoped_record rec(backend, name);
            func(payload, ptr, args2, rv2);
            for (uint64_t index: rv2)
                ad_var_check_implicit(index);
        }

        // Perform some sanity checks on the return values
        ad_call_check_rv(backend, size, i, rv, rv2);

        // Preallocate memory in the first iteration
        if (rv_ad.empty()) {
            rv3.reserve(rv2.size() * callable_count);
            rv_ad.resize(rv2.size(), false);
        }

        // Move return values to a separate array storing them for all callables
        for (size_t j = 0; j < rv2.size(); ++j) {
            uint64_t index = rv2[j];
            rv_ad[j] |= (index >> 32) != 0;
            if (!index)
                jit_raise(
                    "ad_call_getter(\"%s%s%s\"): return value of callable %zu "
                    "is empty/uninitialized, which is not permitted!",
                    domain_or_empty, separator, name, i);
            rv3.push_back_borrow((uint32_t) index);
            size_t size = jit_var_size((uint32_t) index);
            if (size != 1)
                jit_raise("ad_call_getter(\"%s%s%s\"): return value of "
                          "callable %zu is not a scalar (r%u has size %zu).",
                          domain_or_empty, separator, name, i, (uint32_t) index,
                          size);
        }
    }

    for (size_t i = 0; i < rv2.size(); ++i) {
        // Deallocate previous entry
        jit_var_dec_ref(rv[i]);
        rv[i] = 0;

        // Check if this is a literal
        bool is_literal = true;
        for (size_t j = 0; j < callable_count; ++j) {
            if (rv3[i] != rv3[i+j*rv2.size()])
                is_literal = false;
        }

        if (is_literal) {
            jit_var_inc_ref(rv3[i]);
            rv[i] = rv3[i];
            continue;
        }

        VarType type = jit_var_type(rv2[i]);
        size_t tsize = jit_type_size(type);

        void *ptr =
            jit_malloc(backend == JitBackend::CUDA ? AllocType::Device
                                                   : AllocType::HostAsync,
                       tsize * (callable_count + 1));

        JitVar buf = JitVar::steal(
            jit_var_mem_map(backend, type, ptr, callable_count + 1, 1));

        AggregationEntry *agg = (AggregationEntry *)
            jit_malloc(backend == JitBackend::CUDA ? AllocType::HostPinned
                                                   : AllocType::Host,
                        sizeof(AggregationEntry) * callable_count), *p = agg;

        for (size_t j = 0; j < callable_count; ++j) {
            uint32_t rv3_i = rv3[i+j*rv2.size()];
            VarState state = jit_var_state(rv3_i);
            p->offset = (j + 1) * tsize;

            switch (state) {
                case VarState::Literal:
                    p->size = (int) tsize;
                    p->src = 0;
                    jit_var_read(rv3_i, 0, &p->src);
                    break;

                case VarState::Unevaluated:
                case VarState::Evaluated:
                    p->size = -(int) tsize;
                    cleanup.push_back(jit_var_data(rv3_i, (void **) &p->src));
                    break;

                default:
                    jit_free(agg);
                    jit_raise("ad_call_getter(): invalid variable state");
            }

            p++;
        }

        jit_aggregate(backend, ptr, agg, (uint32_t) (p - agg));
        rv[i] = jit_var_gather(buf.index(), index, mask.index());
    }
}

// Strategy 2: perform symbolic indirection by tracing all callables
static void ad_call_record(JitBackend backend, const char *domain,
                            const char *name, size_t size, uint32_t index,
                            uint32_t mask_, size_t callable_count,
                            const dr_vector<uint64_t> args,
                            dr_vector<uint64_t> &rv, dr_vector<bool> &rv_ad,
                            ad_call_func func, void *payload) {
    (void) domain;
    (void) size;

    JitVar mask;
    if (mask_)
        mask = JitVar::borrow(mask_);
    else
        mask = JitVar::steal(jit_var_bool(backend, true));

    dr_index64_vector args2;
    dr_vector<uint64_t> rv2;

    dr_index32_vector args3, rv3;

    args2.reserve(args.size());
    args2.reserve(args.size());
    std::string combined(name);
    if (domain && combined.find("::") == std::string::npos)
        combined = std::string(domain) + "::" + combined;

    dr_vector<uint32_t> checkpoints(callable_count + 1, 0),
                            inst_id(callable_count, 0);

    {
        scoped_record rec(backend, name);

        // Wrap input arguments to clearly expose them as inputs of the vcall
        for (size_t i = 0; i < args.size(); ++i) {
            uint32_t wrapped = jit_var_call_input((uint32_t) args[i]);
            args3.push_back_steal(wrapped);

            if (args[i] >> 32)
                args2.push_back_steal(ad_var_new(wrapped));
            else
                args2.push_back_borrow(wrapped);
        }

        size_t callable_count_final = 0;
        {
            scoped_set_mask mask_guard(backend, jit_var_call_mask(backend));
            for (size_t i = 0; i < callable_count; ++i) {
                checkpoints[i] = rec.checkpoint_and_rewind();
                rv2.clear();

                void *ptr;
                if (domain) {
                    ptr = jit_registry_ptr(backend, domain, (uint32_t) i + 1);
                    if (!ptr)
                        continue;
                } else {
                    ptr = (void *) (uintptr_t) i;
                }
                callable_count_final++;

                // Populate 'rv2' with function return values. This may raise
                // an exception, in which case everything should be properly
                // cleaned up in this function's scope
                scoped_set_self set_self(backend, i + 1);
                func(payload, ptr, args2, rv2);
                inst_id[i] = i + 1;

                for (uint64_t index: rv2)
                    ad_var_check_implicit(index);

                // Perform some sanity checks on the return values
                ad_call_check_rv(backend, size, i, rv, rv2);

                // Move return values to a separate array storing them for all callables
                if (rv_ad.empty()) {
                    // Preallocate memory in the first iteration
                    rv3.reserve(rv2.size() * callable_count);
                    rv_ad.resize(rv2.size(), false);
                }

                for (size_t j = 0; j < rv2.size(); ++j) {
                    uint64_t index = rv2[j];
                    rv_ad[j] |= (index >> 32) != 0;
                    rv3.push_back_borrow((uint32_t) index);
                }
            }

            checkpoints[callable_count] = rec.checkpoint_and_rewind();
        }

        dr_vector<uint32_t> rv4;
        rv4.resize(rv.size());

        jit_var_call(
            combined.c_str(), index, mask.index(), callable_count_final,
            inst_id.data(), (uint32_t) args3.size(), args3.data(),
            (uint32_t) rv3.size(), rv3.data(), checkpoints.data(), rv4.data());

        for (size_t i = 0; i < rv.size(); ++i) {
            ad_var_dec_ref(rv[i]);
            rv[i] = rv4[i];
        }

        rec.disarm();
    }
}

// Strategy 3: group the arguments and evaluate a kernel per callable
static void ad_call_reduce(JitBackend backend, const char *domain,
                            const char *name, size_t size, uint32_t index_,
                            uint32_t mask, size_t callable_count,
                            const dr_vector<uint64_t> args,
                            dr_vector<uint64_t> &rv,
                            ad_call_func func, void *payload) {
    (void) name; // unused
    const char *domain_or_empty = domain ? domain : "",
               *separator = domain ? "::" : "";

    JitVar index;
    if (mask)
        index = JitVar::steal(jit_var_and(index_, mask));
    else
        index = JitVar::borrow(index_);

    jit_var_schedule(index.index());
    for (uint64_t arg_i : args)
        jit_var_schedule((uint32_t) arg_i);

    uint32_t n_inst = callable_count;
    CallBucket *buckets =
        jit_var_call_reduce(backend, domain, index.index(), &n_inst);

    dr_index64_vector args2(args.size(), 0);
    args2.clear();

    dr_vector<uint64_t> rv2;
    size_t last_size = 0;
    JitVar memop_mask = JitVar::steal(jit_var_bool(backend, true));

    for (size_t i = 0; i < n_inst ; ++i) {
        if (buckets[i].id == 0)
            continue;

        uint32_t callable_index = buckets[i].id - 1,
                 index2 = buckets[i].index;

        size_t wavefront_size = jit_var_size(index2);

        // Don't merge subsequent wavefronts into the same kernel,
        // which could happen if they have the same size
        if (last_size == wavefront_size)
            jit_eval();
        last_size = wavefront_size;

        // Fetch arguments
        scoped_set_mask mask_guard(
            backend, jit_var_mask_default(backend, wavefront_size));
        for (size_t j = 0; j < args.size(); ++j)
            args2.push_back_steal(
                ad_var_gather(args[j], index2, memop_mask.index(), true));

        // Populate 'rv2' with function return values. This may raise an
        // exception, in which case everything should be properly cleaned up in
        // this function's scope
        rv2.clear();

        void *ptr;
        if (domain) {
            ptr = jit_registry_ptr(backend, domain, buckets[i].id);
            if (!ptr)
                jit_raise(
                    "ad_call_reduce(\"%s%s%s\"): instance %u does not exist (or no longer exists).",
                    domain_or_empty, separator, name, buckets[i].id);
        } else {
            ptr = (void *) (uintptr_t) callable_index;
        }

        JitVar instance_id = JitVar::steal(
            ad_var_gather(index.index(), index2, memop_mask.index(), false));

        scoped_set_self set_self(backend, i + 1, instance_id.index());
        func(payload, ptr, args2, rv2);

        // Perform some sanity checks on the return values
        ad_call_check_rv(backend, size, i, rv, rv2);

        // Merge 'rv2' into 'rv' (main function return values)
        for (size_t j = 0; j < rv2.size(); ++j) {
            uint64_t index =
                ad_var_scatter(rv[j], rv2[j], index2, memop_mask.index(),
                               ReduceOp::None, true);
            ad_var_dec_ref(rv[j]);
            rv[j] = index;
        }

        args2.release();
    }

    for (uint64_t index : rv)
        jit_var_schedule((uint32_t) index);
}

// Helper function full of checks (used by all strategies)
static void ad_call_check_rv(JitBackend backend, size_t size,
                              size_t callable_index,
                              dr_vector<uint64_t> &rv,
                              const dr_vector<uint64_t> &rv2) {
    // Examine return values
    if (rv.size() != rv2.size()) {
        if (!rv.empty())
            jit_raise(
                "ad_call(): callable %zu returned an unexpected "
                "number of return values (got %zu indices, expected %zu)",
                callable_index, rv2.size(), rv.size());

        // Allocate a zero-initialized output array in the first iteration
        rv.resize(rv2.size());

        uint64_t zero = 0;
        for (size_t i = 0; i < rv2.size(); ++i) {
            if (rv2[i] == 0)
                jit_raise("ad_call(): callable %zu returned an empty/uninitialized "
                          "Dr.Jit array, which is not allowed", callable_index);

            rv[i] = jit_var_literal(backend, jit_var_type(rv2[i]), &zero, size);
        }
    } else {
        // Some sanity checks
        for (size_t i = 0; i < rv.size(); ++i) {
            uint64_t i1 = rv[i], i2 = rv2[i];

            if (i2 == 0)
                jit_raise("ad_call(): callable %zu returned an empty/uninitialized "
                          "Dr.Jit array, which is not allowed", callable_index);

            VarInfo v1 = jit_set_backend(i1),
                    v2 = jit_set_backend(i2);

            if (v2.backend != backend)
                jit_raise("ad_call(): callable %zu returned an array "
                          "with an inconsistent backend", callable_index);

            if (v1.type != v2.type)
                jit_raise("ad_call(): callable %zu returned an array "
                          "with an inconsistent type (%s vs %s)",
                          callable_index, jit_type_name(v1.type),
                          jit_type_name(v2.type));
        }
    }
}

/// CustomOp that hooks a recorded virtual function call into the AD graph
struct CallOp : public dr::detail::CustomOpBase {
public:
    CallOp(JitBackend backend, std::string &&name, const char *domain,
            uint32_t index, uint32_t mask, size_t callable_count,
            const dr_vector<uint64_t> &args, size_t rv_size, void *payload,
            ad_call_func func, ad_call_cleanup cleanup)
        : m_name(std::move(name)), m_domain(domain), m_index(index), m_mask(mask),
          m_callable_count(callable_count), m_payload(payload),
          m_func(func), m_cleanup(cleanup) {
        m_backend = backend;

        jit_var_inc_ref(m_index);
        jit_var_inc_ref(m_mask);

        m_args.reserve(args.size());
        m_args2.reserve(args.size());
        m_rv.reserve(rv_size);
        m_rv2.reserve(rv_size);

        m_input_offsets.reserve(args.size());
        m_output_offsets.reserve(rv_size);
        m_temp.reserve(std::max(args.size(), rv_size));

        for (size_t i = 0; i < args.size(); ++i)
            m_args.push_back_borrow((uint32_t) args[i]);

        m_name_op = "Call: " + m_name;
    }

    ~CallOp() {
        jit_var_dec_ref(m_index);
        jit_var_dec_ref(m_mask);
        if (m_cleanup)
            m_cleanup(m_payload);
    }

    uint64_t combine(uint32_t ad_index, uint32_t jit_index = 0) {
        return (((uint64_t) ad_index) << 32) + jit_index;
    }

    /// Implements f(arg..., grad(arg)...) -> grad(rv)...
    void forward() override {
        std::string name = m_name + " [ad, fwd]";

        dr_index64_vector args, rv;
        args.reserve(m_args.size() + m_input_offsets.size());
        rv.reserve(m_output_offsets.size());

        for (uint32_t index : m_args)
            args.push_back_borrow(index);
        for (size_t i = 0; i < m_input_offsets.size(); ++i)
            args.push_back_steal(ad_grad(combine(m_input_indices[i])));

        ad_call(m_backend, m_domain, m_callable_count, name.c_str(), false, m_index,
                 m_mask, args, rv, this, &forward_cb, nullptr, false);

        ad_assert(rv.size() == m_output_offsets.size(), "Size mismatch!");

        m_args2.release();
        m_rv2.clear();
        m_temp.release();

        for (size_t i = 0; i < m_output_offsets.size(); ++i)
            ad_accum_grad(combine(m_output_indices[i]), (uint32_t) rv[i]);
    }

    /// Implements f(arg..., grad(rv)...) -> grad(arg) ...
    void backward() override {
        scoped_isolation_boundary isolation_guard;
        std::string name = m_name + " [ad, bwd]";

        dr_index64_vector args, rv;
        args.reserve(m_args.size() + m_output_offsets.size());
        rv.reserve(m_input_offsets.size());

        for (uint32_t index : m_args)
            args.push_back_borrow(index);
        for (size_t i = 0; i < m_output_offsets.size(); ++i)
            args.push_back_steal(ad_grad(combine(m_output_indices[i])));

        ad_call(m_backend, m_domain, m_callable_count, name.c_str(), false,
                 m_index, m_mask, args, rv, this, &backward_cb, nullptr, false);

        ad_assert(rv.size() == m_input_offsets.size(), "Size mismatch!");

        m_args2.release();
        m_rv2.clear();
        m_temp.release();

        for (size_t i = 0; i < m_input_offsets.size(); ++i)
            ad_accum_grad(combine(m_input_indices[i]), (uint32_t) rv[i]);
    }

    static void forward_cb(void *ptr, void *self,
                           const dr_vector<uint64_t> &args,
                           dr_vector<uint64_t> &rv) {
        ((CallOp *) ptr)->forward_cb(self, args, rv);
    }

    /// Forward AD callback (invoked by forward() once per callable)
    void forward_cb(void *self, const dr_vector<uint64_t> &args,
                    dr_vector<uint64_t> &rv) {
        m_args2.release();
        for (size_t i = 0; i < m_args.size(); ++i)
            m_args2.push_back_borrow(args[i]);

        for (size_t i = 0; i < m_input_offsets.size(); ++i) {
            uint64_t &index     = m_args2[m_input_offsets[i]],
                      index_new = ad_var_new((uint32_t) index);
            ad_var_dec_ref(index);
            index = index_new;
        }

        m_rv2.clear();
        m_func(m_payload, self, m_args2, m_rv2);

        for (size_t i = 0; i < m_input_offsets.size(); ++i) {
            uint64_t index = m_args2[m_input_offsets[i]];
            ad_accum_grad(index, (uint32_t) args[m_args.size() + i]);
            ad_enqueue(dr::ADMode::Forward, index);
        }

        for (size_t i = m_input_offsets.size(); i < m_input_indices.size(); ++i)
            ad_enqueue(dr::ADMode::Forward, ((uint64_t) m_input_indices[i]) << 32);

        // Enqueue implicit dependencies
        ad_traverse(dr::ADMode::Forward, (uint32_t) dr::ADFlag::ClearNone);

        m_temp.release();
        for (size_t i = 0; i < m_output_offsets.size(); ++i) {
            uint32_t index = ad_grad(m_rv2[m_output_offsets[i]]);
            m_temp.push_back_steal(index);
            rv.push_back(index);
        }
    }

    static void backward_cb(void *ptr, void *self,
                           const dr_vector<uint64_t> &args,
                           dr_vector<uint64_t> &rv) {
        ((CallOp *) ptr)->backward_cb(self, args, rv);
    }

    /// Backward AD callback (invoked by backward() once per callable)
    void backward_cb(void *self, const dr_vector<uint64_t> &args,
                     dr_vector<uint64_t> &rv) {
        m_args2.release();
        for (size_t i = 0; i < m_args.size(); ++i)
            m_args2.push_back_borrow(args[i]);

        for (size_t i = 0; i < m_input_offsets.size(); ++i) {
            uint64_t &index     = m_args2[m_input_offsets[i]],
                      index_new = ad_var_new((uint32_t) index);
            ad_var_dec_ref(index);
            index = index_new;
        }

        m_rv2.clear();
        m_func(m_payload, self, m_args2, m_rv2);

        for (size_t i = 0; i < m_output_offsets.size(); ++i) {
            uint64_t index     = m_rv2[m_output_offsets[i]],
                     index_new = ad_var_copy(index);
            ad_accum_grad(index_new, (uint32_t) args[m_args.size() + i]);
            ad_enqueue(dr::ADMode::Backward, index_new);
            ad_var_dec_ref(index_new);
        }

        ad_traverse(dr::ADMode::Backward, (uint32_t) dr::ADFlag::ClearNone);

        m_temp.release();
        for (size_t i = 0; i < m_input_offsets.size(); ++i) {
            uint32_t index = ad_grad(m_args2[m_input_offsets[i]]);
            m_temp.push_back_steal(index);
            rv.push_back(index);
        }
    }


    const char *name() const override { return m_name_op.c_str(); }

    void add_input(size_t i, uint64_t index) {
        if (add_index(m_backend, index >> 32, true))
            m_input_offsets.push_back((uint32_t) i);
    }

    void add_output(size_t i, uint64_t index) {
        if (add_index(m_backend, index >> 32, false)) {
            m_output_offsets.push_back((uint32_t) i);
            m_rv.push_back_borrow((index >> 32) << 32);
        }
    }

    void disable_deleter() { m_cleanup = nullptr; }

private:
    std::string m_name, m_name_op;
    const char *m_domain;
    uint32_t m_index, m_mask;
    size_t m_callable_count;
    dr_index32_vector m_args;
    dr_index64_vector m_args2;
    dr_index64_vector m_rv;
    dr_vector<uint64_t> m_rv2;
    dr_index32_vector m_temp;
    dr_vector<uint32_t> m_input_offsets;
    dr_vector<uint32_t> m_output_offsets;
    void *m_payload;
    ad_call_func m_func;
    ad_call_cleanup m_cleanup;
};

// Generic checks, then forward either to ad_call_record or ad_call_reduce
bool ad_call(JitBackend backend, const char *domain, size_t callable_count,
             const char *name, bool is_getter, uint32_t index, uint32_t mask,
             const dr_vector<uint64_t> &args, dr_vector<uint64_t> &rv,
             void *payload, ad_call_func func, ad_call_cleanup cleanup,
             bool ad) {
    try {
        const char *domain_or_empty = domain ? domain : "",
                   *separator = domain ? "::" : "";

        if ((callable_count != 0) == (domain != nullptr))
            jit_raise("ad_call(\"%s%s%s\"): please specify either the "
                      "'domain' parameter *or* 'callable_count', but not both",
                      domain_or_empty, separator, name);

        if (domain)
            callable_count = jit_registry_id_bound(backend, domain);

        size_t size = jit_var_size(index);
        if (mask) {
            size_t size_2 = jit_var_size(mask);

            if (size == 1)
                size = size_2;
            else if (size != size_2 && size_2 != 1)
                jit_raise("ad_call(\"%s%s%s\"): mismatched argument sizes "
                          "(%zu and %zu)",
                          domain_or_empty, separator, name, size, size_2);
        }

        bool needs_ad = false;
        for (uint64_t arg_i : args) {
            size_t size_2 = jit_var_size((uint32_t) arg_i);

            if (size == 1)
                size = size_2;
            else if (size != size_2 && size_2 != 1)
                jit_raise("ad_call(\"%s%s%s\"): mismatched argument sizes "
                          "(%zu and %zu)",
                          domain_or_empty, separator, name, size, size_2);

            needs_ad |= arg_i >> 32;
        }

        if (index == 0 || size == 0 || jit_var_is_zero_literal(mask) ||
            callable_count == 0) {
            scoped_set_mask mask_guard(backend, jit_var_bool(backend, false));
            func(payload, nullptr, args, rv);

            for (uint64_t &i : rv) {
                uint64_t zero = 0;
                if (i)
                    i = jit_var_literal(backend, jit_var_type(i), &zero, size);
            }

            return true;
        }

        dr_vector<bool> rv_ad;
        dr_vector<uint32_t> implicit_in;

        if (is_getter) {
            scoped_isolation_boundary guard;
            ad_call_getter(backend, domain, name, size, index, mask,
                            callable_count, args, rv, rv_ad, func, payload);
            ad_copy_implicit_deps(implicit_in);
            guard.success = true;
        } else if (jit_flag(JitFlag::VCallRecord)) {
            scoped_isolation_boundary guard;
            ad_call_record(backend, domain, name, size, index, mask,
                            callable_count, args, rv, rv_ad, func, payload);
            ad_copy_implicit_deps(implicit_in);
            guard.success = true;
        } else {
            if (jit_flag(JitFlag::Symbolic))
                jit_raise(
                    "Dr.Jit is currently recording symbolic computation and cannot perform an\n"
                    "array-based function call in *evaluated mode*. You will likely want to set\n"
                    "the Jit flag drjit.JitFlag.SymbolicCalls to True. Please review the Dr.Jit\n"
                    "documentation of drjit.JitFlag.SymbolicCalls and drjit.switch() for general\n"
                    "information on symbolic and evaluated calls, as well as their limitations.");

            ad_call_reduce(backend, domain, name, size, index, mask,
                            callable_count, args, rv, func, payload);
            ad = false; // derivative already tracked, no CustomOp needed
        }

        for (bool b : rv_ad)
            needs_ad |= b;

        if (ad && needs_ad) {
            std::string combined(name);

            if (domain) {
                callable_count = 0;
                combined = std::string(domain) + "::" + combined;
            }

            nanobind::ref<CallOp> op = new CallOp(
                backend, std::move(combined), domain, index, mask, callable_count,
                args, rv.size(), payload, func, cleanup);

            for (size_t i = 0; i < args.size(); ++i)
                op->add_input(i, args[i]);

            for (uint32_t index: implicit_in)
                op->add_index(backend, index, true);

            for (size_t i = 0; i < rv.size(); ++i) {
                if (!rv_ad[i])
                    continue;

                uint64_t index = ad_var_new((uint32_t) rv[i]);

                jit_var_dec_ref((uint32_t) rv[i]);
                rv[i] = index;
                op->add_output(i, index);
            }

            if (ad_custom_op(op.get())) {
                // CallOp will eventually call cleanup()
                return false;
            }

            // CustomOp was not needed, detach output again..
            op->disable_deleter();
            for (size_t i = 0; i < rv.size(); ++i) {
                uint64_t index = rv[i],
                         index_d = (index << 32) >> 32;
                if (index == index_d)
                    continue;
                jit_var_inc_ref((uint32_t) index_d);
                ad_var_dec_ref(index);
                rv[i] = index_d;
            }
        }

        // Caller should directly call cleanup()
        return true;
    } catch (...) {
        if (cleanup)
            cleanup(payload);
        throw;
    }
}

