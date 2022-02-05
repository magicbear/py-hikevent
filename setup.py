#!/usr/bin/env python3
# encoding: utf-8

from distutils.core import setup, Extension
import glob

hello_module = Extension('hikevent', sources = ['hikevent.cpp'],
                         include_dirs=['include'],
                         library_dirs=['lib'],
                         libraries=['hcnetsdk'],
                         runtime_library_dirs=['/usr/local/lib/hcnetsdk']
                         )

setup(name='hikevent',
      version='0.1.0',
      description='Hello world module written in C',
      ext_modules=[hello_module],
      data_files=[('/usr/local/lib/hcnetsdk', glob.glob('lib/*.so*')),('/usr/local/lib/hcnetsdk/HCNetSDKCom/', glob.glob('lib/HCNetSDKCom/*'))]
      )
