#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Independent, manually assembled wire oracle. No production codec imports.

--check compares the committed full-byte vectors. --write is an explicit maintainer
operation, never used to obtain expected output during the C test. See
docs/controller-profile.md for provenance.
"""
import argparse
from pathlib import Path


def reverse(value, bits):
    return int(f'{value:0{bits}b}'[::-1], 2)


def checksum(data, reflected=False):
    # Polynomial long division of an augmented bit string, not the codec's
    # byte-at-a-time LFSR. Reflect inputs/output for MODBUS; CMS is unreflected.
    if reflected:
        data = bytes(reverse(b, 8) for b in data)
    dividend = (int.from_bytes(data, 'big') << 16) ^ (0xffff << (8 * len(data)))
    while dividend.bit_length() > 16:
        dividend ^= 0x18005 << (dividend.bit_length() - 17)
    return reverse(dividend, 16) if reflected else dividend


def envelope(source, destination, payload):
    payload = bytes.fromhex(payload)
    data = bytes([len(payload) + 11]) + bytes.fromhex(source + 'c6' + destination) + payload
    data += checksum(data[1:], True).to_bytes(2, 'little')
    return (data + checksum(data).to_bytes(2, 'big')).hex().upper()


# Report payloads and expected state payloads are handwritten records in the
# specification's order. Only envelope/CRCs are calculated. Addresses are invented.
BASE = 'df203000404b02a88a02ba00e100641919da000a000a0a47'
# name, controller, radiator, mode, centi-C, seconds, minutes, report, expected payload
CASES = [
    ('comfort', 'f1010203', 'f1040506', 0, 2100, 43200, 120, BASE,
     '4500004b02a83b004c00008e000084cc00000000972030049402ba00e100641919da000000005b0078b20c0000'),
    ('off_min_clock_duration', 'f1010203', 'f1040506', 1, 0, 0, 1, BASE,
     '4500004b02a83b004c00008e000084cc000000009720300494014001e100641919da000000005b0001b2000000'),
    ('frost_unavailable_max_clock_duration', 'f1010203', 'f1040506', 2, 700, 86399, 1439,
     'df003001404b084c8a084c63e100373764da1234ffff0a47',
     '4500004b084c3b004c00008e000084cc00000000970030059401be02e100373764da000000005b059fb2173b3b'),
    ('eco_local_change_exact_echo', 'f1010203', 'f1040506', 3, 1550, 12345, 117,
     'df203100404b00e68a023c03e10137feffdaffff12340a47',
     '4500004b00e63b004c00008e000084cc000000009720300494025703e10137feffda000000005b0075b203192d'),
    ('minimum_target_second_pair', 'f10a0b0c', 'f1fedcba', 0, 500, 3599, 116,
     'df003000404b01238a02ba00e100460123daabcd56780a47',
     '4500004b01233b004c00008e000084cc000000009700300494019a00e100460123da000000005b0074b2003b3b'),
    ('maximum_target_d0_family', 'd0010203', 'd0040506', 0, 3000, 3600, 256,
     'df20ffff7f4bffff8a02ba08e1abcdef01daabcd12340a47',
     '4500004bffff3b004c00008e000084cc000000009720300594035c00e1abcdef01da000000005b0100b2010000'),
    ('round_down', 'f1010203', 'f1040506', 0, 2051, 59, 120, BASE,
     '4500004b02a83b004c00008e000084cc00000000972030049402b100e100641919da000000005b0078b200003b'),
    ('round_up_heating', 'f1010203', 'f1040506', 0, 2053, 60, 120,
     'df203001404b02a88a02ba00e100641919da000a000a0a47',
     '4500004b02a83b004c00008e000084cc00000000972030059402b200e100641919da000000005b0078b2000100'),
    ('local_change_and_heating', 'f1010203', 'f1040506', 0, 2100, 43200, 120,
     'df203101404b02a88a02ba00e100641919da000a000a0a47',
     '4500004b02a83b004c00008e000084cc00000000972030059402ba00e100641919da000000005b0078b20c0000'),
    ('third_f1_controller', 'f1070809', 'f1040506', 0, 2050, 0, 120, BASE,
     '4500004b02a83b004c00008e000084cc00000000972030049402b100e100641919da000000005b0078b2000000'),
]


def render():
    assert checksum(b'123456789', True) == 0x4b37
    assert checksum(b'123456789') == 0xaee7
    lines = ['// SPDX-License-Identifier: MIT',
             '// Independent full bytes: profile_vectors.py; do not derive from the C builder.',
             'static const struct profile_vector vectors[] = {']
    for name, controller, radiator, mode, centi, clock, minutes, report, state in CASES:
        source = ','.join('0x' + controller[i:i+2] for i in range(0, 8, 2))
        dest = ','.join('0x' + radiator[i:i+2] for i in range(0, 8, 2))
        lines += [f'    // {name}',
                  f'    {{{{{{{source}}}, {{{dest}}}, (rf_mode){mode}, {centi}, {clock}, {minutes}}},',
                  f'     "{envelope(radiator, controller, report)}",',
                  f'     "{envelope(controller, radiator, state)}"}},']
    lines += ['};']
    # A deliberately different legacy template proves that path keeps opaque
    # values, 97 bits, and 5B rather than adopting the new profile's constants.
    template = '45a1b24b02a83b7f4c33448e112233cc55667788971155a09402ba00e100641919da000000005b0081b20c0000'
    legacy = '45a1b24b02a83b7f4c33448e112233cc55667788972055a094025703e100641919da000000005b0081b2000000'
    lines += [f'static const char *legacy_template = "{envelope("f1010203", "f1040506", template)}";',
              f'static const char *legacy_expected = "{envelope("f1010203", "f1040506", legacy)}";']
    # CRC-valid shape corruption: the report decoder's historical tolerance is
    # left alone, but the profile requires 0A=47 and the exact documented tags.
    for name, payload in [('bad_tail', BASE[:-2] + '46'), ('bad_tag', BASE.replace('4b02a8', '4c02a8'))]:
        lines += [f'static const char *{name} = "{envelope("f1040506", "f1010203", payload)}";']
    lines += ['static const struct { const char *controller, *radiator, *report; } invalid_pairs[] = {']
    for controller, radiator in [('a1010203', 'a1040506'), ('f1010203', 'd0040506'),
                                  ('f1000000', 'f1040506'), ('f1010203', 'f1010203')]:
        lines += [f'    {{"{controller}", "{radiator}", "{envelope(radiator, controller, BASE)}"}},']
    lines += ['};']
    # Independent failures of inner vs outer CRC, resealing only the outer for
    # the inner failure. Both are full-sized frames, not truncation cases.
    raw = bytearray.fromhex(envelope('f1040506', 'f1010203', BASE))
    raw[-4] ^= 1
    raw[-2:] = checksum(raw[:-2]).to_bytes(2, 'big')
    lines += [f'static const char *bad_inner = "{raw.hex().upper()}";']
    raw = bytearray.fromhex(envelope('f1040506', 'f1010203', BASE))
    raw[-1] ^= 1
    lines += [f'static const char *bad_outer = "{raw.hex().upper()}";']
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    path = Path(__file__).with_name('profile_vectors.inc')
    expected = render()
    if args.write:
        path.write_text(expected)
    elif args.check:
        assert path.read_text() == expected, 'independent vectors differ'
        print('Independent profile vectors and both CRC check values: passed')
    else:
        print(expected, end='')
