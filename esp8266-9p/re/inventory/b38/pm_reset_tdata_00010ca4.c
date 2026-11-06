
void pm_reset_tdata(void)

{
  PTR_DAT_00010ca0[0x83] = PTR_DAT_00010ca0[0x87];
  return;
}

