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

    // Helper iterator class for Python __iter__ protocol.
    // lazy_key is not a Python type, so we can't use py::make_iterator
    // directly — each __next__ materializes the key as std::string.
    struct py_iter {
        iter_t it;
        iter_t end_it;
        py_iter(iter_t&& i, iter_t&& e)
            : it(std::move(i)), end_it(std::move(e)) {}
    };

    std::string iter_name = std::string("_Iter_") + name;
    py::class_<py_iter>(m, iter_name.c_str())
        .def("__iter__", [](py_iter& self) -> py_iter& { return self; })
        .def("__next__", [](py_iter& self) -> py::tuple {
            if (self.it == self.end_it)
                throw py::stop_iteration();
            std::string key = (*self.it).first;
            const V& val = (*self.it).second;
            py::tuple result;
            if constexpr (std::is_same_v<V, py::object>)
                result = py::make_tuple(key, val);
            else
                result = py::make_tuple(key, static_cast<PyV>(val));
            ++self.it;
            return result;
        });

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

        // __iter__ — yields (key, value) tuples in sorted order
        .def("__iter__", [](const trie_t& t) {
            return py_iter(t.begin(), iter_t(t.end()));
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

        // insert(key, value) → bool (true if inserted, false if already exists)
        .def("insert", [](trie_t& t, std::string_view key, PyV val) -> bool {
            if constexpr (std::is_same_v<V, py::object>)
                return t.insert(key, val).second;
            else
                return t.insert(key, static_cast<V>(val)).second;
        })

        // clear()
        .def("clear", &trie_t::clear)

        // memory_usage()
        .def("memory_usage", &trie_t::memory_usage)

        // items() — yields (key, value) tuples in sorted order
        .def("items", [](const trie_t& t) {
            return py_iter(t.begin(), iter_t(t.end()));
        }, py::keep_alive<0, 1>())

        // keys() → list[str]
        .def("keys", [](const trie_t& t) {
            py::list result;
            for (auto it = t.begin(); it != t.end(); ++it)
                result.append(py::cast(std::string((*it).first)));
            return result;
        })

        // values() → list[value]
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
