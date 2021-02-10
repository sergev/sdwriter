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

APPNAME = 'sdwriter'
VERSION = '0.1'

def options(opt):
    opt.load('compiler_c')

def configure(conf):
    out = 'build'
    conf.load('compiler_c')

def build(bld):
    LIBS = []
    if sys.platform == 'linux2':
        LIBS = ['udev']
    if sys.platform == 'darwin':
        bld.env.FRAMEWORK += ['CoreFoundation', 'IOKit']

    bld.program(
        source       = 'sdwriter.c',
        target       = APPNAME,
        includes     = ['.'],
        lib          = LIBS,
        install_path = '/usr/local/bin',
        cflags       = ['-g', '-O', '-Wall'],
        ldflags      = ['-g'],
    )
