
undefined4 pm_rtc_clock_cali_proc(void)

{
  code *pcVar1;
  undefined *puVar2;
  
  puVar2 = PTR_DAT_00010148;
  pcVar1 = *(code **)(*(int *)PTR_g_phyFuns_0001014c + 0x98);
  *(undefined4 *)(PTR_DAT_00010148 + 4) = 0;
  (*pcVar1)(0x6a,2,8);
  (*(code *)PTR_pm_rtc_clock_cali_00010150)(puVar2);
  return *(undefined4 *)(puVar2 + 4);
}

