
void wifi_pm_close_rf(void)

{
  int *piVar1;
  undefined *puVar2;
  uint uVar3;
  uint uVar4;
  uint uVar5;
  int iVar6;
  undefined8 uVar7;
  
  uVar7 = (*(code *)PTR_esp_timer_get_time_000102a4)();
  puVar2 = PTR_DAT_0001029c;
  uVar5 = *(uint *)PTR_DAT_0001029c;
  iVar6 = *(int *)(PTR_DAT_000102a0 + 4);
  uVar4 = uVar5 - *(int *)PTR_DAT_000102a0;
  piVar1 = (int *)(PTR_DAT_0001029c + 4);
  uVar3 = uVar4 + (int)uVar7;
  *(uint *)PTR_DAT_0001029c = uVar3;
  *(uint *)(puVar2 + 4) =
       (uint)(uVar3 < uVar4) +
       ((*piVar1 - iVar6) - (uint)(uVar5 < uVar4)) + (int)((ulonglong)uVar7 >> 0x20);
  (*(code *)PTR_clockgate_watchdog_000102a8)(1);
  (*(code *)PTR_phy_close_rf_000102ac)();
  (*(code *)PTR_clockgate_watchdog_000102b0)(0);
  (*(code *)PTR_esp_sleep_unlock_000102b4)();
  return;
}

