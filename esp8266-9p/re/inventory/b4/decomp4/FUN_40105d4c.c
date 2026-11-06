
/* B3 SDK fingerprint: 8/19 windows, object wdev.o. */

void wDev_AppendRxBlocks(uint *param_1,undefined4 param_2,uint param_3)

{
  ushort uVar1;
  uint uVar2;
  uint uVar3;
  uint uVar4;
  int iVar5;
  uint uVar6;
  uint uVar7;
  uint uVar8;
  ushort *puVar9;
  uint *puVar10;
  ushort uVar11;
  
  uVar7 = DAT_40105d44;
  uVar6 = DAT_401022c0;
  uVar4 = DAT_401011bc;
  uVar3 = DAT_401011ac;
  uVar2 = DAT_40100f5c;
  if (param_1 == (uint *)0x0) {
    uVar8 = 0;
  }
  else {
    uVar8 = 0;
    puVar10 = param_1;
    do {
      memw();
      memw();
      *puVar10 = *puVar10 & uVar6 | uVar3;
      memw();
      memw();
      *puVar10 = *puVar10 & uVar2;
      memw();
      memw();
      *puVar10 = *puVar10 & uVar4;
      memw();
      memw();
      memw();
      *puVar10 = *puVar10 & uVar7 | (*puVar10 & 0xfff) << 0xc;
      puVar10 = (uint *)puVar10[2];
      uVar8 = uVar8 + 1 & 0xffff;
    } while (puVar10 != (uint *)0x0);
  }
  if (param_3 == uVar8) {
    (*DAT_40100430)();
    uVar11 = (ushort)param_3;
    puVar9 = *(ushort **)PTR_DAT_401053f8;
    uVar1 = *puVar9;
    if (uVar1 == 0) {
      *(uint **)(puVar9 + 4) = param_1;
      memw();
      uVar1 = *puVar9;
      *(uint **)(DAT_40102780 + 0x208) = param_1;
      *puVar9 = uVar1 + uVar11;
      *(undefined4 *)(puVar9 + 6) = param_2;
      if (*puVar9 == 1) {
        *(ushort **)(*(int *)(puVar9 + 6) + 8) = puVar9 + 0xc;
      }
    }
    else if (uVar1 == 1) {
      if (*(int *)(puVar9 + 8) == 0) {
        *(uint **)(puVar9 + 8) = param_1;
      }
      else {
        uVar1 = puVar9[1];
        *(uint **)(*(int *)(puVar9 + 10) + 8) = param_1;
        uVar11 = uVar1 + uVar11;
      }
      *(undefined4 *)(puVar9 + 10) = param_2;
      puVar9[1] = uVar11;
    }
    else {
      *(uint **)(*(int *)(puVar9 + 6) + 8) = param_1;
      *(undefined4 *)(puVar9 + 6) = param_2;
      *puVar9 = uVar1 + uVar11;
    }
    uVar2 = DAT_40102804;
    iVar5 = DAT_40101abc;
    if (1 < (uint)*puVar9 + (uint)puVar9[1]) {
      memw();
      memw();
      *(uint *)(DAT_40101abc + 0x2a8) =
           *(uint *)(DAT_40101abc + 0x2a8) & DAT_40102804 | DAT_40100f54;
      memw();
      memw();
      *(uint *)(iVar5 + 0x2ac) = *(uint *)(iVar5 + 0x2ac) & uVar2 | DAT_40105d48;
    }
    (*DAT_401009a0)();
    return;
  }
  (*DAT_40100180)(PTR_s__s__u_4010556c,PTR_DAT_40105570,0x5d2);
  do {
                    /* WARNING: Do nothing block with infinite loop */
  } while( true );
}

