
void pm_set_idle_timer(void)

{
  undefined *puVar1;
  
  if (*PTR_DAT_00010c60 == '\0') {
    puVar1 = PTR_DAT_00010c60;
    (*(code *)PTR_os_timer_arm_00010c6c)(PTR_DAT_00010c68,PTR_DAT_00010c64[0x86]);
    *puVar1 = 1;
  }
  return;
}

