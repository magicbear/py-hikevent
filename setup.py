#!/usr/bin/env python3
# encoding: utf-8

from distutils.core import setup, Extension

hello_module = Extension('hikevent', sources = ['hikevent.cpp'],
                         include_dirs=['include'],
                         library_dirs=['lib'],
                         libraries=['hcnetsdk'],
                         extra_objects=['lib/libHCCore.so','lib/libhcnetsdk.so','lib/libhpr.so'])

setup(name='hikevent',
      version='0.1.0',
      description='Hello world module written in C',
      ext_modules=[hello_module])
