#!/bin/sh
# Fetch LovyanGFX 1.2.32 into components/LovyanGFX (it is not kept in git).
set -e
cd "$(dirname "$0")/.."
if [ -f components/LovyanGFX/library.json ]; then
  echo "components/LovyanGFX already present"; exit 0
fi
git clone --depth 1 --branch 1.2.32 https://github.com/lovyan03/LovyanGFX.git components/LovyanGFX
rm -rf components/LovyanGFX/.git
