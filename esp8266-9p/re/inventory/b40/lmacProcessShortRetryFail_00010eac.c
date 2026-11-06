
void FUN_00010eac(int param_1,int param_2,int param_3)

{
  char cVar1;
  int iVar2;
  undefined4 uVar3;
  uint uVar4;
  uint uVar5;
  undefined *puVar6;
  
  puVar6 = PTR_lmacConfMib_00010e90;
  if (param_2 == 0) {
    (*(code *)PTR_ets_printf_00010e98)(PTR_s__s__u_00010e8c,PTR_DAT_00010e88,0x442);
    do {
                    /* WARNING: Do nothing block with infinite loop */
    } while( true );
  }
  if (*(byte *)(param_1 + 9) < (byte)PTR_lmacConfMib_00010e90[0xd]) {
    *(byte *)(param_1 + 9) = *(byte *)(param_1 + 9) + 1;
  }
  if (*(byte *)(param_1 + 9) < (byte)puVar6[0xd]) {
    if (*(byte *)(param_1 + 8) <= *(byte *)(param_1 + 6)) goto LAB_00010efe;
    cVar1 = *(byte *)(param_1 + 6) + 1;
  }
  else {
    cVar1 = *(char *)(param_1 + 7);
  }
  *(char *)(param_1 + 6) = cVar1;
LAB_00010efe:
  iVar2 = *(int *)(param_2 + 0x28);
  uVar4 = (*(uint *)(iVar2 + 8) >> 8 & 0x3f) + 1 & 0x3f;
  uVar5 = *(uint *)(iVar2 + 8) & DAT_00010e94;
  *(uint *)(iVar2 + 8) = uVar5 | uVar4 << 8;
  if ((param_3 == 0) || ((uVar5 & 0xf0) >> 4 < uVar4)) {
    *(uint *)(iVar2 + 8) =
         *(uint *)(iVar2 + 8) & 0xffffff0f | ((*(uint *)(iVar2 + 8) >> 4 & 0xf) + 1 & 0xf) << 4;
  }
  if (1 < (byte)(*(char *)(param_1 + 0x11) - 1U)) {
    (*(code *)PTR_ets_printf_00010e9c)(PTR_s__s__u_00010e8c,PTR_DAT_00010e88,0x45b);
    do {
                    /* WARNING: Do nothing block with infinite loop */
    } while( true );
  }
  iVar2 = (*(code *)PTR_rcReachRetryLimit_00010ea0)();
  if (((iVar2 == 0) &&
      ((*(uint *)(*(int *)(param_2 + 0x28) + 8) >> 8 & 0x3f) < (uint)(byte)puVar6[0xd])) &&
     (iVar2 = (*(code *)PTR_lmacMSDUAged_00010ea4)(param_2), iVar2 == 0)) {
    *(byte *)(*(int *)(param_2 + 0x14) + 1) = *(byte *)(*(int *)(param_2 + 0x14) + 1) | 8;
    uVar3 = 1;
  }
  else {
    *(undefined1 *)(param_1 + 0x11) = 6;
    uVar3 = 0;
  }
  (*(code *)PTR_FUN_00010ea8)(param_1,uVar3);
  return;
}

