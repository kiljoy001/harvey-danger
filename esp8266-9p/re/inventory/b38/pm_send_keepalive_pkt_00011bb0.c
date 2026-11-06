
bool pm_send_keepalive_pkt(void)

{
  bool bVar1;
  undefined *puVar2;
  
  puVar2 = PTR_DAT_00011ba4;
  memw();
  bVar1 = DAT_00011ba8 <
          (uint)((*(int *)PTR_WdevTimOffSet_00011ba0 - *(int *)(PTR_DAT_00011ba4 + 100)) +
                *DAT_00011b9c);
  if (bVar1) {
    memw();
    *(int *)(PTR_DAT_00011ba4 + 100) = *(int *)PTR_WdevTimOffSet_00011ba0 + *DAT_00011b9c;
    (*(code *)PTR_pm_send_nullfunc_00011bac)((*(uint *)(puVar2 + 0x6c) & 0xfffffffd) == 4);
  }
  return bVar1;
}

