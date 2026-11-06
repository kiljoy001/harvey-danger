# B15 - OSI adapter fixes applied (item 3, adapter half)

Status: fixes applied, host- and device-verified. Board
restored to stock AT. Blob-free Wi-Fi operation (Phase C
proper) is not claimed; see the boundary note at the end.

## The three fixes (from the blob call-site RE)

In `firmware/kernel/osi_wifi.c`:

1. `__wifi_queue_create` rejected `queue_len > 32`, so
   pp_attach's `__wifi_queue_create(75, 8)` would have failed
   and the pp task would never start. The cap is now on the
   product: `queue_len * item_size <= 1024` (the per-queue pool
   size), with `queue_len <= 255`. 75x8 = 600 B fits.
2. `__wifi_task_get_max_priority` returned 32. The RTOS
   config sets `configMAX_PRIORITIES` to 15, and pp_attach
   runs its task at max-2 = 13. Now returns 15.
3. `__wifi_queue_send_from_isr` always wrote `*hptw = 0`, so
   pp_post_from_isr's yield-on-wake could never trigger. A
   successful send now reports hptw=1; a cooperative yield is
   always safe on this substrate.

## Verification

Failing-first: the new `test_osi_blob_contract` in
`firmware/kernel/test/test_kernel.c` failed on priority (15
expected), then passed after the fixes, together with the
75-entry queue fill/drain and hptw checks:
`test_kernel: all tests passed`.

On device, kerntest gained the same checks
(`osi-max-priority`, `osi-queue-75x8`, `osi-from-isr-hptw`,
`osi-queue-75-recv`) and still ends
`KERNEL-TEST ALL-PASS heap=13496` with REGDUMP sane.

Note the flash dance this required: the RTOS bootloader from
B14 occupies sector 0, so stock boot had to be restored before
the NONOS kerntest image at 0x1000 would boot. Flash layouts
are not interchangeable between substrates; the restore script
must always start from the full stock backup.

## Boundary: what "blob-free" still means

The adapter contract is now proven against the real blob call
sites, on host and on the Phase C substrate. What remains for
Phase C proper is linking `libpp.a`/`libnet80211.a` against
this OSI surface with no FreeRTOS in the image and bringing
the radio up on our scheduler. That is a link-and-bringup
campaign (undefined-symbol closure over the blob set), not a
small patch, and it has not been started. The honest state:
adapter ready, blobs not yet run blob-free.

## Next steps

1. Blob link experiment: link libpp.a against libp9.a in a
   kerntest-style image and enumerate the undefined-symbol
   closure. Size the campaign from that list.
2. Three-way substrate benchmark (NONOS device numbers).
