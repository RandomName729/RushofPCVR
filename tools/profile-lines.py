#!/usr/bin/env python3
"""Where inside one function the samples of a thread fall, by source line.

Goes with tools/symbolize-stacks.mjs --profile: once that has named the function a thread
spends its time in, this tells which lines of it (inlined callees included).

    profile-lines.py <core.log> <shadps4 binary> <thread name> <part of the function name>
"""
import collections
import os
import re
import subprocess
import sys


def main():
    log, binary, thread, function = sys.argv[1:5]
    innermost = collections.Counter()
    for line in open(log, encoding='latin1'):
        match = re.match(r'^  (.{16}) tid (\d+):(.*)$', line)
        if not match or thread not in match.group(1):
            continue
        # The first frame inside the emulator itself; library frames come in brackets.
        for frame in re.findall(r'\[[^\]]*\]|0x[0-9a-f]+', match.group(3)):
            if frame.startswith('0x'):
                innermost[frame] += 1
                break
    addresses = list(innermost)
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'llvm', 'bin',
                        'llvm-addr2line.exe')
    output = []
    # A command line only holds so much.
    for start in range(0, len(addresses), 400):
        output += subprocess.run([tool, '-f', '-C', '-e', binary] + addresses[start:start + 400],
                                 capture_output=True, text=True).stdout.split('\n')[:800]
    lines = collections.Counter()
    for index, address in enumerate(addresses):
        name, place = output[2 * index], output[2 * index + 1]
        if function in name:
            place = place.replace(chr(92), '/').split('/src/')[-1]
            lines[place] += innermost[address]
    total = sum(innermost.values())
    print(f'{function}: {sum(lines.values())} of {total} samples')
    for place, count in lines.most_common(25):
        print(f'{count:5} {place}')


if __name__ == '__main__':
    main()
