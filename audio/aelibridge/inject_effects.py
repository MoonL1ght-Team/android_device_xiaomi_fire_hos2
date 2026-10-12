#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2026 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
# Adds the libaelibridge library with the legacy effects it serves, and
# native AIDL effects shipped by the tree, to an AIDL audio_effects_config.xml.
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

# Native AIDL effects, loaded by the factory directly
NATIVE = [
    # library name, path
    (('v4a_aidl', 'libv4a_aidl.so'),
     # ViPER4Android RE: the type is the AIDL one the app looks for
     ('v4a_standard_aidl', '90380da3-8536-4744-a6a3-5731970e640f',
      '7261676f-6d75-7369-6364-28e2fd3ac39e')),
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
    for (lib, path), (name, uuid, type_uuid) in NATIVE:
        if path not in text:
            text = insert_before(text, '</libraries>', f'<library name="{lib}" path="{path}"/>')
        if uuid not in text:
            text = insert_before(
                text, '</effects>',
                f'<effect name="{name}" library="{lib}" uuid="{uuid}" type="{type_uuid}"/>')
    os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
    open(dst, 'w').write(text)


if __name__ == '__main__':
    main()
