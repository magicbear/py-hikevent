#!/usr/bin/env python3
# encoding: utf-8

from distutils.core import setup, Extension
import glob
import pkgconfig

d = pkgconfig.parse('libavcodec libavformat libavutil libswresample libswscale')

hello_module = Extension('hikevent', sources = ['hikevent.cpp', 'hikbase.c'],
                         include_dirs=d['include_dirs'] + ['include'],
                         library_dirs=d['library_dirs'] + ['lib'],
                         libraries=d['libraries'] + ['hcnetsdk','HCCore','PlayCtrl','AudioRender', 'SuperRender'],
                         runtime_library_dirs=['/usr/local/lib/hcnetsdk']
                         )

setup(name='hikevent',
      version='0.1.0',
      description='Hello world module written in C',
      ext_modules=[hello_module],
      data_files=[('/usr/local/lib/hcnetsdk', glob.glob('lib/*.so*')),('/usr/local/lib/hcnetsdk/HCNetSDKCom/', glob.glob('lib/HCNetSDKCom/*')),
                  ('/etc/ld.so.conf.d/', ['hcnetsdk.conf'])]
      )
