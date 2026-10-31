#!/usr/bin/env python3
"""Exercise the production GLB decoder, pose and dynamics at the 1024-node limit."""
import argparse
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=shutil.which('g++') or 'g++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    cpp = root / 'app/src/main/cpp'
    build = root / 'build/tests'
    build.mkdir(parents=True, exist_ok=True)
    binary = build / 'model_bone_limit_test.exe'
    subprocess.run([args.compiler, '-std=c++17', '-O0', '-static', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(cpp), '-isystem', str(cpp / 'third_party/glm'),
                    str(cpp / 'self_model/model_glb.cpp'),
                    str(cpp / 'self_model/tests/model_bone_limit_test.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
