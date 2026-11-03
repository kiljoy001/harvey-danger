#!/bin/sh
# Run the capture program against Mesa's panfrost DRM shim and print the job
# chains the real panfrost Gallium driver builds.
set -e
cd /tmp/opencode

B=mesa25
SHIM=$PWD/$B/src/panfrost/drm-shim/libpanfrost_noop_drm_shim.so
EGL=$PWD/$B/src/egl/libEGL_mesa.so.0
DRI=$PWD/$B/src/gallium/targets/dri

ls -l "$SHIM" "$EGL" >/dev/null

mkdir -p /tmp/opencode/eglvendor
cat > /tmp/opencode/eglvendor/10_mesa.json <<EOF
{
    "file_format_version": "1.0.0",
    "ICD": { "library_path": "$EGL" }
}
EOF

__EGL_VENDOR_LIBRARY_FILENAMES=/tmp/opencode/eglvendor/10_mesa.json \
MESA_LOADER_DRIVER_OVERRIDE=panfrost \
LIBGL_DRIVERS_PATH=$DRI \
PAN_GPU_ID=${PAN_GPU_ID:-0x7002} \
LD_LIBRARY_PATH=$PWD/$B/src/egl:$DRI:$PWD/$B/src/util:$PWD/$B/src/mapi/es2api \
LD_PRELOAD=$SHIM \
./capture "$@"
