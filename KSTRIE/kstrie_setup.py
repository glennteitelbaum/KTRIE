from pybind11.setup_helpers import Pybind11Extension, build_ext
from setuptools import setup

setup(
    ext_modules=[
        Pybind11Extension(
            "kstrie",
            ["py_kstrie.cpp"],
            extra_compile_args=["-std=c++23", "-O2", "-march=x86-64-v4"],
        ),
    ],
    cmdclass={"build_ext": build_ext},
)
