#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

import os, tempfile, subprocess

__all__ = ["AsmException", "ARMAsm"]

class AsmException(Exception):
    pass

class ARMAsm:
    HEADER = b'.text\n.globl _start\n_start:\n'
    FOOTER = b'\n.pool\n'

    _env = {'PATH': os.environ.get('PATH', '/usr/bin'), 'LANG': 'C'}
    _arch = os.environ.get('ARCH', 'aarch64-linux-gnu-')
    _use_llvm = None
    _llvm_suffix = None

    def __init__(self, source, addr=0):
        source = self.HEADER + source.encode('ascii') + self.FOOTER

        self.addr = addr
        self._elf_data, self.data = self._compile(source, addr)
        self.len = len(self.data)

    def disassemble(self):
        disass_cmd = self._build_command('objdump', '-zd /dev/stdin',
            llvm_arch_arg='arch')
        proc = subprocess.run(disass_cmd, input=self._elf_data, env=self._env,
            check=True, stdout=subprocess.PIPE)

        for line in proc.stdout.decode('ascii').split('\n'):
            if not line or line[0] not in ' ':
                continue

            yield line

    @classmethod
    def _compile(cls, source, addr):
        # gcc and objdump requires writable directory
        with tempfile.TemporaryDirectory('m1n1asm') as dir:
            elf_file_name = os.path.join(dir, 'b.elf')

            asm_cmd = cls._build_command('gcc', '-pipe -Wall -nostartfiles '
                '-nodefaultlibs -march=armv8.2-a -x assembler - -o {elf}',
                elf=elf_file_name, linker_arg=f'-Ttext={addr:#x}')

            copy_cmd = cls._build_command('objcopy', '-j.text -O binary {elf} '
                '/dev/stdout', elf=elf_file_name, llvm_arch_arg='input-target')

            subprocess.run(asm_cmd, input=source, env=cls._env, check=True)
            proc = subprocess.run(copy_cmd, env=cls._env, check=True,
                stdout=subprocess.PIPE)

            with open(elf_file_name, 'rb') as elf_file:
                elf_data = elf_file.read()

            return elf_data, proc.stdout

    @classmethod
    def _build_command(cls, tool, cmd_args, llvm_arch_arg='target',
        linker_arg='', **fmtargs):
        if cls._use_llvm is None:
            suffix = os.environ.get('USE_LLVM')
            if suffix is None:
                suffix = os.environ.get('USE_CLANG')

            if suffix is None:
                cls._use_llvm = False
            else:
                if len(suffix) < 2:
                    raise ValueError("USE_LLVM must indicate the full suffix "
                        "to put on llvm/clang commands (example: USE_LLVM=-11 "
                        "will use clang-11 and llvm-objcopy-11)")
                cls._use_llvm = True
                cls._llvm_suffix = suffix

        if cls._use_llvm:
            if tool == 'gcc':
                tool = 'clang'
            else:
                tool = f'llvm-{tool}'

            cmdline = [f'{tool}{cls._llvm_suffix}',
                f'--{llvm_arch_arg}={cls._arch}']

            if linker_arg:
                cmdline.extend(('-fuse-ld=lld', f'-Wl,{linker_arg}'))

        else:
            cmdline = [f'{cls._arch}{tool}']

            if linker_arg:
                cmdline.append(linker_arg)

        cmdline.extend(cmd_args.format(**fmtargs).split())

        return cmdline


if __name__ == "__main__":
    import struct
    code = """
    ldr x0, =0xDEADBEEF
    ret
    """
    c = ARMAsm(code, 0x1238)
    print("\n".join(c.disassemble()))
    assert c.addr == 0x1238
    assert struct.unpack('4I', c.data) == (
        0x58000040,  # ldr x0, +0x8
        0x0d65f03c0, # ret
        0xdeadbeef,
        0x00000000,
    )

