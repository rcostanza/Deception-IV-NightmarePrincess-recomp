"""scripts/link.py without game binaries: module tables of a synthetic SELF, ids, fs->gs loads."""
import hashlib
import struct
import tempfile
import unittest
from pathlib import Path

from paths import ROOT  # noqa: F401  (sys.path for scripts/)
import link


def self_fixture():
    """A SELF holding one prx-like module: one code byte, a string table, one exported symbol
    'bzQExy189ZI#C#A' whose library C and module A are both ('libc', 1), and empty relocation
    tables. Enough for link.module without game binaries."""
    data = bytearray(0x3a0)
    data[:4] = b'O='
    struct.pack_into('<H', data, 24, 2)
    struct.pack_into('<QQQQ', data, 32, 0x800, 0x200, 16, 16)
    struct.pack_into('<QQQQ', data, 64, 0x100800, 0x220, 0x160, 0x160)
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 96, b'ELF	', 0xfe18, 62, 1, 0, 64, 0, 0,
                     64, 56, 3, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', data, 160, 1, 5, 0x1000, 0, 0, 16, 16, 0x1000)
    struct.pack_into('<IIQQQQQQ', data, 216, 0x61000000, 4, 0x2000, 0, 0, 0x160, 0, 16)
    struct.pack_into('<IIQQQQQQ', data, 272, 2, 4, 0x20b0, 0, 0, 176, 176, 8)
    data[0x200] = 0xc3
    data[0x220:0x230] = b'bzQExy189ZI#C#A\0'
    data[0x230:0x235] = b'libc\0'
    struct.pack_into('<IBBHQQ', data, 0x260, 0, 0x12, 0, 1, 0, 1)
    libc = (1 << 32) | 16  # version 1, name at string offset 16
    tags = [(0x61000035, 0), (0x61000037, 32), (0x61000039, 64), (0x6100003f, 24),
            (0x61000029, 0), (0x6100002d, 0), (0x6100002f, 0), (0x61000031, 0),
            (0x61000013, (2 << 48) | libc), (0x6100000d, libc), (0, 0)]
    for i, (tag, value) in enumerate(tags):
        struct.pack_into('<QQ', data, 0x2d0 + 16 * i, tag, value)
    return bytes(data)



class ModuleTests(unittest.TestCase):
    def test_module_accepts_unpacked_elf_with_the_same_exports(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'libc.prx'
            source = self_fixture()
            path.write_bytes(source)
            expected = link.module(path)
            elf = link.parse_self(source)[0]
            path.write_bytes(elf)
            actual = link.module(path)
        self.assertEqual(actual['symbols'], expected['symbols'])
        self.assertEqual(actual['ph'], expected['ph'])
        self.assertEqual(actual['relocs'], expected['relocs'])
        self.assertEqual(actual['sha256'], hashlib.sha256(elf).hexdigest())

    def test_module_reads_the_file_once_and_reports_its_hash(self):
        data = self_fixture()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'libc.prx'
            path.write_bytes(data)
            info = link.module(path)
        self.assertEqual(info['sha256'], hashlib.sha256(data).hexdigest())
        self.assertEqual(info['missing'], [])
        self.assertEqual(info['symbols'][0]['identity'], ('bzQExy189ZI', ('libc', 1), ('libc', 1)))
        self.assertEqual(info['relocs'], [])

    def test_ids_are_base64_most_significant_first(self):
        self.assertEqual(link.encode_id(0), 'A')
        self.assertEqual(link.encode_id(2), 'C')
        self.assertEqual(link.encode_id(64), 'BA')
        self.assertEqual(link.encode_id(63), '-')


class FsLoadTests(unittest.TestCase):
    LOAD = bytes.fromhex('64488b042500000000')  # mov rax, fs:[0]

    def test_thread_pointer_loads_in_code_are_rewritten_to_gs(self):
        image = bytearray(self.LOAD + b'\x90' + self.LOAD + b'\xc3')
        ph = [dict(type=1, vaddr=0, filesz=len(image), memsz=len(image), flags=5)]
        self.assertEqual(link.patch_fs_loads(image, ph, 0), 2)
        self.assertEqual((image[0], image[10]), (0x65, 0x65))

    def test_loads_outside_executable_segments_are_kept(self):
        image = bytearray(self.LOAD)
        ph = [dict(type=1, vaddr=0, filesz=len(image), memsz=len(image), flags=6)]
        self.assertEqual(link.patch_fs_loads(image, ph, 0), 0)
        self.assertEqual(image[0], 0x64)

    def test_a_module_is_patched_at_its_base(self):
        image = bytearray(16) + bytearray(self.LOAD)
        ph = [dict(type=1, vaddr=0, filesz=len(self.LOAD), memsz=len(self.LOAD), flags=5)]
        self.assertEqual(link.patch_fs_loads(image, ph, 16), 1)
        self.assertEqual(image[16], 0x65)


if __name__ == '__main__':
    unittest.main()
