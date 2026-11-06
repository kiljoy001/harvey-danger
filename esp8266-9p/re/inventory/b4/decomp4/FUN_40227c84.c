
/* B3 SDK fingerprint: 24/55 windows, object ieee80211_sta.o. */

uint sta_input(int param_1,int param_2,undefined4 param_3,undefined4 param_4)

{
  byte bVar1;
  byte bVar2;
  short sVar3;
  byte *pbVar4;
  uint uVar5;
  uint uVar6;
  int iVar7;
  uint uVar8;
  byte bVar9;
  int iVar10;
  code *pcVar11;
  byte *pbVar12;
  uint uVar13;
  int iVar14;
  uint in_a14;
  int iVar15;
  
  uVar5 = **(uint **)(param_2 + 0x10);
  if ((uVar5 >> 0xe & 3) == 0) {
    uVar5 = uVar5 >> 0x10 & 0xfff;
  }
  else {
    uVar5 = (*(uint **)(param_2 + 0x10))[1] >> 8;
  }
  if ((*(char *)(DAT_40201728 + 0x6a) == '\x01') || (param_1 == 0)) goto LAB_40227d53;
  in_a14 = 0xff;
  iVar15 = *(int *)(param_1 + 0x90);
  uVar6 = uVar5 & 0xffff;
  if (uVar6 < 0x10) goto LAB_40227d53;
  pbVar4 = *(byte **)(*(int *)(param_2 + 4) + 4);
  bVar9 = *pbVar4;
  if ((bVar9 & 3) != 0) goto LAB_40227d53;
  bVar1 = pbVar4[1];
  in_a14 = bVar9 & 0xc;
  uVar13 = bVar9 & 0xf0;
  if ((((iVar15 != 0) && ((*(byte *)(DAT_4021eaa0 + 0x46) & 1) == 0)) &&
      (iVar7 = (*DAT_402034f8)(pbVar4 + 10,iVar15,6), iVar7 != 0)) &&
     ((in_a14 != 0 || (uVar13 != 0xd0)))) goto LAB_40227d53;
  *(char *)(param_1 + 0xc0) = (char)param_4;
  if (((in_a14 & 4) == 0) && ((pbVar4[4] & 1) == 0)) {
    bVar9 = pbVar4[1];
    if ((*pbVar4 & 0x8c) == 0x88) {
      if ((bVar9 & 3) == 3) {
        bVar2 = pbVar4[0x1e];
      }
      else {
        bVar2 = pbVar4[0x18];
      }
      uVar8 = bVar2 & 0xf;
    }
    else {
      uVar8 = 0x10;
    }
    sVar3 = *(short *)(pbVar4 + 0x16);
    if ((in_a14 == 0) && (uVar13 == 0xd0)) {
      if ((bVar9 >> 6 & 1) == 0) {
        pbVar12 = pbVar4 + 0x18;
      }
      else {
        pbVar12 = pbVar4 + 0x20;
      }
      if ((int)uVar6 < (int)(pbVar12 + (8 - (int)pbVar4))) goto LAB_40227d53;
      if (*pbVar12 == 0x7f) {
        iVar7 = (*DAT_402034f8)(pbVar12 + 1,PTR_DAT_40227c7c,3);
        if (iVar7 == 0) goto LAB_40227dbb;
        bVar9 = pbVar4[1];
      }
    }
    iVar7 = uVar8 * 2 + param_1;
    if (((bVar9 & 8) != 0) && (*(short *)(iVar7 + 0x60) == sVar3)) goto LAB_40227d53;
    *(short *)(iVar7 + 0x60) = sVar3;
  }
LAB_40227dbb:
  uVar8 = 0x1e;
  if ((pbVar4[1] & 3) != 3) {
    uVar8 = 0x18;
  }
  if ((*pbVar4 & 0x8c) == 0x88) {
    uVar8 = uVar8 + 2;
  }
  *(short *)(param_2 + 0x14) = (short)uVar8;
  *(short *)(param_2 + 0x16) = ((short)uVar5 - (short)uVar8) + -4;
  if (in_a14 == 0) {
    if (((bVar1 & 3) == 0) && (0x17 < uVar6)) {
      FUN_402282d0(param_1,param_2,uVar13,param_3,param_4);
    }
  }
  else if ((((in_a14 != 4) && (in_a14 == 8)) && (*(int *)(param_1 + 0x90) != 0)) &&
          ((uVar8 <= uVar6 && ((bVar1 & 3) == 2)))) {
    if ((pbVar4[1] & 0x40) == 0) {
      *(uint *)(*(int *)(param_2 + 4) + 4) = *(int *)(*(int *)(param_2 + 4) + 4) + uVar8;
    }
    else {
      iVar7 = FUN_402211e8(param_1,param_2,uVar8);
      if (iVar7 == 0) goto LAB_40227d53;
    }
    param_2 = FUN_40223748(param_2,pbVar4,uVar6,uVar8);
    if (param_2 != 0) {
      iVar10 = *(int *)(param_2 + 4);
      iVar7 = *(int *)(iVar10 + 4);
      if ((*(int *)(DAT_40227c80 + 0x1fc) == 0) || (*(int *)(DAT_40227c80 + 0x200) != 1)) {
        if ((*(uint *)(*(int *)(param_1 + 0x90) + 8) & 1) == 0) {
          if (*(char *)(iVar15 + 0x18) != '\x01') {
LAB_40227f8c:
            FUN_402236f4(param_1);
            return 8;
          }
          if (*(ushort *)(iVar7 + 0xc) == DAT_40221a6c) {
            if ((*(char *)(DAT_40227c80 + -0x143) == '\x04') &&
               (*(char *)(DAT_40201728 + 0x6a) == '\x04')) {
              if ((*(undefined4 **)(DAT_4020101c + 0x1f0) != (undefined4 *)0x0) &&
                 (pcVar11 = (code *)**(undefined4 **)(DAT_4020101c + 0x1f0), pcVar11 != (code *)0x0)
                 ) {
                iVar14 = DAT_4020101c;
                (*pcVar11)(iVar7 + 6,*(int *)(iVar10 + 4) + 0xe,*(ushort *)(param_2 + 0x16) - 0xe);
                if ((*(int *)(iVar14 + 0x1f0) != 0) &&
                   (pcVar11 = *(code **)(*(int *)(iVar14 + 0x1f0) + 8), pcVar11 != (code *)0x0)) {
                  iVar7 = (*pcVar11)();
                  if (iVar7 == 1) {
                    FUN_4023b478(iVar14,iVar15);
                  }
                  else if (iVar7 != 2) goto LAB_40227d53;
                  iVar15 = *(int *)(iVar14 + 0x1f0);
                  *(undefined1 *)(DAT_40201728 + 0x6a) = 0;
                  if ((iVar15 != 0) && (pcVar11 = *(code **)(iVar15 + 0x10), pcVar11 != (code *)0x0)
                     ) {
                    (*pcVar11)();
                  }
                }
              }
            }
            else {
              FUN_4023a208(iVar7 + 6,*(int *)(iVar10 + 4) + 0xe,*(ushort *)(param_2 + 0x16) - 0xe);
            }
          }
        }
        else {
          if (*(ushort *)(iVar7 + 0xc) != DAT_40221a6c) goto LAB_40227f8c;
          if ((*(char *)(DAT_40227c80 + -0x143) == '\x04') &&
             (*(char *)(DAT_40201728 + 0x6a) == '\x04')) {
            if ((*(undefined4 **)(DAT_4020101c + 0x1f0) != (undefined4 *)0x0) &&
               (pcVar11 = (code *)**(undefined4 **)(DAT_4020101c + 0x1f0), pcVar11 != (code *)0x0))
            {
              iVar14 = DAT_4020101c;
              (*pcVar11)(iVar7 + 6,*(int *)(iVar10 + 4) + 0xe,*(ushort *)(param_2 + 0x16) - 0xe);
              if ((*(int *)(iVar14 + 0x1f0) != 0) &&
                 (pcVar11 = *(code **)(*(int *)(iVar14 + 0x1f0) + 8), pcVar11 != (code *)0x0)) {
                iVar7 = (*pcVar11)();
                if (iVar7 == 1) {
                  FUN_4023b478(iVar14,iVar15);
                }
                else if (iVar7 != 2) goto LAB_40227d53;
                iVar15 = *(int *)(iVar14 + 0x1f0);
                *(undefined1 *)(DAT_40201728 + 0x6a) = 0;
                if ((iVar15 != 0) && (pcVar11 = *(code **)(iVar15 + 0x10), pcVar11 != (code *)0x0))
                {
                  (*pcVar11)();
                }
              }
            }
          }
          else {
            FUN_4023a208(iVar7 + 6,*(int *)(iVar10 + 4) + 0xe,*(ushort *)(param_2 + 0x16) - 0xe);
          }
        }
      }
      else if ((*(int *)(DAT_4020101c + 500) != 0) &&
              (pcVar11 = *(code **)(*(int *)(DAT_4020101c + 500) + 8), pcVar11 != (code *)0x0)) {
        (*pcVar11)(iVar7 + 6,iVar7 + 0xe,*(ushort *)(param_2 + 0x16) - 0xe);
      }
    }
  }
LAB_40227d53:
  if (param_2 != 0) {
    FUN_4021e7c8(param_2);
  }
  return in_a14;
}

