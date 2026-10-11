#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2026 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
# Adds the libaelibridge library and the legacy effects it serves to an
# AIDL audio_effects_config.xml.
#
# usage: inject_effects.py SRC.xml DST.xml

import os
import re
import sys

LIBRARY = '<library name="aelibridge" path="libaelibridge.so"/>'

# name, implementation uuid, type uuid
EFFECTS = [
    # Dolby DAP (libswdap.so)
    ('dap', '9d4921da-8225-4f29-aefa-39537a04bcaa', '46d279d9-9be7-453d-9d7c-ef937f675587'),
]


def insert_before(text, closing, line):
    m = re.search(r'\n([ \t]*)' + re.escape(closing), text)
    if not m:
        sys.exit(f'{closing} not found')
    indent = m.group(1)
    return text[:m.start()] + f'\n{indent}    {line}' + text[m.start():]


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: inject_effects.py SRC.xml DST.xml')
    src, dst = sys.argv[1:3]
    text = open(src).read()
    if 'libaelibridge.so' not in text:
        text = insert_before(text, '</libraries>', LIBRARY)
    for name, uuid, type_uuid in EFFECTS:
        if uuid in text:
            continue
        text = insert_before(
            text, '</effects>',
            f'<effect name="{name}" library="aelibridge" uuid="{uuid}" type="{type_uuid}"/>')
    os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
    open(dst, 'w').write(text)


if __name__ == '__main__':
    main()
