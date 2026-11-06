
/* B4: SDK fingerprint start 0x40221a6c is an 8-byte literal pool; code starts 0x40221a74
   (frame-control parsing). */

uint hostap_input(int *param_1,int param_2,int param_3,undefined4 param_4)

{
  byte bVar1;
  byte bVar2;
  ushort uVar3;
  bool bVar4;
  uint uVar5;
  uint uVar6;
  uint uVar7;
  int iVar8;
  byte *pbVar9;
  int iVar10;
  undefined4 uVar11;
  int iVar12;
  int iVar13;
  uint *puVar14;
  uint uVar15;
  byte *pbVar16;
  byte *pbVar17;
  uint uVar18;
  byte *pbStack_20;
  int iStack_1c;
  
  puVar14 = *(uint **)(param_2 + 0x10);
  iVar8 = *param_1;
  if ((*puVar14 >> 0xe & 3) == 0) {
    uVar5 = *puVar14 >> 0x10 & 0xfff;
  }
  else {
    uVar5 = puVar14[1] >> 8;
  }
  uVar6 = uVar5 & 0xffff;
  if (uVar6 < 0x10) {
    uVar18 = 0xff;
    goto LAB_40221af3;
  }
  pbVar17 = *(byte **)(*(int *)(param_2 + 4) + 4);
  bVar1 = *pbVar17;
  uVar18 = 0xff;
  if ((bVar1 & 3) != 0) goto LAB_40221af3;
  bVar2 = pbVar17[1];
  uVar18 = bVar1 & 0xc;
  uVar15 = bVar1 & 0xf0;
  if (((uVar18 == 8) && ((bVar2 & 3) == 3)) && ((pbVar17[4] & 1) != 0)) {
    FUN_402268a4(pbVar17,uVar6 - 4,param_3 + -0x60);
    goto LAB_40221af3;
  }
  if (((bVar2 & 3) == 0) && (uVar18 != 4)) {
    if (uVar6 < 0x18) goto LAB_40221af3;
    pbStack_20 = pbVar17 + 0x10;
  }
  else {
    pbStack_20 = pbVar17 + 4;
  }
  pbVar9 = pbVar17 + 10;
  iStack_1c = FUN_4022a0dc();
  if (iStack_1c == 0) {
    if (uVar18 == 8) {
      iVar8 = FUN_4022a0dc(PTR_DAT_40204cd4);
      if (iVar8 != 0) {
        param_1[0x24] = iVar8;
        ieee80211_send_deauth(param_1,pbVar9,6);
      }
      goto LAB_40221af3;
    }
    if (((((char)puVar14[2] != '\0') && ((char)puVar14[2] != -0xb)) ||
        ((iVar10 = (*DAT_402034f8)(pbStack_20,PTR_DAT_40204cd4,6), iVar10 != 0 &&
         (iVar10 = (*DAT_402034f8)(pbStack_20,iVar8 + 0x33,6), iVar10 != 0)))) ||
       (iStack_1c = FUN_4022a0dc(PTR_DAT_40204cd4), iStack_1c == 0)) goto LAB_40221af3;
    bVar4 = false;
  }
  else {
    if (((char)puVar14[2] != '\0') && ((char)puVar14[2] != -0xb)) goto LAB_40221af3;
    memw();
    *(undefined4 *)(iStack_1c + 0x104) = *(undefined4 *)(DAT_402012d8 + 0x24);
    if ((uVar18 != 0) || (uVar15 != 0xb0)) {
      *(undefined1 *)(iStack_1c + 0xea) = 0;
    }
    bVar4 = true;
  }
  param_1[0x24] = iStack_1c;
  if ((((uVar18 != 0) || (uVar15 != 0x40)) &&
      (iVar10 = (*DAT_402034f8)(pbStack_20,iVar8 + 0x33,6), iVar10 != 0)) &&
     (iVar10 = (*DAT_402034f8)(pbStack_20,PTR_DAT_40204cd4,6), iVar10 != 0)) goto LAB_40221af3;
  *(char *)(param_1 + 0x30) = (char)param_4;
  if (((uVar18 >> 2 & 1) == 0) && (uVar15 != 0x40)) {
    if ((*pbVar17 & 0x8c) == 0x88) {
      if ((pbVar17[1] & 3) == 3) {
        bVar1 = pbVar17[0x1e];
      }
      else {
        bVar1 = pbVar17[0x18];
      }
      uVar7 = bVar1 & 0xf;
    }
    else {
      uVar7 = 0x10;
    }
    uVar3 = *(ushort *)(pbVar17 + 0x16);
    if (uVar18 == 0) {
      if (uVar15 == 0xd0) {
        if ((pbVar17[1] >> 6 & 1) == 0) {
          pbVar16 = pbVar17 + 0x18;
        }
        else {
          pbVar16 = pbVar17 + 0x20;
        }
        if ((int)uVar6 < (int)(pbVar16 + (8 - (int)pbVar17))) goto LAB_40221af3;
        if ((*pbVar16 == 0x7f) &&
           (iVar10 = (*DAT_402034f8)(pbVar16 + 1,PTR_DAT_40221a68,3), iVar10 == 0))
        goto LAB_40221c98;
      }
      else if (((uVar15 == 0xb0) && (param_1[0x26] != iStack_1c)) &&
              (*(char *)(iStack_1c + 0xea) == '\0')) {
        bVar4 = false;
      }
    }
    if (bVar4) {
      iVar10 = uVar7 * 2 + iStack_1c;
      if (((pbVar17[1] & 8) != 0) && ((int)((uint)uVar3 - (uint)*(ushort *)(iVar10 + 0x9e)) < 1))
      goto LAB_40221af3;
    }
    else {
      iVar10 = uVar7 * 2 + iStack_1c;
    }
    *(ushort *)(iVar10 + 0x9e) = uVar3;
  }
LAB_40221c98:
  (*DAT_40201e88)(param_1 + 0x12,pbVar9,6);
  if (uVar18 == 0) {
    uVar11 = (*(code *)PTR_FUN_40221a70)(pbVar17);
    *(short *)(param_2 + 0x14) = (short)uVar11;
    *(short *)(param_2 + 0x16) = ((short)uVar5 - (short)uVar11) + -4;
    if ((((bVar2 & 3) == 0) && (0x17 < uVar6)) && ((pbVar17[10] & 1) == 0)) {
      if (((pbVar17[1] & 0x40) != 0) && (uVar15 != 0xd0)) {
        if ((uVar15 != 0xb0) ||
           (((param_1[0x14] & 0x10U) == 0 ||
            (iVar8 = FUN_402211e8(param_1,param_2,uVar11), iVar8 == 0)))) goto LAB_40221af3;
        iVar8 = *(int *)(*(int *)(param_2 + 4) + 4);
        *(byte *)(iVar8 + 1) = *(byte *)(iVar8 + 1) & 0xbf;
      }
      FUN_40222040(param_1,param_2,uVar15,param_3,param_4);
    }
  }
  else if (uVar18 == 4) {
    FUN_40222678(param_1,param_2,uVar15);
  }
  else if (uVar18 == 8) {
    iVar10 = (*(code *)PTR_FUN_40221a70)(pbVar17);
    *(short *)(param_2 + 0x14) = (short)iVar10;
    *(short *)(param_2 + 0x16) = ((short)uVar5 - (short)iVar10) + -4;
    if ((iVar10 <= (int)uVar6) && ((bVar2 & 3) == 1)) {
      if (*(short *)(iStack_1c + 0x1a) == 0) {
        ieee80211_send_mgmt(param_1,0xa0,7);
      }
      else {
        uVar5 = (uint)pbVar17[1];
        if ((uVar5 & 0x10) != (*(uint *)(iStack_1c + 8) & 0x10)) {
          FUN_4022651c(iStack_1c);
          uVar5 = (uint)pbVar17[1];
        }
        if ((uVar5 & 0x40) == 0) {
          *(uint *)(*(int *)(param_2 + 4) + 4) =
               (uint)*(ushort *)(param_2 + 0x14) + *(int *)(*(int *)(param_2 + 4) + 4);
        }
        else {
          if (((param_1[0x14] & 0x10U) == 0) ||
             (iVar12 = FUN_402211e8(param_1,param_2,iVar10), iVar12 == 0)) goto LAB_40221af3;
          pbVar17[1] = pbVar17[1] & 0xbf;
        }
        iVar10 = FUN_40223748(param_2,pbVar17,uVar6,iVar10);
        param_2 = iVar10;
        if (iVar10 != 0) {
          pbVar17 = *(byte **)(*(int *)(iVar10 + 4) + 4);
          if ((*(uint *)(iStack_1c + 8) & 1) == 0) {
            if (*(char *)(iStack_1c + 0x18) == '\x01') {
              if (*(ushort *)(pbVar17 + 0xc) == DAT_40221a6c) {
                FUN_40237594(*(undefined4 *)(param_1[0x33] + 0x18),
                             *(undefined4 *)(param_1[0x24] + 0xe4),pbVar17 + 0xe,
                             *(ushort *)(iVar10 + 0x16) - 0xe);
                param_2 = iVar10;
              }
              goto LAB_40221af3;
            }
          }
          else if ((*(ushort *)(pbVar17 + 0xc) == DAT_40221a6c) && (param_1[0x33] != 0)) {
            FUN_40237594(*(undefined4 *)(param_1[0x33] + 0x18),*(undefined4 *)(param_1[0x24] + 0xe4)
                         ,pbVar17 + 0xe,*(ushort *)(iVar10 + 0x16) - 0xe);
            param_2 = iVar10;
            goto LAB_40221af3;
          }
          if ((((*pbVar17 & 1) != 0) ||
              (iVar12 = (*DAT_402034f8)(pbVar17,iVar8 + 0x33,6), iVar12 != 0)) &&
             (iVar12 = FUN_40232378(3,*(undefined2 *)(iVar10 + 0x16),0), iVar12 != 0)) {
            (*DAT_40201e88)(*(undefined4 *)(iVar12 + 4),*(undefined4 *)(*(int *)(iVar10 + 4) + 4),
                            *(undefined2 *)(iVar10 + 0x16));
            *(short *)(iVar12 + 0xe) = *(short *)(iVar12 + 0xe) + -1;
            iVar13 = FUN_40224480(iVar8);
            if (iVar13 == 1) {
              *(short *)(iVar12 + 0xe) = *(short *)(iVar12 + 0xe) + 1;
              FUN_4023258c();
            }
          }
          if (((*pbVar17 & 1) != 0) || (iVar8 = (*DAT_402034f8)(pbVar17,iVar8 + 0x33,6), iVar8 == 0)
             ) {
            FUN_402219a0(param_1,iVar10);
            return 8;
          }
        }
      }
    }
  }
LAB_40221af3:
  if (param_2 != 0) {
    FUN_4021e7c8(param_2);
  }
  return uVar18;
}

