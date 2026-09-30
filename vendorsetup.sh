#!/bin/bash

echo "fire: applying source patches"
bash "$(dirname "${BASH_SOURCE[0]}")/patches/apply-patches.sh"
