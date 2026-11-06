
void FUN_40105fd8(void)

{
  uint uVar1;
  ushort *puVar2;
  int iVar3;
  ushort *puVar4;
  
  memw();
  if (((*(uint *)(DAT_40102780 + 0x238) & 0xff) < 0x42) ||
     ((*(uint *)(DAT_40102780 + 0x238) & 0x80) != 0)) {
    puVar4 = *(ushort **)(*(int *)(*(int *)PTR_DAT_401053f8 + 8) + 4);
    uVar1 = *(uint *)(puVar4 + 2);
    if (((*puVar4 >> 0xe != 0) &&
        ((((uVar1 & 0x80) != 0 || (7 < (uVar1 & 0x7f))) || ((uVar1 & 0x40000000) != 0)))) &&
       (puVar2 = (ushort *)FUN_40100cd4(0xc,PTR_s_wdev_c_40105fd4,0x79e), puVar2 != (ushort *)0x0))
    {
      (*DAT_40100558)(puVar2,puVar4,0xc);
      iVar3 = FUN_401064e8();
      if (iVar3 == 0) {
        (*DAT_40100180)(PTR_s__s__u_4010556c,PTR_DAT_40105570,0x7a5);
        do {
                    /* WARNING: Do nothing block with infinite loop */
        } while( true );
      }
      *(uint *)(puVar2 + 4) =
           *(uint *)(puVar2 + 4) & DAT_401028d4 | (*(byte *)(iVar3 + 6) & 0xf) << 0x10;
      if (*puVar2 >> 0xe != 0) {
        uVar1 = *(uint *)(puVar2 + 2);
        if (((((uVar1 & 0x80) != 0) || (7 < (uVar1 & 0x7f))) || ((uVar1 & 0x40000000) != 0)) &&
           ((uVar1 & 0x7f) < 0x20)) {
          iVar3 = FUN_40104844(0x20,9,puVar2);
          if (iVar3 != 1) {
            return;
          }
          FUN_40100d9c(puVar2,PTR_s_wdev_c_40105fd4,0x7af);
          return;
        }
      }
      FUN_40100d9c(puVar2,PTR_s_wdev_c_40105fd4,0x7a9);
    }
  }
  return;
}

