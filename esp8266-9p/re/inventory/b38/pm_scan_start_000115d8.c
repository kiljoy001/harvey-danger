
bool pm_scan_start(void)

{
  bool bVar1;
  undefined *puVar2;
  
  (*(code *)PTR___wifi_task_suspend_all_000115c0)();
  if ((*(int *)(PTR_DAT_000115bc + 0x6c) == 7) || (*(int *)(PTR_DAT_000115bc + 0x6c) == 0)) {
    (*(code *)PTR___wifi_task_resume_all_000115d4)();
    bVar1 = true;
  }
  else {
    puVar2 = PTR_DAT_000115bc;
    (*(code *)PTR___wifi_task_suspend_all_000115c4)();
    (*(code *)PTR_pm_change_to_state_000115c8)(7);
    (*(code *)PTR___wifi_task_resume_all_000115cc)();
    (*(code *)PTR___wifi_task_resume_all_000115d0)();
    bVar1 = *(int *)(puVar2 + 0x6c) == 7;
  }
  return bVar1;
}

