
void pm_attach(void)

{
  undefined1 uVar1;
  undefined *puVar2;
  
  (*(code *)PTR___wifi_task_suspend_all_00010d64)();
  puVar2 = PTR_DAT_00010d44;
  *(undefined4 *)(PTR_DAT_00010d44 + 0x74) = 1;
  puVar2[0x86] = 0x14;
  puVar2[0x8d] = 1;
  puVar2[0x89] = 4;
  puVar2[0x85] = 4;
  puVar2[0x8b] = 0x18;
  puVar2[0x8a] = 4;
  puVar2[0x87] = 2;
  puVar2[0x88] = 2;
  puVar2[0x83] = 2;
  puVar2[0x84] = 2;
  puVar2[0x8c] = 2;
  uVar1 = (*(code *)PTR_wifi_nvs_get_sta_listen_interval_00010d68)();
  puVar2[0x78] = uVar1;
  (*(code *)PTR_pm_reset_param_00010d6c)();
  (*(code *)PTR_os_timer_setfn_00010d70)(puVar2,PTR_pm_wait_bcn_fun_00010d48,0);
  (*(code *)PTR_os_timer_setfn_00010d74)(PTR_DAT_00010d50,PTR_pm_open_rf_for_bcn_fun_00010d4c,0);
  (*(code *)PTR_os_timer_setfn_00010d78)(PTR_DAT_00010d58,PTR_pm_wait_tx_rx_idle_fun_00010d54,0);
  (*(code *)PTR_ppRegisterTxCallback_00010d7c)(PTR_FUN_00010d5c,6);
  (*(code *)PTR_ppRegisterTxCallback_00010d80)(PTR_FUN_00010d60,5);
  (*(code *)PTR___wifi_task_resume_all_00010d84)();
  return;
}

