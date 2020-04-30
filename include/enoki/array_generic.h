/*
    enoki/array_generic.h -- Generic array implementation that forwards
    all operations to the underlying data type (usually without making use of
    hardware vectorization)

    Copyright (c) 2020 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a BSD-style
    license that can be found in the LICENSE file.
*/

#pragma once

#include <enoki/array_static.h>

NAMESPACE_BEGIN(enoki)

template <typename Value_, size_t Size_, bool IsMask_, typename Derived_, typename = int>
struct StaticArrayImpl;

namespace detail {
    template <typename Type, size_t Size, typename = int> struct vectorize {
        using Parent = vectorize<Type, detail::lpow2(Size)>;
        static constexpr bool recurse = Parent::recurse || Parent::self;
        static constexpr bool self = false;
    };

    template <typename Type> struct vectorize<Type, 1> {
        static constexpr bool recurse = false;
        static constexpr bool self = false;
    };

    template <typename Type> struct vectorize<Type, 0> {
        static constexpr bool recurse = false;
        static constexpr bool self = false;
    };

    /// Decide whether an array can be handled using a packet implementation
    template <typename Type>
    constexpr bool vectorizable_type_v = std::is_same_v<Type, float> ||
                                         std::is_same_v<Type, double> ||
                                         (std::is_integral_v<Type> &&
                                          (sizeof(Type) == 4 || sizeof(Type) == 8));

    template <typename Type, size_t Size>
    using vectorize_t = vectorize<Type, Size * sizeof(Type)>;

    template <typename Type, size_t Size>
    using enable_if_generic =
        enable_if_t<Size != 0 &&
                    !(vectorizable_type_v<Type> &&
                      (vectorize_t<Type, Size>::self ||
                       (Size >= 4 && vectorize_t<Type, Size>::recurse)))>;

    template <typename Type, size_t Size>
    using enable_if_recursive =
        enable_if_t<vectorizable_type_v<Type> && (Size >= 4) &&
                    vectorize_t<Type, Size>::recurse>;
};

/**
 * Generic fallback array type. Requires that 'Value_' is default, copy-, and
 * move-constructible, as well as assignable.
 */
template <typename Value_, size_t Size_, bool IsMask_, typename Derived_>
struct StaticArrayImpl<Value_, Size_, IsMask_, Derived_,
                       detail::enable_if_generic<Value_, Size_>>
    : StaticArrayBase<std::conditional_t<IsMask_, mask_t<Value_>, Value_>,
                      Size_, IsMask_, Derived_> {

    using Base = StaticArrayBase<
        std::conditional_t<IsMask_, mask_t<Value_>, Value_>,
        Size_, IsMask_, Derived_>;

    using typename Base::Derived;
    using typename Base::Value;
    using typename Base::Scalar;
    using typename Base::Array1;
    using typename Base::Array2;

    using Base::Size;
    using Base::derived;
    using Base::coeff;

    ENOKI_ARRAY_DEFAULTS(StaticArrayImpl)
    ENOKI_ARRAY_FALLBACK_CONSTRUCTORS(StaticArrayImpl)


#if defined(NDEBUG)
    StaticArrayImpl() = default;
#else
    template <typename T = Value_, enable_if_t<!std::is_scalar_v<T>> = 0>
    StaticArrayImpl() { }
    template <typename T = Value_, enable_if_t<std::is_scalar_v<T>> = 0>
    StaticArrayImpl() : StaticArrayImpl(DebugInitialization<Scalar>) { }
#endif

    StaticArrayImpl(const Value &v) {
        ENOKI_CHKSCALAR("Constructor (scalar broadcast)");
        for (size_t i = 0; i < Size_; ++i)
            m_data[i] = v;
    }

    /// Construct from component values
    template <typename... Ts, detail::enable_if_components_t<Size_, Ts...> = 0>
    ENOKI_INLINE StaticArrayImpl(Ts&&... ts) : m_data{ move_cast_t<Ts, Value>(ts)... } {
        ENOKI_CHKSCALAR("Constructor (component values)");
    }

    /// Construct from sub-arrays
    template <typename T1, typename T2, typename T = StaticArrayImpl, enable_if_t<
              array_depth_v<T1> == array_depth_v<T> && array_size_v<T1> == Base::Size1 &&
              array_depth_v<T2> == array_depth_v<T> && array_size_v<T2> == Base::Size2 &&
              Base::Size2 != 0> = 0>
    StaticArrayImpl(const T1 &a1, const T2 &a2)
        : StaticArrayImpl(a1, a2, std::make_index_sequence<Base::Size1>(),
                                  std::make_index_sequence<Base::Size2>()) { }

    /// Access elements by reference, and without error-checking
    ENOKI_INLINE Value &coeff(size_t i) { return m_data[i]; }

    /// Access elements by reference, and without error-checking (const)
    ENOKI_INLINE const Value &coeff(size_t i) const { return m_data[i]; }

    /// Pointer to the underlying storage
    Value *data() { return m_data; }

    /// Pointer to the underlying storage (const)
    const Value *data() const { return m_data; }

private:
    Value m_data[Size_];
};

