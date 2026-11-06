
uint pm_allow_tx(char param_1)

{
  uint uVar1;
  uint uVar2;
  
  uVar2 = *(uint *)(PTR_DAT_000113c8 + 0x6c);
  uVar1 = (uint)(uVar2 == 0 || uVar2 == 7);
  if (uVar1 != 0) {
    return uVar1;
  }
  if (uVar2 == 2) {
    if (*(int *)(PTR_DAT_000113c8 + 0x74) == 0) {
      return 1;
    }
    if (param_1 == '\0') goto LAB_00011450;
LAB_00011427:
    PTR_DAT_000113c8[0x83] = 0;
  }
  else if (param_1 != '\0') goto LAB_00011427;
  if (uVar2 == 1) {
    (*(code *)PTR___wifi_task_suspend_all_000113cc)();
    (*(code *)PTR_pm_change_to_state_000113d0)(0);
    (*(code *)PTR___wifi_task_resume_all_000113d4)();
    (*(code *)PTR_pm_set_idle_timer_000113d8)();
    return uVar2;
  }
LAB_00011450:
  if ((uVar2 == 2) || (uVar2 == 5)) {
    (*(code *)PTR_pm_set_idle_timer_000113ec)();
    uVar1 = 1;
  }
  else {
    (*(code *)PTR___wifi_task_suspend_all_000113dc)();
    (*(code *)PTR_pm_change_to_state_000113e0)(2);
    (*(code *)PTR___wifi_task_resume_all_000113e4)();
    (*(code *)PTR_pm_set_idle_timer_000113e8)();
  }
  return uVar1;
}

