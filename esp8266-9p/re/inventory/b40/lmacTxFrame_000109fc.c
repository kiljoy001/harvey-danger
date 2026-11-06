
void lmacTxFrame(int param_1,uint param_2)

{
  char cVar1;
  undefined2 uVar2;
  uint uVar3;
  uint *puVar4;
  int iVar5;
  undefined *puVar6;
  undefined *puVar7;
  int iVar8;
  
  puVar6 = PTR_DAT_000109c4;
  param_2 = param_2 & 0xff;
  iVar8 = param_2 * 8;
  cVar1 = PTR_DAT_000109c4[param_2 * 0x24 + 0x11];
  if ((1 < (byte)(cVar1 - 3U)) && (cVar1 != '\0')) {
    (*(code *)PTR_ets_printf_000109e4)(PTR_s__s__u_000109cc,PTR_DAT_000109c8,0x60b);
    do {
                    /* WARNING: Do nothing block with infinite loop */
    } while( true );
  }
  if (cVar1 == '\x04') {
    if (*(int *)(PTR_DAT_000109c4 + param_2 * 0x24) != param_1) {
      (*(code *)PTR_ets_printf_000109e8)(PTR_s__s__u_000109cc,PTR_DAT_000109c8,0x60e);
      do {
                    /* WARNING: Do nothing block with infinite loop */
      } while( true );
    }
  }
  else {
    *(int *)(PTR_DAT_000109c4 + param_2 * 0x24) = param_1;
    if (param_1 == 0) {
      (*(code *)PTR_ets_printf_000109ec)(PTR_s__s__u_000109cc,PTR_DAT_000109c8,0x614);
      do {
                    /* WARNING: Do nothing block with infinite loop */
      } while( true );
    }
    if ((uint)*(ushort *)(PTR_lmacConfMib_000109d0 + 0xe) <
        (uint)*(ushort *)(param_1 + 0x1a) + (uint)*(ushort *)(param_1 + 0x18)) {
      uVar3 = **(uint **)(param_1 + 0x28);
      if ((uVar3 & 2) == 0) {
        **(uint **)(param_1 + 0x28) = uVar3 & DAT_000109d4 | 0x100;
      }
    }
    puVar4 = *(uint **)(param_1 + 0x28);
    if ((((*puVar4 & 0x1000) != 0) && (puVar6[param_2 * 0x24 + 0x11] == '\x03')) &&
       (uVar3 = puVar4[2], 2 < (uVar3 >> 4 & 0xf))) {
      *puVar4 = *puVar4 & DAT_000109d4 | 0x100;
      puVar4[2] = (uVar3 & DAT_000109d8 | (uVar3 >> 8 & 0x3f) << 0xe) & DAT_000109dc;
    }
    (*(code *)PTR_FUN_000109f0)(puVar6 + param_2 * 0x24);
  }
  iVar5 = iVar8 + param_2;
  memw();
  puVar7 = puVar6;
  uVar2 = (*(code *)PTR___udivsi3_000109f4)
                    (((1 << 0x20 - (0x20 - ((byte)puVar6[iVar5 * 4 + 6] & 0x1f))) - 1U &
                     *DAT_000109e0) * 0x183,100);
  puVar6[iVar5 * 4 + 0x11] = 1;
  if (((((*(uint **)(param_1 + 0x28))[1] & 2) != 0) && (**(char **)(param_1 + 0xc) == '\b')) &&
     ((**(uint **)(param_1 + 0x28) & 2) != 0)) {
    uVar2 = 0x7d;
  }
  (*(code *)PTR_wDev_EnableTransmit_000109f8)
            (puVar7[(iVar8 + param_2) * 4 + 4],puVar7[(iVar8 + param_2) * 4 + 5],uVar2);
  return;
}

