
void rcGetSched(int param_1,uint *param_2)

{
  bool bVar1;
  byte bVar2;
  undefined4 uVar3;
  undefined1 uVar4;
  uint uVar5;
  uint uVar6;
  undefined *puVar7;
  int iVar8;
  uint uVar9;
  uint uVar10;
  undefined1 *puVar11;
  uint uVar12;
  uint uVar13;
  
  puVar7 = PTR_DAT_000107f8;
  if ((param_1 == 0) || (puVar11 = *(undefined1 **)(param_1 + 0x34), puVar11 == (undefined1 *)0x0))
  {
    puVar7 = PTR_DAT_000107e8;
    if (((*param_2 & DAT_000107ec) == 0) &&
       ((puVar7 = PTR_DAT_000107e4, param_1 != 0 &&
        (puVar7 = PTR_DAT_000107e8, *(int *)(param_1 + 0x30) != 0)))) {
      puVar7 = PTR_DAT_000107e4;
    }
    goto LAB_00010a79;
  }
  uVar5 = *param_2;
  if ((uVar5 & 2) == 0) {
    if ((uVar5 & 8) != 0) {
      memw();
      uVar13 = *DAT_000107f0;
      uVar9 = uVar13 - *(uint *)(param_1 + 0x24);
      uVar5 = uVar9 - 1;
      if (*(uint *)(param_1 + 0x24) <= uVar13) {
        uVar5 = uVar9;
      }
      if ((*(int *)(param_1 + 0x14) != 0) &&
         ((99 < *(uint *)(param_1 + 0x28) || (DAT_000107f4 < uVar5)))) {
        *(uint *)(param_1 + 0x24) = uVar13;
        uVar9 = *(int *)puVar7 + 1;
        *(uint *)puVar7 = uVar9;
        uVar5 = (*(code *)PTR___udivsi3_00010828)(*(int *)(param_1 + 0x18) << 7);
        *(char *)(param_1 + 10) = (char)uVar5;
        if (*(byte *)(param_1 + 0xb) == 0) {
          uVar9 = (int)((uint)(byte)puVar11[9] * 3 + 0x80) >> 2;
          if ((uVar9 & 0xff) < (uVar5 & 0xff)) {
            *(char *)(param_1 + 0xb) = (char)((int)((uVar5 & 0xff) + uVar9) >> 1);
          }
          else {
            *(char *)(param_1 + 0xb) = (char)uVar9;
          }
        }
        else {
          uVar10 = (int)((uVar5 & 0xff) + (uint)*(byte *)(param_1 + 0xb)) >> 1;
          uVar6 = uVar13 - *(uint *)(param_1 + 0x20);
          *(char *)(param_1 + 0xb) = (char)uVar10;
          uVar5 = uVar6 - 1;
          if (*(uint *)(param_1 + 0x20) <= uVar13) {
            uVar5 = uVar6;
          }
          uVar6 = (*(code *)PTR_FUN_0001082c)(*(undefined1 *)(param_1 + 1),*puVar11);
          uVar3 = DAT_00010800;
          uVar6 = uVar6 >> 1 & 0xff;
          uVar12 = (uint)(byte)puVar11[9];
          iVar8 = 0x5a;
          if (uVar12 == 0) {
LAB_00010920:
            bVar1 = iVar8 - uVar6 < uVar10;
          }
          else {
            if (uVar12 < 0x41) {
              iVar8 = 100;
              goto LAB_00010920;
            }
            bVar1 = false;
            if (0x6e - uVar6 < uVar10) {
              bVar1 = (uVar12 + 2) - uVar6 < uVar10;
            }
          }
          if ((bVar1) && (*(uint *)(param_1 + 0x2c) < uVar5)) {
            puVar11[0xb] = 0;
            *(undefined4 *)(param_1 + 0x2c) = uVar3;
            *(undefined4 *)(param_1 + 0x18) = 0;
            *(undefined4 *)(param_1 + 0x14) = 0;
            *(undefined4 *)(param_1 + 0x28) = 0;
            *(undefined1 *)(param_1 + 10) = 0;
            puVar7 = PTR_g_ic_00010820;
            *(undefined1 *)(param_1 + 0xb) = 0;
            *(undefined1 *)(param_1 + 6) = 0;
            uVar5 = (uint)*(byte *)(param_1 + 0x45);
            if (((uint)(byte)puVar7[0x165] & 1 << (*(byte *)(param_1 + 0x45) & 0x1f)) == 0) {
              bVar2 = *(byte *)(param_1 + 4);
            }
            else {
              iVar8 = (*(code *)PTR_phytype2mode_00010830)(*(undefined1 *)(param_1 + 0x30));
              bVar2 = PTR_DAT_00010804[(iVar8 * 2 + uVar5) * 2];
            }
            iVar8 = *(int *)(param_1 + 0x34);
            if (bVar2 < *(byte *)(iVar8 + 10)) {
              *(int *)(param_1 + 0x34) = iVar8 + -0xc;
              *(byte *)(iVar8 + -1) = *(byte *)(iVar8 + -1) | 1;
              memw();
              *(uint *)(param_1 + 0x20) = *DAT_000107f0;
            }
          }
          else {
            bVar2 = puVar11[0xb];
            if (((bVar2 & 1) != 0) || ((uVar9 & 3) == 0)) {
              if ((int)uVar10 < (int)(uVar12 - uVar6)) {
                if (((bVar2 & 1) != 0) && (*(uint *)(param_1 + 0x2c) <= DAT_000107fc)) {
                  *(uint *)(param_1 + 0x2c) = *(uint *)(param_1 + 0x2c) << 1;
                }
                uVar5 = uVar13 - *(uint *)(param_1 + 0x1c);
                if (uVar13 < *(uint *)(param_1 + 0x1c)) {
                  uVar5 = uVar5 - 1;
                }
                if (DAT_00010808 < uVar5) {
                  (*(code *)PTR_FUN_00010834)(param_1);
                }
              }
              else if ((bVar2 & 1) != 0) {
                puVar11[0xb] = bVar2 & 0xfe;
                *(undefined4 *)(param_1 + 0x2c) = DAT_00010800;
              }
            }
          }
        }
        uVar5 = (uint)*(byte *)(*(int *)(param_1 + 0x34) + 10);
        iVar8 = (*(code *)PTR_phytype2mode_00010838)(*(undefined1 *)(param_1 + 0x30));
        uVar9 = (uint)(byte)PTR_DAT_00010804[(iVar8 * 2 + (uint)*(byte *)(param_1 + 0x45)) * 2];
        if (uVar9 <= uVar5) {
          uVar9 = (uint)(byte)(PTR_DAT_00010804 + (iVar8 * 2 + (uint)*(byte *)(param_1 + 0x45)) * 2)
                              [1];
          if (uVar9 < uVar5) {
            uVar5 = uVar9;
          }
          uVar9 = uVar5 & 0xff;
        }
        *(uint *)(param_1 + 0x34) = uVar9 * 0xc + *(int *)(param_1 + 0x3c);
        *(undefined4 *)(param_1 + 0x18) = 0;
        *(undefined4 *)(param_1 + 0x14) = 0;
        *(undefined4 *)(param_1 + 0x28) = 0;
      }
      *param_2 = *param_2 | DAT_0001080c;
      puVar7 = *(undefined **)(param_1 + 0x34);
      goto LAB_00010a79;
    }
    if (((uVar5 & DAT_00010810) != 0) && (puVar7 = PTR_DAT_00010814, *(int *)(param_1 + 0x30) != 0))
    goto LAB_00010a79;
  }
  puVar7 = *(undefined **)(param_1 + 0x38);
LAB_00010a79:
  param_2[6] = (uint)puVar7;
  if (param_2[6] == 0) {
    (*(code *)PTR_esp_log_write_0001083c)(1,PTR_DAT_0001081c,PTR_DAT_00010818);
  }
  if (((param_1 == 0) || ((*(uint *)(param_1 + 8) & 0x40) == 0)) || ((*param_2 & 8) == 0)) {
    if (((uint)(byte)PTR_g_ic_00010820[0x161] & 1 << ((byte)(param_2[1] >> 1) & 1)) == 0) {
      uVar4 = *(undefined1 *)param_2[6];
    }
    else {
      uVar4 = PTR_g_ic_00010820[0x162];
    }
  }
  else {
    uVar4 = *(undefined1 *)(param_1 + 7);
  }
  *(undefined1 *)(param_2 + 3) = uVar4;
  if ((param_2[1] & 2) == 0) {
    uVar4 = *PTR_sig_max_rate2_00010824;
    iVar8 = (*(code *)PTR_should_update_rate_00010840)((char)param_2[3],uVar4);
    if (iVar8 != 0) {
      *(undefined1 *)(param_2 + 3) = uVar4;
    }
  }
  return;
}

