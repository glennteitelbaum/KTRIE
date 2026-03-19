// py_kntrie.cpp — pybind11 wrapper for gteitelbaum::kntrie
//
// Five value types: Int64, Int32, Float, Bool, Object
// Key type: int64_t for all
//
// Build: g++ -std=c++23 -O2 -shared -fPIC
//        $(python3 -m pybind11 --includes) py_kntrie.cpp
//        -o kntrie$(python3-config --extension-suffix)

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "kntrie.hpp"

namespace py = pybind11;

using namespace gteitelbaum;

// ============================================================================
// bind_kntrie<V> — register one kntrie<int64_t, V> class
// ============================================================================

template<typename V, typename PyV>
void bind_kntrie(py::module_& m, const char* name) {
    using trie_t = kntrie<int64_t, V>;
    using iter_t = typename trie_t::const_iterator;

    py::class_<trie_t>(m, name)

        // Construction
        .def(py::init<>())

        // __getitem__
        .def("__getitem__", [](const trie_t& t, int64_t key) -> PyV {
            auto it = t.find(key);
            if (it == t.end())
                throw py::key_error(std::to_string(key));
            if constexpr (std::is_same_v<V, py::object>)
                return it.value();
            else
                return static_cast<PyV>(it.value());
        })

        // __setitem__
        .def("__setitem__", [](trie_t& t, int64_t key, PyV val) {
            if constexpr (std::is_same_v<V, py::object>)
                t.insert_or_assign(key, val);
            else
                t.insert_or_assign(key, static_cast<V>(val));
        })

        // __delitem__
        .def("__delitem__", [](trie_t& t, int64_t key) {
            if (t.erase(key) == 0)
                throw py::key_error(std::to_string(key));
        })

        // __contains__
        .def("__contains__", [](const trie_t& t, int64_t key) {
            return t.contains(key);
        })

        // __len__
        .def("__len__", [](const trie_t& t) { return t.size(); })

        // __bool__
        .def("__bool__", [](const trie_t& t) { return !t.empty(); })

        // __iter__ — yields (key, value) tuples
        .def("__iter__", [](const trie_t& t) {
            return py::make_iterator(t.begin(), t.end());
        }, py::keep_alive<0, 1>())

        // get(key, default=None)
        .def("get", [](const trie_t& t, int64_t key, py::object dflt) -> py::object {
            auto it = t.find(key);
            if (it == t.end()) return dflt;
            if constexpr (std::is_same_v<V, py::object>)
                return it.value();
            else
                return py::cast(static_cast<PyV>(it.value()));
        }, py::arg("key"), py::arg("default") = py::none())

        // insert(key, value) → bool
        .def("insert", [](trie_t& t, int64_t key, PyV val) -> bool {
            if constexpr (std::is_same_v<V, py::object>)
                return t.insert({key, val}).second;
            else
                return t.insert({key, static_cast<V>(val)}).second;
        })

        // modify(key, fn) → bool
        // modify(key, fn, default) → bool
        .def("modify", [](trie_t& t, int64_t key, py::function fn,
                          py::object dflt) -> bool {
            if (dflt.is_none()) {
                // Two-arg: modify existing only
                return t.modify(key, [&fn](V& v) {
                    if constexpr (std::is_same_v<V, py::object>)
                        v = fn(v);
                    else
                        v = fn(py::cast(v)).template cast<V>();
                });
            } else {
                // Three-arg: modify or insert default
                V def_val;
                if constexpr (std::is_same_v<V, py::object>)
                    def_val = dflt;
                else
                    def_val = dflt.cast<V>();
                return t.modify(key, [&fn](V& v) {
                    if constexpr (std::is_same_v<V, py::object>)
                        v = fn(v);
                    else
                        v = fn(py::cast(v)).template cast<V>();
                }, def_val);
            }
        }, py::arg("key"), py::arg("fn"), py::arg("default") = py::none())

        // erase_when(key, fn) → bool
        .def("erase_when", [](trie_t& t, int64_t key, py::function fn) -> bool {
            return t.erase_when(key, [&fn](const V& v) -> bool {
                if constexpr (std::is_same_v<V, py::object>)
                    return fn(v).template cast<bool>();
                else
                    return fn(py::cast(v)).template cast<bool>();
            });
        })

        // clear()
        .def("clear", &trie_t::clear)

        // memory_usage()
        .def("memory_usage", &trie_t::memory_usage)

        // items() — yields (key, value) tuples
        .def("items", [](const trie_t& t) {
            return py::make_iterator(t.begin(), t.end());
        }, py::keep_alive<0, 1>())

        // keys()
        .def("keys", [](const trie_t& t) {
            py::list result;
            for (auto it = t.begin(); it != t.end(); ++it)
                result.append(it.key());
            return result;
        })

        // values()
        .def("values", [](const trie_t& t) {
            py::list result;
            for (auto it = t.begin(); it != t.end(); ++it) {
                if constexpr (std::is_same_v<V, py::object>)
                    result.append(it.value());
                else
                    result.append(py::cast(static_cast<PyV>(it.value())));
            }
            return result;
        })

        // __repr__
        .def("__repr__", [name](const trie_t& t) {
            return std::string("kntrie.") + name +
                   "(size=" + std::to_string(t.size()) + ")";
        });
}

// ============================================================================
// Module definition
// ============================================================================

PYBIND11_MODULE(kntrie, m) {
    m.doc() = "kntrie — ordered associative container with int64 keys";

    bind_kntrie<int64_t,    int64_t>   (m, "Int64");
    bind_kntrie<int32_t,    int32_t>   (m, "Int32");
    bind_kntrie<double,     double>    (m, "Float");
    bind_kntrie<bool,       bool>      (m, "Bool");
    bind_kntrie<py::object, py::object>(m, "Object");
}
