// py_ktoken.cpp — pybind11 wrapper for ktoken::tokenizer
//
// Six classes in two submodules: tiktoken and huggingface
// Each wraps one tokenizer<Model, Format> directly.
//
// Build: g++ -std=c++23 -O2 -shared -fPIC -I../kntrie -I../kstrie
//        $(python3 -m pybind11 --includes) py_ktoken.cpp
//        -o ktoken$(python3-config --extension-suffix)

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "nlohmann_json.hpp"
#include "ktoken.hpp"

namespace py = pybind11;

// ============================================================================
// bind_tokenizer<T> — register one tokenizer class
// ============================================================================

template<typename T>
void bind_tokenizer(py::module_& m, const char* name) {

    py::class_<T>(m, name)

        // Construction: (unicode_db_path, vocab_path)
        .def(py::init<const char*, const char*>(),
             py::arg("unicode_db_path"), py::arg("vocab_path"))

        // encode(text) → list[int]
        // Accepts str or bytes. Releases GIL during C++ work.
        .def("encode", [](const T& tok, py::object input) {
            std::string buf;
            const uint8_t* data;
            size_t len;

            if (py::isinstance<py::str>(input)) {
                buf = input.cast<std::string>();
                data = reinterpret_cast<const uint8_t*>(buf.data());
                len = buf.size();
            } else if (py::isinstance<py::bytes>(input)) {
                char* ptr; Py_ssize_t sz;
                PyBytes_AsStringAndSize(input.ptr(), &ptr, &sz);
                data = reinterpret_cast<const uint8_t*>(ptr);
                len = static_cast<size_t>(sz);
            } else {
                throw py::type_error("encode() requires str or bytes");
            }

            std::vector<uint32_t> tokens;
            {
                py::gil_scoped_release release;
                tokens = tok.encode(data, len);
            }

            py::list result(tokens.size());
            for (size_t i = 0; i < tokens.size(); ++i)
                result[i] = py::cast(static_cast<int>(tokens[i]));
            return result;
        }, py::arg("text"))

        // encode_batch(texts) → list[list[int]]
        .def("encode_batch", [](const T& tok, py::list texts) {
            py::list result;
            for (auto& item : texts) {
                std::string s;
                const uint8_t* data;
                size_t len;

                if (py::isinstance<py::str>(item)) {
                    s = item.cast<std::string>();
                    data = reinterpret_cast<const uint8_t*>(s.data());
                    len = s.size();
                } else if (py::isinstance<py::bytes>(item)) {
                    char* ptr; Py_ssize_t sz;
                    PyBytes_AsStringAndSize(item.ptr(), &ptr, &sz);
                    data = reinterpret_cast<const uint8_t*>(ptr);
                    len = static_cast<size_t>(sz);
                } else {
                    throw py::type_error(
                        "encode_batch() items must be str or bytes");
                }

                std::vector<uint32_t> tokens;
                {
                    py::gil_scoped_release release;
                    tokens = tok.encode(data, len);
                }

                py::list toks(tokens.size());
                for (size_t i = 0; i < tokens.size(); ++i)
                    toks[i] = py::cast(static_cast<int>(tokens[i]));
                result.append(toks);
            }
            return result;
        }, py::arg("texts"))

        // decode(tokens) → bytes
        .def("decode", [](const T& tok, py::list tokens) {
            std::vector<uint32_t> ids;
            ids.reserve(tokens.size());
            for (auto& t : tokens)
                ids.push_back(t.cast<uint32_t>());

            std::vector<uint8_t> bytes;
            {
                py::gil_scoped_release release;
                bytes = tok.decode(ids.data(), ids.size());
            }

            return py::bytes(
                reinterpret_cast<const char*>(bytes.data()),
                bytes.size());
        }, py::arg("tokens"))

        // decode_str(tokens) → str (UTF-8)
        .def("decode_str", [](const T& tok, py::list tokens) {
            std::vector<uint32_t> ids;
            ids.reserve(tokens.size());
            for (auto& t : tokens)
                ids.push_back(t.cast<uint32_t>());

            std::vector<uint8_t> bytes;
            {
                py::gil_scoped_release release;
                bytes = tok.decode(ids.data(), ids.size());
            }

            // Decode as UTF-8, raise on invalid
            return py::str(
                reinterpret_cast<const char*>(bytes.data()),
                bytes.size());
        }, py::arg("tokens"))

        // vocab_size property
        .def_property_readonly("vocab_size", [](const T& tok) {
            return tok.vocab_size();
        })

        // train(unicode_db_path, corpus, vocab_size) → tokenizer
        .def_static("train", [](const char* unicode_db_path,
                                py::bytes corpus,
                                uint32_t vocab_size) {
            char* ptr; Py_ssize_t sz;
            PyBytes_AsStringAndSize(corpus.ptr(), &ptr, &sz);
            const uint8_t* data = reinterpret_cast<const uint8_t*>(ptr);

            T result;
            {
                py::gil_scoped_release release;
                result = T::train(unicode_db_path, data,
                                  static_cast<size_t>(sz), vocab_size);
            }
            return result;
        }, py::arg("unicode_db_path"), py::arg("corpus"),
           py::arg("vocab_size"))

        // __repr__
        .def("__repr__", [name](const T& tok) {
            return std::string("ktoken.") + name +
                   "(vocab_size=" +
                   std::to_string(tok.vocab_size()) + ")";
        });
}

// ============================================================================
// Module definition
// ============================================================================

PYBIND11_MODULE(ktoken, m) {
    m.doc() = "ktoken — compact BPE tokenizer";

    auto tiktoken = m.def_submodule("tiktoken",
        "Tokenizers loading tiktoken format files");
    auto huggingface = m.def_submodule("huggingface",
        "Tokenizers loading HuggingFace tokenizer.json files");

    using namespace ktoken;

    bind_tokenizer<tokenizer<cl100k, tiktoken_format>>(tiktoken, "Cl100k");
    bind_tokenizer<tokenizer<p50k,   tiktoken_format>>(tiktoken, "P50k");
    bind_tokenizer<tokenizer<o200k,  tiktoken_format>>(tiktoken, "O200k");

    bind_tokenizer<tokenizer<cl100k, huggingface_format>>(huggingface, "Cl100k");
    bind_tokenizer<tokenizer<p50k,   huggingface_format>>(huggingface, "P50k");
    bind_tokenizer<tokenizer<o200k,  huggingface_format>>(huggingface, "O200k");
}
