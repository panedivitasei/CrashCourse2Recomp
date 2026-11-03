"""Build exact boolean-specialized DXIL variants from a translated pixel shader."""
import argparse
import concurrent.futures
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--translator', type=Path, required=True)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    translator = args.translator.resolve()
    source = Path(str(args.prefix) + '.ps.hlsl').read_text()
    original = Path(str(args.prefix) + '.ps.dxil').read_bytes()
    reflection = Path(str(args.prefix) + '.ps.refl').read_bytes()
    mask, = struct.unpack_from('<I', reflection, 36)
    bits = [i for i in range(32) if mask & (1 << i)]
    if reflection[0] != 1 or mask & 0xFFFF or not 0 < len(bits) <= 10:
        raise ValueError('Expected a pixel shader with 1-10 upper-word boolean bits')
    definition = '#define g_Booleans ((xe_bool_word(1)) << 16)'
    if source.count(definition) != 1:
        raise ValueError('Unrecognized boolean definition')
    with tempfile.TemporaryDirectory(prefix='cc2_boolean_') as temporary:
        root = Path(temporary)

        def compile_variant(index):
            value = sum(1 << bit for j, bit in enumerate(bits) if index & (1 << j))
            hlsl = root / f'{index}.hlsl'
            dxil = root / f'{index}.dxil'
            hlsl.write_text(source.replace(definition, f'#define g_Booleans {value}u'))
            result = subprocess.run([str(translator), '--compile', str(hlsl), str(dxil), 'ps_6_0'],
                                    capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            blob = dxil.read_bytes()
            if blob[:4] != b'DXBC' or struct.unpack_from('<I', blob, 24)[0] != len(blob):
                raise ValueError('Invalid compiled DXIL container')
            return value, blob

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            variants = list(pool.map(compile_variant, range(1 << len(bits))))
    data = bytearray(b'CC2BOOL1')
    data += struct.pack('<III', len(original), mask, len(variants)) + original
    for value, blob in variants:
        data += struct.pack('<II', value, len(blob)) + blob
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary_output = args.output.with_suffix('.tmp')
    temporary_output.write_bytes(data)
    temporary_output.replace(args.output)
    sizes = [len(blob) for _, blob in variants]
    print(f'{len(variants)} variants, mask {mask:08X}, original {len(original)} bytes, '
          f'variants {min(sizes)}-{max(sizes)} bytes, bundle {len(data)} bytes', flush=True)


if __name__ == '__main__':
    main()
