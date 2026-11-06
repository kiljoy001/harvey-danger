
ulonglong wdev_get_tsf_time(int param_1)

{
  if (param_1 == 0) {
    memw();
    return (ulonglong)
           (uint)(*(int *)PTR_WdevTimOffSet_000121a4 + *(int *)PTR_DAT_000121a8 + *DAT_000121a0);
  }
  memw();
  memw();
  return CONCAT44(*DAT_000121ac,*DAT_000121b0);
}

