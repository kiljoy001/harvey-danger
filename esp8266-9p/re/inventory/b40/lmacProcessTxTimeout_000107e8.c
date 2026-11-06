
void lmacProcessTxTimeout(void)

{
  undefined *puVar1;
  uint *puVar2;
  uint *puVar3;
  int iVar4;
  uint uVar5;
  uint uVar6;
  
  memw();
  memw();
  uVar6 = *DAT_000107a8 >> 0x10 & 0xfff;
  memw();
  *DAT_000107a8 = *DAT_000107a8 & DAT_000107ac;
  puVar3 = DAT_000107b0;
  if (uVar6 != 0) {
    while( true ) {
      iVar4 = -LZCOUNT(-uVar6 & uVar6);
      uVar5 = iVar4 + 0x1f;
      if (uVar5 == 0xffffffff) break;
      (*(code *)PTR_vPortETSIntrLock_000107d0)();
      memw();
      memw();
      *puVar3 = *puVar3 | DAT_000107b4;
      (*(code *)PTR_vPortETSIntrUnlock_000107d4)();
      (*(code *)PTR_ets_delay_us_000107d8)(0x22);
      (*(code *)PTR_vPortETSIntrLock_000107dc)();
      puVar2 = (uint *)(uVar5 * -0x18 + DAT_000107b8);
      memw();
      memw();
      *puVar2 = *puVar2 & DAT_000107bc;
      memw();
      memw();
      *puVar3 = *puVar3 & DAT_000107c0;
      puVar1 = PTR_DAT_000107c4;
      *(int *)(PTR_DAT_000107c4 + uVar5 * 0x24 + 0x20) =
           *(int *)(PTR_DAT_000107c4 + uVar5 * 0x24 + 0x20) + 1;
      if (((puVar1[uVar5 * 0x24 + 0x11] == '\x01') && (iVar4 == -0x18)) ||
         (memw(), (int)*puVar2 < 0)) {
        memw();
        memw();
        *puVar2 = *puVar2 & DAT_000107c8;
        *(undefined1 *)((int)(puVar1 + uVar5 * 0x24) + 0x11) = 6;
        if (*(int *)(puVar1 + uVar5 * 0x24) != 0) {
          (*(code *)PTR_FUN_000107e0)();
        }
      }
      else if (*(int *)(puVar1 + uVar5 * 0x24) != 0) {
        puVar2 = *(uint **)(*(int *)(puVar1 + uVar5 * 0x24) + 0x28);
        *puVar2 = *puVar2 | DAT_000107cc;
      }
      (*(code *)PTR_vPortETSIntrUnlock_000107e4)();
      uVar6 = uVar6 & (uint)(0xfffffffefffffffe >> 0x20 - (uVar5 & 0x1f));
    }
  }
  return;
}

