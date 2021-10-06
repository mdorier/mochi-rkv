#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <rkv/cxx/rkv-exception.hpp>

namespace py = pybind11;
using namespace pybind11::literals;

PYBIND11_MODULE(pyrkv_common, m) {
    m.doc() = "Python binding for common stuff in the RKV library";

    py::register_exception<rkv::Exception>(m, "Exception");

    py::class_<rkv_database_id_t>(m, "DatabaseID")
        .def(py::init<>())
        .def("__str__", [](const rkv_database_id_t& id) {
            char id_str[37];
            rkv_database_id_to_string(id, id_str);
            return std::string(id_str, 36);
        })
        .def("__eq__", [](const rkv_database_id_t& id1, const rkv_database_id_t& id2) {
            return uuid_compare(id1.uuid, id2.uuid) == 0;
        })
        .def("__hash__", [](const rkv_database_id_t& id) {
                auto data = reinterpret_cast<const uint64_t*>(id.uuid);
                uint64_t h = (~data[0]) ^ data[1];
            return h;
        })
        .def_static("from_str", [](const std::string& str) {
            if(str.size() != 36) {
                throw std::invalid_argument("string should have 36 characters");
            }
            rkv_database_id_t db_id;
            rkv_database_id_from_string(str.data(), &db_id);
            return db_id;
        });
}

