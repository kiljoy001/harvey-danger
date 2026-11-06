
void lmac_check_ac(void)

{
  uint *puVar1;
  int *piVar2;
  int *piVar3;
  int *piVar4;
  uint *puVar5;
  
  (*(code *)PTR_vPortETSIntrLock_000113e8)();
  piVar2 = (int *)PTR_DAT_000113a4;
  piVar3 = (int *)PTR_DAT_000113a8;
  if (((((byte)PTR_DAT_000113a4[0x11] != 0) &&
       (*(int *)PTR_DAT_000113a4 == *(int *)PTR_DAT_000113a8)) &&
      ((uint)(byte)PTR_DAT_000113a4[0x11] == *(uint *)(PTR_DAT_000113a8 + 4))) &&
     ((*(int *)(PTR_DAT_000113a4 + 0x1c) == *(int *)(PTR_DAT_000113a8 + 8) &&
      (*(int *)(PTR_DAT_000113a4 + 0x20) == *(int *)(PTR_DAT_000113a8 + 0xc))))) {
    (*(code *)PTR_ets_printf_000113ec)(PTR_s_E_AC__d___d___d_000113ac);
    memw();
    memw();
    memw();
    memw();
    memw();
    memw();
    piVar4 = DAT_000113b8;
    (*(code *)PTR_ets_printf_000113f0)
              (PTR_s_CONF__x__PLCP__x__rf__x__rx__x___000113c8,*DAT_000113b0,*DAT_000113b4,
               *DAT_000113b8,*DAT_000113bc,*DAT_000113c0,*DAT_000113c4);
    memw();
    if (*piVar4 != DAT_000113cc) {
      (*(code *)PTR_clockgate_watchdog_000113f4)(1);
      (*(code *)PTR_phy_open_rf_000113f8)();
      (*(code *)PTR_clockgate_watchdog_000113fc)(0);
    }
    puVar1 = DAT_000113d0;
    memw();
    memw();
    *DAT_000113d0 = *DAT_000113d0 & 0xfffffffd;
    memw();
    memw();
    *puVar1 = *puVar1 | 2;
    memw();
    *DAT_000113b8 = 0;
    (*(code *)PTR_pm_wakeup_init_00011400)(8);
    puVar5 = DAT_000113d4;
    memw();
    memw();
    *DAT_000113d4 = *DAT_000113d4 | DAT_000113d8;
    (*(code *)PTR_ets_delay_us_00011404)(0x22);
    puVar1 = DAT_000113b4;
    memw();
    memw();
    *DAT_000113b4 = *DAT_000113b4 & DAT_000113dc;
    memw();
    memw();
    *puVar5 = *puVar5 & DAT_000113e0;
    piVar2[8] = piVar2[8] + 1;
    if (*(char *)((int)piVar2 + 0x11) == '\x01') {
      memw();
      memw();
      *puVar1 = *puVar1 & DAT_000113e4;
      *(undefined1 *)((int)piVar2 + 0x11) = 6;
      if (*piVar2 != 0) {
        (*(code *)PTR_FUN_00011408)(piVar2);
      }
    }
  }
  (*(code *)PTR_vPortETSIntrUnlock_0001140c)();
  *piVar3 = *piVar2;
  piVar3[1] = (uint)*(byte *)((int)piVar2 + 0x11);
  piVar3[2] = piVar2[7];
  piVar3[3] = piVar2[8];
  return;
}

