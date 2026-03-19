// py_kstrie.cpp — pybind11 wrapper for gteitelbaum::kstrie
//
// Five value types: Int64, Int32, Float, Bool, Object
// Key type: str for all
//
// Build: g++ -std=c++23 -O2 -shared -fPIC
//        $(python3 -m pybind11 --includes) py_kstrie.cpp
//        -o kstrie$(python3-config --extension-suffix)

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "kstrie.hpp"

namespace py = pybind11;

using namespace gteitelbaum;

// ============================================================================
// bind_kstrie<V> — register one kstrie<V> class
// ============================================================================

template<typename V, typename PyV>
void bind_kstrie(py::module_& m, const char* name) {
    using trie_t = kstrie<V>;
    using iter_t = typename trie_t::const_iterator;

    auto cls = py::class_<trie_t>(m, name)

        // Construction
        .def(py::init<>())

        // __getitem__
        .def("__getitem__", [](const trie_t& t, std::string_view key) -> PyV {
            auto it = t.find(key);
            if (it == t.end())
                throw py::key_error(std::string(key));
            if constexpr (std::is_same_v<V, py::object>)
                return it.value();
            else
                return static_cast<PyV>(it.value());
        })

        // __setitem__
        .def("__setitem__", [](trie_t& t, std::string_view key, PyV val) {
            if constexpr (std::is_same_v<V, py::object>)
                t.insert_or_assign(key, val);
            else
                t.insert_or_assign(key, static_cast<V>(val));
        })

        // __delitem__
        .def("__delitem__", [](trie_t& t, std::string_view key) {
            if (t.erase(key) == 0)
                throw py::key_error(std::string(key));
        })

        // __contains__
        .def("__contains__", [](const trie_t& t, std::string_view key) {
            return t.find(key) != t.end();
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
        .def("get", [](const trie_t& t, std::string_view key,
                       py::object dflt) -> py::object {
            auto it = t.find(key);
            if (it == t.end()) return dflt;
            if constexpr (std::is_same_v<V, py::object>)
                return it.value();
            else
                return py::cast(static_cast<PyV>(it.value()));
        }, py::arg("key"), py::arg("default") = py::none())

        // insert(key, value) → bool
        .def("insert", [](trie_t& t, std::string_view key, PyV val) -> bool {
            if constexpr (std::is_same_v<V, py::object>)
                return t.insert(key, val);
            else
                return t.insert(key, static_cast<V>(val));
        })

        // modify(key, fn) → bool
        // modify(key, fn, default) → bool
        .def("modify", [](trie_t& t, std::string_view key, py::function fn,
                          py::object dflt) -> bool {
            if (dflt.is_none()) {
                return t.modify(key, [&fn](V& v) {
                    if constexpr (std::is_same_v<V, py::object>)
                        v = fn(v);
                    else
                        v = fn(py::cast(v)).template cast<V>();
                });
            } else {
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
        .def("erase_when", [](trie_t& t, std::string_view key,
                              py::function fn) -> bool {
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
                result.append(py::cast(it.key()));
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

        // ----- Prefix operations -----

        // prefix_count(pfx) → int
        .def("prefix_count", [](const trie_t& t, std::string_view pfx) {
            return t.prefix_count(pfx);
        })

        // prefix_items(pfx) → list[(str, value)]
        .def("prefix_items", [](const trie_t& t, std::string_view pfx) {
            py::list result;
            t.prefix_walk(pfx, [&](std::string_view key, const V& val) {
                if constexpr (std::is_same_v<V, py::object>)
                    result.append(py::make_tuple(
                        py::str(key.data(), key.size()), val));
                else
                    result.append(py::make_tuple(
                        py::str(key.data(), key.size()),
                        py::cast(static_cast<PyV>(val))));
            });
            return result;
        })

        // prefix_keys(pfx) → list[str]
        .def("prefix_keys", [](const trie_t& t, std::string_view pfx) {
            py::list result;
            t.prefix_walk(pfx, [&](std::string_view key, const V&) {
                result.append(py::str(key.data(), key.size()));
            });
            return result;
        })

        // prefix_values(pfx) → list[value]
        .def("prefix_values", [](const trie_t& t, std::string_view pfx) {
            py::list result;
            t.prefix_walk(pfx, [&](std::string_view, const V& val) {
                if constexpr (std::is_same_v<V, py::object>)
                    result.append(val);
                else
                    result.append(py::cast(static_cast<PyV>(val)));
            });
            return result;
        })

        // prefix_copy(pfx) → new kstrie
        .def("prefix_copy", [](const trie_t& t, std::string_view pfx) {
            return t.prefix_copy(pfx);
        })

        // prefix_erase(pfx) → int
        .def("prefix_erase", [](trie_t& t, std::string_view pfx) {
            return t.prefix_erase(pfx);
        })

        // prefix_split(pfx) → new kstrie
        .def("prefix_split", [](trie_t& t, std::string_view pfx) {
            return t.prefix_split(pfx);
        })

        // __repr__
        .def("__repr__", [name](const trie_t& t) {
            return std::string("kstrie.") + name +
                   "(size=" + std::to_string(t.size()) + ")";
        });
}

// ============================================================================
// Module definition
// ============================================================================

PYBIND11_MODULE(kstrie, m) {
    m.doc() = "kstrie — ordered associative container with string keys and prefix operations";

    bind_kstrie<int64_t,    int64_t>   (m, "Int64");
    bind_kstrie<int32_t,    int32_t>   (m, "Int32");
    bind_kstrie<double,     double>    (m, "Float");
    bind_kstrie<bool,       bool>      (m, "Bool");
    bind_kstrie<py::object, py::object>(m, "Object");
}
