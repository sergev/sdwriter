#! /usr/bin/env python3
# encoding: utf-8
#
# Use:
#       waf configure build
#       waf install
#       waf clean
#       waf distclean
#
import sys

def options(project):
    project.load('compiler_c')

def configure(project):
    out = 'build'
    project.load('compiler_c')

def build(project):
    LIBS = []
    if sys.platform == 'linux2':
        LIBS = ['udev']

    if sys.platform == 'darwin':
        project.env.FRAMEWORK += ['CoreFoundation', 'IOKit']

    project.program(
        source       = 'sdwriter.c',
        target       = 'sdwriter',
        includes     = ['.'],
        lib          = LIBS,
        install_path = '/usr/local/bin',
        cflags       = ['-g', '-O', '-Wall'],
        ldflags      = ['-g'],
    )
