#!/usr/bin/busybox sh
/bin/busybox --install -s /bin
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t devtmpfs dev /dev
mount -t proc proc /proc
mount -t sysfs sys /sys
echo "=== LINUX-UP"
uname -a
echo "=== MODULES"
insmod /lib/modules/6.18.21-current-rockchip64/kernel/drivers/gpu/drm/scheduler/gpu-sched.ko && echo gpu-sched ok
insmod /lib/modules/6.18.21-current-rockchip64/kernel/drivers/gpu/drm/drm_shmem_helper.ko && echo shmem ok
insmod /lib/modules/6.18.21-current-rockchip64/kernel/drivers/gpu/drm/panfrost/panfrost.ko && echo panfrost ok
sleep 3
echo "=== DRM NODES"
ls -l /dev/dri 2>&1
echo "=== PANFROST"
ls /sys/bus/platform/drivers/panfrost/ 2>&1
dmesg | grep -i -E "panfrost|mali|drm" | tail -20
echo "=== CAPTURE surfaceless"
EGL_PLATFORM=surfaceless PAN_MESA_DEBUG=trace /capture
echo "=== CAPTURE rc=$?"
echo "=== CAPTURE gbm"
PAN_MESA_DEBUG=trace /capture_gbm
echo "=== GBM rc=$?"
echo "=== CAPTURE-DONE"
poweroff -f
while true; do sleep 5; done
