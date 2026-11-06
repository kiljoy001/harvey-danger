
void pm_recv_all_data(void)

{
  byte *pbVar1;
  byte bVar2;
  undefined *puVar3;
  
  puVar3 = PTR_DAT_00010cd8;
  pbVar1 = PTR_DAT_00010cd8 + 0x85;
  PTR_DAT_00010cd8[0x84] = PTR_DAT_00010cd8[0x88];
  bVar2 = puVar3[0x89];
  if ((*pbVar1 != bVar2) && (2 < bVar2)) {
    puVar3[0x85] = bVar2 - 2;
  }
  return;
}

