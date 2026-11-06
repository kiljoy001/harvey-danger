
void pm_set_sleep_type(int param_1)

{
  undefined4 uVar1;
  int iVar2;
  undefined *puVar3;
  undefined *puVar4;
  undefined *puVar5;
  
  (*(code *)PTR___wifi_task_suspend_all_000114b0)();
  if (*(int *)(PTR_DAT_00011498 + 0x74) != param_1) {
    puVar3 = PTR_DAT_00011498;
    puVar5 = PTR_DAT_00011498;
    (*(code *)PTR_pm_reset_param_000114b8)();
    puVar4 = PTR_DAT_0001149c;
    if (param_1 == 0) {
      *(undefined4 *)(puVar5 + 0x74) = 0;
      if (1 < *(uint *)(puVar5 + 0x6c)) {
        (*(code *)PTR___wifi_task_suspend_all_000114bc)();
        (*(code *)PTR_pm_change_to_state_000114c0)(param_1);
        (*(code *)PTR___wifi_task_resume_all_000114c4)();
      }
      uVar1 = (*(code *)PTR_esp_log_early_timestamp_000114c8)();
      (*(code *)PTR_ets_printf_000114cc)(PTR_DAT_000114a0,uVar1,puVar4);
    }
    else {
      iVar2 = *(int *)(puVar5 + 0x74);
      *(int *)(puVar5 + 0x74) = param_1;
      if ((((iVar2 == 0) && (**(char **)PTR_g_wifi_nvs_000114a8 == '\x01')) &&
          (*(int *)(PTR_g_ic_000114a4 + 8) != 0)) &&
         (*(int *)(*(int *)(PTR_g_ic_000114a4 + 8) + 0x78) == 5)) {
        (*(code *)PTR___wifi_task_suspend_all_000114d0)();
        (*(code *)PTR_pm_change_to_state_000114d4)(2);
        (*(code *)PTR___wifi_task_resume_all_000114d8)();
      }
      uVar1 = (*(code *)PTR_esp_log_early_timestamp_000114dc)();
      (*(code *)PTR_ets_printf_000114e0)
                (PTR_DAT_000114ac,uVar1,puVar4,*(undefined4 *)(puVar3 + 0x74));
    }
  }
  (*(code *)PTR___wifi_task_resume_all_000114b4)();
  return;
}

