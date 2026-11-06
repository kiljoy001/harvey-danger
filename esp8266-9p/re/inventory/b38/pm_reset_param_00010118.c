
void pm_reset_param(void)

{
  undefined *puVar1;
  int iVar2;
  
  puVar1 = PTR_DAT_00010110;
  *(undefined4 *)PTR_DAT_00010100 = DAT_00010104;
  *(undefined4 *)PTR_DAT_00010108 = DAT_0001010c;
  iVar2 = 0;
  do {
    puVar1[iVar2] = 0;
    iVar2 = iVar2 + 1;
  } while (iVar2 != 0x177);
  *PTR_DAT_00010114 = 0;
  return;
}

