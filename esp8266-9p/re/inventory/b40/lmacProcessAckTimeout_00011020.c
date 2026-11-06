
void lmacProcessAckTimeout(uint param_1)

{
  byte bVar1;
  byte bVar2;
  undefined *puVar3;
  int iVar4;
  uint uVar5;
  int *piVar6;
  undefined4 uVar7;
  uint uVar8;
  int *piVar9;
  undefined *puVar10;
  int iVar11;
  
  puVar3 = PTR_lmacConfMib_00011000;
  puVar10 = PTR_DAT_00010ff4;
  param_1 = param_1 & 0xff;
  iVar11 = param_1 * 8;
  piVar9 = (int *)(PTR_DAT_00010ff4 + param_1 * 0x24);
  iVar4 = *piVar9;
  if (iVar4 == 0) {
    (*(code *)PTR_ets_printf_00011008)(PTR_s__s__u_00010ffc,PTR_DAT_00010ff8,0x4ee);
    do {
                    /* WARNING: Do nothing block with infinite loop */
    } while( true );
  }
  if ((**(uint **)(iVar4 + 0x28) & 0x100) != 0) {
    bVar1 = *(byte *)((int)piVar9 + 7);
    *(undefined1 *)((int)piVar9 + 9) = 0;
    bVar2 = puVar3[0xc];
    *(byte *)((int)piVar9 + 6) = bVar1;
    if (*(byte *)((int)piVar9 + 10) < bVar2) {
      *(byte *)((int)piVar9 + 10) = *(byte *)((int)piVar9 + 10) + 1;
    }
    if (((byte)puVar10[param_1 * 0x24 + 10] < bVar2) && (bVar1 < (byte)puVar10[param_1 * 0x24 + 8]))
    {
      puVar10[param_1 * 0x24 + 6] = bVar1 + 1;
    }
    uVar8 = *(uint *)(*(int *)(iVar4 + 0x28) + 8);
    uVar5 = uVar8 & DAT_00011004;
    *(uint *)(*(int *)(iVar4 + 0x28) + 8) =
         uVar5 & 0xffffff0f | ((uVar8 >> 0xe & 0x3f) + 1 & 0x3f) << 0xe |
         (((uVar5 & 0xf0) >> 4) + 1 & 0xf) << 4;
    piVar6 = (int *)(puVar10 + param_1 * 0x24);
    if ((byte)(*(char *)((int)piVar6 + 0x11) - 1U) < 2) {
      iVar4 = (*(code *)PTR_rcReachRetryLimit_00011010)();
      if (((iVar4 == 0) &&
          ((*(uint *)(*(int *)(*piVar6 + 0x28) + 8) >> 0xe & 0x3f) < (uint)(byte)puVar3[0xc])) &&
         (iVar4 = (*(code *)PTR_lmacMSDUAged_00011014)(), iVar4 == 0)) {
        *(byte *)(*(int *)(*piVar6 + 0x14) + 1) = *(byte *)(*(int *)(*piVar6 + 0x14) + 1) | 8;
        uVar7 = 1;
      }
      else {
        puVar10[(iVar11 + param_1) * 4 + 0x11] = 6;
        uVar7 = 0;
      }
      (*(code *)PTR_FUN_00011018)(piVar9,uVar7);
      return;
    }
    (*(code *)PTR_ets_printf_0001100c)(PTR_s__s__u_00010ffc,PTR_DAT_00010ff8,0x4c4);
    do {
                    /* WARNING: Do nothing block with infinite loop */
    } while( true );
  }
  (*(code *)PTR_FUN_0001101c)(piVar9);
  return;
}

