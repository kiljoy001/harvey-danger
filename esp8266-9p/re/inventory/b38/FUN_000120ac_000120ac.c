
void FUN_000120ac(int param_1)

{
  int iVar1;
  char *pcVar2;
  
  pcVar2 = PTR_DAT_0001207c;
  if (0xf < (byte)*PTR_DAT_0001207c) {
    *(uint *)(*(int *)(param_1 + 0x28) + 4) =
         *(uint *)(*(int *)(param_1 + 0x28) + 4) & DAT_00012080 | DAT_00012084;
  }
  if ((*(uint *)(*(int *)(param_1 + 0x28) + 4) & DAT_00012088) == DAT_00012084) {
    *pcVar2 = 0;
    if (*(int *)(PTR_DAT_00012090 + 0x6c) == 5) {
      (*(code *)PTR___wifi_task_suspend_all_0001209c)();
      (*(code *)PTR_pm_change_to_state_000120a0)(2);
      (*(code *)PTR___wifi_task_resume_all_000120a4)();
    }
    (*(code *)PTR_ppProcessWaitQ_000120a8)();
  }
  else {
    iVar1 = (*(code *)PTR_rc_get_trc_00012094)(0,PTR_DAT_0001208c);
    if (iVar1 != 0) {
      *pcVar2 = *pcVar2 + '\x01';
      (*(code *)PTR_pm_send_nullfunc_00012098)(0);
    }
  }
  return;
}

