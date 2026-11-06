# B27 - Proc creation failure is loud now

Status: DONE (host test + normal boot + forced-failure
boot on device + stock restore verified).

## The B26 failure mode, closed

B26 lost srv9p silently: the proc table filled, the
substrate printed one line nobody watched, and the
app ignored the xTaskCreate return. Fixes, smallest
that close it:

1. Substrate messages name the victim and the budget:
   `p9_os: FATAL out of procs (8 slots) creating srv9p`
   and the stack-pool equivalent with used/requested/
   total bytes (p9_os_xtensa.c).
2. Occupancy API: p9_os_proc_slots / p9_os_proc_used /
   p9_os_proc_name (p9_os_xtensa.h), so boot can print
   the budget instead of inferring it from a missing
   server.
3. esp9 app_main checks both required procs: cons or
   srv9p failing to start is ESP_LOGE + abort().
   Immediately after, boot prints the table:

       procs 7/8: uiT tiT TmrSvc sys_evt ppT cons srv9p

   (That line also settles the B26 slot census for
   real: seven live procs at done-time, one spare.)

## Tests

- Host (maintained): test_shim gained
  shim-task-exhaustion - fill the substrate with
  blocked procs and assert the next xTaskCreate
  returns exactly shimFAIL (the checkable signal the
  app now relies on), never shimPASS. test_shim,
  test_kernel, test_pp, test_esf_buf all pass.
- Target normal boot: procs line above, 9p-listening,
  memory gate green (headroom 26944).
- Target forced failure: rebuilt with MAX_XPROCS
  temporarily 6 (the B26 configuration), flashed.
  Boot log:

      p9_os: FATAL out of procs (6 slots) creating srv9p
      esp9: FATAL: srv9p proc create failed
      abort() was called at PC 0x40223991 on core 0
      PANIC wdt=0 frame= ... (panic dump follows)

  Unmissable, and it names the proc. Reverted to 8,
  rebuilt, reflashed, normal boot re-verified, then
  stock AT restored (AT/AT+GMR ok).

## Note

abort() on this substrate dumps and re-enters the
panic path repeatedly rather than rebooting cleanly.
For a fatal boot misconfiguration that is acceptable
(loud beats silent), but a clean fatal_reboot() call
would be tidier if this ever needs to be unattended.
