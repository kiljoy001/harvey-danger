
void pm_reset_keepalive_time(void)

{
  memw();
  *(int *)(PTR_DAT_00010d24 + 100) = *(int *)PTR_WdevTimOffSet_00010d28 + *DAT_00010d20;
  return;
}

