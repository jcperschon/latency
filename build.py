import argparse
import os
import subprocess


class pushd(object):

    def __enter__(self):
        try:
            os.chdir(self._new)
        except:
            pass

    def __exit__(self, type, value, traceback):
        os.chdir(self._old)

    def __init__(self, path):
        self._old = os.getcwd()
        self._new = path

def cmake(action, build_type, memcheck, skip_tests):
    source = os.getcwd()
    build_types = {
        'debug': '-DCMAKE_BUILD_TYPE=Debug',
        'release': '-DCMAKE_BUILD_TYPE=Release',
    }
    cmd = 'cmake %s %s' % (build_types[build_type], source)
    cmds = {
        'build': ['sudo ldconfig', cmd, 'make -j4'],
        'clean': ['make clean', 'rm -f CMakeCache.txt'],
        'install': ['sudo make install', 'sudo ldconfig'],
    }
    with pushd('build'):
        if not skip_tests:
            cmds['build'].append('ctest --output-on-failure')
        if memcheck and not skip_tests:
            cmds['build'].append('ctest -T memcheck --output-on-failure')
        for cmd in cmds[action]:
            subprocess.check_call(cmd.split())

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-a', type=str, help='Build action', dest='action',
                        action='store',
                        choices=['build', 'clean', 'install', 'package'],
                        default='build')
    parser.add_argument('-t', type=str, help="Build output type", dest='type',
                        action='store', choices=["debug", "release"],
                        default='debug')
    parser.add_argument('-M', help="Do memory leak checks", action='store_true')
    parser.add_argument('-S', help="Skip tests", action='store_true')
    args = parser.parse_args()
    cmake(args.action, args.type, args.M, args.S)
