
void FUN_00011fe8(int param_1)

{
  byte bVar1;
  undefined *puVar2;
  undefined *puVar3;
  
  puVar2 = PTR_DAT_00011fc4;
  puVar3 = PTR_DAT_00011fc0;
  if ((*(uint *)(*(int *)(param_1 + 0x28) + 4) & DAT_00011fb8) == DAT_00011fbc) {
    PTR_DAT_00011fc4[0x83] = PTR_DAT_00011fc4[0x87];
    *puVar3 = 0;
    if (*(int *)(puVar2 + 0x6c) == 4) {
      (*(code *)PTR___wifi_task_suspend_all_00011fd4)();
      (*(code *)PTR_pm_change_to_state_00011fd8)(6);
      (*(code *)PTR___wifi_task_resume_all_00011fdc)();
    }
    else if (*(int *)(puVar2 + 0x6c) == 3) {
      puVar3 = PTR_DAT_00011fc8;
      (*(code *)PTR_os_timer_disarm_00011fe0)(PTR_DAT_00011fc8);
      (*(code *)PTR_os_timer_arm_00011fe4)(puVar3,10,0);
      *PTR_DAT_00011fcc = 1;
    }
  }
  else {
    bVar1 = *PTR_DAT_00011fc0;
    *PTR_DAT_00011fc0 = bVar1 + 1;
    if (0xe < bVar1) {
      *puVar3 = 0xe;
    }
    (*(code *)PTR_pm_send_nullfunc_00011fd0)(1);
  }
  return;
}

