
void pm_set_sleep_time(undefined4 param_1)

{
  (*(code *)PTR_pm_usec2rtc_0001018c)(param_1,*(undefined4 *)(PTR_DAT_00010188 + 4));
  (*(code *)PTR_pm_set_sleep_cycles_00010190)();
  return;
}

