
void FUN_0001031c(undefined4 *param_1)

{
  short sVar1;
  short sVar2;
  bool bVar3;
  uint uVar4;
  byte bVar5;
  short sVar6;
  undefined2 uVar7;
  uint uVar8;
  ushort uVar9;
  uint uVar10;
  int iVar11;
  uint *puVar12;
  uint uVar13;
  uint uVar14;
  uint uVar15;
  uint uVar16;
  uint *puVar17;
  
  puVar17 = (uint *)*param_1;
  uVar10 = *(uint *)(puVar17[10] + 8);
  uVar13 = uVar10 >> 0x17;
  if ((1 < (byte)(*(char *)((int)param_1 + 0x11) - 2U)) && (*(char *)((int)param_1 + 0x11) != '\0'))
  {
    (*(code *)PTR_ets_printf_00010310)(PTR_s__s__u_000102dc,PTR_DAT_000102d8,299);
    do {
                    /* WARNING: Do nothing block with infinite loop */
    } while( true );
  }
  uVar4 = puVar17[6];
  uVar15 = puVar17[5];
  uVar16 = (uint)*(ushort *)((int)puVar17 + 0x1a);
  uVar9 = (ushort)(uVar10 >> 0x17);
  if ((*(char *)((int)param_1 + 0xb) == '\0') && (*(char *)((int)param_1 + 0xd) == '\0')) {
    if (*(short *)(param_1 + 6) == 0) {
      bVar3 = true;
      if (0xf < *(byte *)(puVar17[10] + 0xc)) {
        *(ushort *)(uVar15 + 2) = uVar9;
      }
    }
    else {
      *(ushort *)(uVar15 + 2) = uVar9;
      uVar10 = *(uint *)puVar17[10];
      if (((uVar10 & 0x402) == 0) && ((uVar10 & DAT_000102e0) != DAT_000102c8)) {
        uVar7 = (*(code *)PTR_ppCalTxop_00010314)
                          (((uint *)puVar17[10])[1] >> 2 & 0xf,uVar13,*(undefined2 *)(param_1 + 6));
        *(undefined2 *)(uVar15 + 2) = uVar7;
      }
      if (uVar13 < *(ushort *)(uVar15 + 2)) {
        *(ushort *)(param_1 + 5) = *(ushort *)(uVar15 + 2);
        *(undefined1 *)(param_1 + 3) = 1;
        *(undefined2 *)((int)param_1 + 0x16) = 0;
        if ((*(uint *)puVar17[10] & 0x100) != 0) {
          (*(code *)PTR_RC_GetAckTime_00010318)((char)((uint *)puVar17[10])[3]);
        }
      }
      else {
        *(undefined2 *)(param_1 + 5) = 0;
      }
      bVar3 = true;
    }
  }
  else {
    sVar6 = *(short *)(param_1 + 5) - *(short *)((int)param_1 + 0x16);
    *(short *)(param_1 + 5) = sVar6;
    sVar2 = *(short *)(puVar17[10] + 0xe);
    sVar1 = *(short *)(puVar17[10] + 0xe);
    *(short *)((int)param_1 + 0x16) = sVar1;
    if ((int)uVar13 < (int)sVar6 - (int)sVar1) {
      *(short *)(uVar15 + 2) = sVar6 - sVar2;
    }
    else {
      *(ushort *)(uVar15 + 2) = uVar9;
      *(undefined1 *)(param_1 + 3) = 0;
    }
    bVar3 = false;
  }
  puVar12 = (uint *)puVar17[10];
  uVar13 = *puVar12;
  iVar11 = 0;
  if ((((uVar13 & 0x402) == 0) && ((uVar13 & DAT_000102e0) != DAT_000102c8)) &&
     (iVar11 = 1, (uVar13 & DAT_000102e4) != 0)) {
    iVar11 = 3;
  }
  if (*PTR_DAT_000102e8 != '\0') {
    if (*PTR_DAT_000102ec == '\0') {
      if (2 < (byte)((char)puVar12[3] - 1U)) goto LAB_00010474;
      bVar5 = (char)puVar12[3] + 4;
    }
    else {
      bVar5 = (char)puVar12[3] - 4;
      if (3 < bVar5) goto LAB_00010474;
    }
    *(byte *)(puVar12 + 3) = bVar5;
  }
LAB_00010474:
  memw();
  uVar13 = *(uint *)PTR_lmacConfMib_000102f4 - (*DAT_000102f0 - puVar12[5] >> 10);
  if (*(uint *)PTR_lmacConfMib_000102f4 < uVar13) {
    uVar13 = 10;
  }
  memw();
  *(uint *)((uint)*(byte *)(param_1 + 1) * -0x18 + DAT_000102f8) = (uVar13 & 0xfff) << 0xc;
  uVar13 = DAT_000102cc;
  uVar10 = *(uint *)puVar17[10];
  if (((uVar10 & 0x200) == 0) || (uVar14 = DAT_000102bc, !bVar3)) {
    uVar14 = DAT_000102c0;
  }
  if (((uVar10 & 0x100) == 0) || (uVar10 = DAT_000102c4, !bVar3)) {
    uVar10 = 0;
  }
  uVar8 = 0;
  if ((*(short *)(param_1 + 5) < 1) && ((byte)((uint *)puVar17[10])[3] < 0x10)) {
    uVar8 = DAT_000102c8;
  }
  memw();
  *(uint *)((uint)*(byte *)(param_1 + 1) * -0x18 + DAT_000102fc) =
       *puVar17 & DAT_00010300 | iVar11 << 0x18 | uVar14 | uVar10 | uVar8;
  memw();
  *(uint *)((uint)*(byte *)(param_1 + 1) * -0x18 + DAT_00010304) =
       (uint)*(ushort *)(uVar15 + 2) << 0x10;
  uVar10 = (uint)*(byte *)(puVar17[10] + 0xc);
  if (uVar10 < 0x10) {
    uVar13 = 0;
  }
  uVar16 = uVar16 + (ushort)uVar4;
  memw();
  *(uint *)((uint)*(byte *)(param_1 + 1) * -0x18 + DAT_00010308) =
       (uVar10 & 0xf) << 0xc | uVar16 & 0xfff | (*(uint *)(puVar17[10] + 4) >> 6 & 0xff) << 0x10 |
       uVar13;
  uVar13 = (uint)*(byte *)(puVar17[10] + 0xc);
  if (0xf < uVar13) {
    uVar10 = DAT_000102d0;
    if (uVar13 < 0x18) {
      uVar10 = DAT_000102d4;
    }
    memw();
    *(uint *)((uint)*(byte *)(param_1 + 1) * -0x18 + DAT_0001030c) =
         uVar13 & 7 | (uVar16 & 0xffff) << 8 | uVar10;
  }
  return;
}

