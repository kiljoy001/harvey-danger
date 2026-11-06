
void lmacProcessTxSuccess(uint param_1,undefined1 param_2)

{
  undefined *puVar1;
  int *piVar2;
  uint uVar3;
  int iVar4;
  
  puVar1 = PTR_DAT_00010dcc;
  param_1 = param_1 & 0xff;
  piVar2 = (int *)(PTR_DAT_00010dcc + param_1 * 0x24);
  if (*(char *)((int)piVar2 + 0x11) != '\x02') {
    (*(code *)PTR_ets_printf_00010de0)(PTR_s__s__u_00010dd4,PTR_DAT_00010dd0,0x2fd);
    do {
                    /* WARNING: Do nothing block with infinite loop */
    } while( true );
  }
  iVar4 = *piVar2;
  uVar3 = **(uint **)(iVar4 + 0x28);
  if (((uVar3 & 0x402) == 0) && ((uVar3 & DAT_00010dd8) != DAT_00010ddc)) {
    *(undefined1 *)((int)piVar2 + 6) = *(undefined1 *)((int)piVar2 + 7);
    if ((uVar3 & 0x100) == 0) {
      *(undefined1 *)((int)piVar2 + 9) = 0;
    }
    else {
      *(undefined1 *)((int)piVar2 + 9) = 0;
      *(undefined1 *)((int)piVar2 + 10) = 0;
    }
    iVar4 = *(int *)(iVar4 + 0x28);
  }
  else {
    PTR_DAT_00010dcc[param_1 * 0x24 + 6] = PTR_DAT_00010dcc[param_1 * 0x24 + 7];
    puVar1[param_1 * 0x24 + 9] = 0;
    puVar1[param_1 * 0x24 + 10] = 0;
    iVar4 = *(int *)(iVar4 + 0x28);
    param_2 = 0x7f;
  }
  *(undefined1 *)(iVar4 + 0xd) = param_2;
  (puVar1 + param_1 * 0x24)[0x11] = 5;
  (*(code *)PTR_FUN_00010de4)(puVar1 + param_1 * 0x24,0);
  return;
}