/// Special case for zero-sized arrays
template <typename Value_, bool IsMask_, typename Derived_>
struct StaticArrayImpl<Value_, 0, IsMask_, Derived_>
    : StaticArrayBase<std::conditional_t<IsMask_, mask_t<Value_>, Value_>, 0,
                      IsMask_, Derived_> {
    using Base =
        StaticArrayBase<std::conditional_t<IsMask_, mask_t<Value_>, Value_>, 0,
                        IsMask_, Derived_>;

    using typename Base::Value;
    using Base::coeff;

    Value &coeff(size_t i) { return *data(); }
    const Value &coeff(size_t i) const { return *data(); }

    /// Pointer to the underlying storage (returns \c nullptr)
    Value *data() { return nullptr; }

    /// Pointer to the underlying storage (returns \c nullptr, const)
    const Value *data() const { return nullptr; }

    ENOKI_ARRAY_DEFAULTS(StaticArrayImpl)

    StaticArrayImpl() = default;
    template <typename Value2, typename Derived2>
    StaticArrayImpl(const ArrayBaseT<Value2, IsMask_, Derived2> &) { }
    template <typename Value2, typename Derived2>
    StaticArrayImpl(const ArrayBaseT<Value2, IsMask_, Derived2> &, detail::reinterpret_flag) { }
    StaticArrayImpl(const Value &) { }
};

namespace detail {
    template <typename T> void put_shape(size_t *shape) {
        size_t size = array_size_v<T>;
        *shape = size == Dynamic ? 0 : size;
        if constexpr (is_array_v<value_t<T>>)
            put_shape<value_t<T>>(shape + 1);
    }

    /// Write the shape of an array to 'shape'
    template <typename T> void put_shape(const T &array, size_t *shape) {
        ENOKI_MARK_USED(shape); ENOKI_MARK_USED(array);

        if constexpr (is_array_v<T>) {
            size_t size = array.derived().size();
            *shape = size;
            if constexpr (is_array_v<value_t<T>>) {
                if (size == 0)
                    put_shape<value_t<T>>(shape + 1);
                else
                    put_shape(array.derived().coeff(0), shape + 1);
            }
        }
    }

    template <typename T>
    bool is_ragged(const T &array, const size_t *shape) {
        ENOKI_MARK_USED(shape);
        if constexpr (is_array_v<T>) {
            size_t size = array.derived().size();
            if (*shape != size)
                return true;

            if constexpr (is_dynamic_v<T>) {
                bool match = false;
                for (size_t i = 0; i < size; ++i)
                    match |= is_ragged(array.derived().coeff(i), shape + 1);
                return match;
            }

        }
        return false;
    }

    template <bool Abbrev = false, typename Stream, typename Array, typename... Indices>
    void print(Stream &os, const Array &a, const size_t *shape, Indices... indices) {
        ENOKI_MARK_USED(shape);
        if constexpr (sizeof...(Indices) == array_depth_v<Array>) {
            os << a.derived().coeff(indices...);
        } else {
            constexpr size_t k = array_depth_v<Array> - sizeof...(Indices) - 1;
            os << "[";
            for (size_t i = 0; i < shape[k]; ++i) {
                if constexpr (is_dynamic_array_v<Array>) {
                    if (Abbrev && shape[k] > 20 && i == 5) {
                        if (k > 0) {
                            os << ".. " << shape[k] - 10 << " skipped ..,\n";
                            for (size_t j = 0; j <= sizeof...(Indices); ++j)
                                os << " ";
                        } else {
                            os << ".. " << shape[k] - 10 << " skipped .., ";
                        }
                        i = shape[k] - 6;
                        continue;
                    }
                }
                print<false>(os, a, shape, i, indices...);
                if (i + 1 < shape[k]) {
                    if constexpr (k == 0) {
                        os << ", ";
                    } else {
                        os << ",\n";
                        for (size_t j = 0; j <= sizeof...(Indices); ++j)
                            os << " ";
                    }
                }
            }
            os << "]";
        }
    }
}

template <typename Array> bool ragged(const Array &a) {
    size_t shape[array_depth_v<Array> + 1];
    detail::put_shape(a, shape);
    return detail::is_ragged(a, shape);
}

template <typename Stream, typename Value, bool IsMask, typename Derived>
ENOKI_NOINLINE Stream &operator<<(Stream &os, const ArrayBaseT<Value, IsMask, Derived> &a) {
    size_t shape[array_depth_v<Derived> + 1];
    schedule(a);
    detail::put_shape(a, shape);

    if (detail::is_ragged(a, shape))
        os << "[ragged array]";
    else
        detail::print<true>(os, a, shape);

    return os;
}

NAMESPACE_END(enoki)
