
/* B3 SDK fingerprint: 10/89 windows, object ieee80211_output.o. */

undefined4 ieee80211_send_mgmt(int param_1,int param_2,uint param_3)

{
  short sVar1;
  char cVar2;
  uint *puVar3;
  ushort uVar4;
  short sVar5;
  uint uVar6;
  int iVar7;
  undefined2 *puVar8;
  int iVar9;
  undefined4 uVar10;
  uint uVar11;
  undefined4 uVar12;
  byte bVar13;
  uint uVar14;
  int iVar15;
  uint *puVar16;
  uint uVar17;
  int iVar18;
  int in_a13;
  int iVar19;
  ushort *local_50;
  uint uStack_4c;
  int iStack_48;
  uint uStack_44;
  int iStack_40;
  ushort *puStack_3c;
  int iStack_38;
  int iStack_34;
  int iStack_30;
  int iStack_2c;
  
  iStack_2c = *(int *)(param_1 + 0x90);
  if (iStack_2c == 0) {
    return 1;
  }
  iVar19 = DAT_40224ef8 + -0x2c;
  if (param_2 == 0) {
LAB_40225299:
    iVar15 = (uint)*(byte *)(DAT_402251bc + 0x41) - (uint)*(byte *)(DAT_402251bc + 0x40);
    if (iVar15 < 7) {
      iVar9 = iVar15 + 0x2f;
    }
    else {
      iVar9 = 0x36;
    }
    if (iVar15 < 8) {
      iVar15 = 0;
    }
    else {
      iVar15 = iVar15 + -5;
    }
    if (*(ushort **)(DAT_40224ef8 + 0x128) == (ushort *)0x0) {
      uVar17 = 0;
    }
    else {
      uVar17 = (uint)**(ushort **)(DAT_40224ef8 + 0x128);
    }
    if (*(ushort **)(DAT_40224ef8 + 300) == (ushort *)0x0) {
      uVar14 = 0;
    }
    else {
      uVar14 = (uint)**(ushort **)(DAT_40224ef8 + 300);
    }
    if (*(ushort **)(DAT_40224ef8 + 0x120) == (ushort *)0x0) {
      uVar6 = 0;
    }
    else {
      uVar6 = (uint)**(ushort **)(DAT_40224ef8 + 0x120);
    }
    iVar7 = *(int *)(DAT_40224ef8 + 0x150);
    if (*(ushort **)(DAT_40224ef8 + 0x134) == (ushort *)0x0) {
      uVar11 = 0;
    }
    else {
      uVar11 = (uint)**(ushort **)(DAT_40224ef8 + 0x134);
    }
    if ((iVar7 == 0) || (*(int *)(iVar7 + 4) == 0)) {
      iVar7 = 0;
    }
    else {
      iVar7 = *(byte *)(iVar7 + 8) + 10;
    }
    iVar9 = FUN_40221258(&local_50,0x18,
                         uVar11 + iVar7 + uVar17 + iVar9 + iVar15 + uVar14 + uVar6 + 0x65);
    iVar15 = DAT_402251bc;
    in_a13 = 0;
    if (iVar9 == 0) goto LAB_40225254;
    uVar17 = *(uint *)(iVar19 + 0x1c);
    uVar4 = 0x11;
    *(undefined2 *)(iVar9 + 0x14) = 0x18;
    if (*(char *)(iVar15 + 0x7e) == '\0') {
      uVar4 = 1;
    }
    uVar14 = **(uint **)(iStack_2c + 0x78);
    if (((uVar17 & 0x40000) != 0) && ((uVar14 & 0x80) != 0)) {
      uVar4 = uVar4 | 0x20;
    }
    if ((((~uVar14 & 0xc0) == 0) || ((~uVar14 & 0x480) == 0)) &&
       ((*(uint *)(iVar19 + 0x20) & 0x4000) != 0)) {
      uVar4 = uVar4 | 0x400;
    }
    *local_50 = uVar4;
    local_50[1] = *(ushort *)(iStack_2c + 0x1c);
    local_50 = local_50 + 2;
    if (param_2 == 0x20) {
      (*DAT_40201e88)(local_50,iStack_2c,6);
      local_50 = local_50 + 3;
    }
    local_50 = (ushort *)FUN_40224e14(local_50,DAT_40203e14,*(undefined4 *)(iVar15 + 0x58));
    iStack_34 = DAT_40224ef8;
    local_50 = (ushort *)FUN_40224d7c();
    if (((*(int *)(iVar15 + 0x3bc) == 0) || (*(int *)(iVar15 + 0x3c0) != 1)) ||
       (*(int *)(iVar19 + 0x160) == 0)) {
      cVar2 = *(char *)(iVar15 + 0x7d);
      if (((cVar2 == '\x04') || (cVar2 == '\x05')) || (cVar2 == '\a')) {
        iVar7 = *(int *)(iVar19 + 0x158);
        puStack_3c = local_50;
        if (iVar7 != 0) {
          iVar18 = iVar7 + 2;
          (*DAT_40201e88)(local_50,iVar18,*(byte *)(iVar7 + 3) + 2);
          local_50 = (ushort *)((int)puStack_3c + *(byte *)(iVar18 + 1) + 2);
        }
        cVar2 = *(char *)(iVar15 + 0x7d);
      }
      if ((((cVar2 == '\x02') || (cVar2 == '\x03')) || (cVar2 == '\x06')) &&
         (iVar7 = *(int *)(iVar19 + 0x154), iVar7 != 0)) {
        iStack_38 = iVar7 + 2;
        (*DAT_40201e88)(local_50,iStack_38,*(byte *)(iVar7 + 3) + 2);
        local_50 = (ushort *)((int)local_50 + *(byte *)(iStack_38 + 1) + 2);
      }
    }
    else {
      local_50 = (ushort *)FUN_40224e6c();
    }
    local_50 = (ushort *)FUN_40224dcc(local_50,iStack_34);
    if (((*(uint *)(iVar15 + 0x380) & 0x80000) != 0) && ((*(uint *)(param_1 + 0x50) & 0xc0) != 0)) {
      local_50 = (ushort *)FUN_40223360(local_50,param_1);
    }
    local_50 = (ushort *)(*(code *)PTR_FUN_402251c4)(local_50,0);
    if (((*(uint *)(iVar15 + 0x380) & 0x10000000) != 0) && ((*(uint *)(param_1 + 0x50) & 0xc0) != 0)
       ) {
      local_50 = (ushort *)FUN_4022337c(local_50,param_1);
    }
    if (*(int *)(iVar19 + 0x14c) != 0) {
      local_50 = (ushort *)FUN_40224e6c();
    }
    iVar15 = *(int *)(iVar19 + 0x17c);
    if ((iVar15 != 0) && (*(int *)(iVar15 + 4) != 0)) {
      local_50 = (ushort *)
                 FUN_4022af4c(local_50,iVar15 + 1,*(int *)(iVar15 + 4),*(undefined1 *)(iVar15 + 8));
    }
    sVar5 = (short)local_50;
    iVar15 = *(int *)(iVar9 + 4);
    sVar1 = *(short *)(iVar9 + 0x14);
    iStack_30 = iVar9;
  }
  else {
    if (param_2 != 0x10) {
      if (param_2 == 0x20) goto LAB_40225299;
      if (param_2 == 0x30) goto LAB_402252ed;
      if (param_2 == 0xa0) {
        uStack_4c = param_3;
        iVar9 = FUN_40221258(&local_50,0x18,2);
        if (iVar9 == 0) {
          return 1;
        }
        *(undefined2 *)(iVar9 + 0x14) = 0x18;
        *local_50 = (ushort)uStack_4c;
        *(undefined2 *)(iVar9 + 0x16) = 2;
        goto LAB_402253fd;
      }
      if (param_2 != 0xb0) {
        if (param_2 == 0xc0) {
          uStack_4c = param_3;
          iVar9 = FUN_40221258(&local_50,0x18,2);
          if (iVar9 == 0) {
            return 1;
          }
          iVar15 = *(int *)(param_1 + 0x98);
          *(undefined2 *)(iVar9 + 0x14) = 0x18;
          *local_50 = (ushort)uStack_4c;
          *(undefined2 *)(iVar9 + 0x16) = 2;
          if (iVar15 != iStack_2c) {
            *(uint *)(iStack_2c + 8) = *(uint *)(iStack_2c + 8) & 0xfffffffe;
          }
          goto LAB_402253fd;
        }
        goto LAB_40225254;
      }
      iStack_48 = 0;
      uStack_44 = param_3 & 0xffff;
      if (((uStack_44 == 2) || (uStack_44 == 3)) && (*(int *)(param_1 + 0x5c) != 0)) {
        iStack_48 = 1;
      }
      iStack_40 = 1;
      if (iStack_48 == 0) {
        if ((uStack_44 < 3) && ((uStack_44 != 1 || (*(char *)(DAT_402251bc + 0x7c) != '\x02')))) {
          iStack_40 = 0;
        }
LAB_402255f4:
        uVar10 = 0x18;
      }
      else {
        iStack_40 = 1;
        if ((int)param_3 >> 0x10 != 0) goto LAB_402255f4;
        uVar10 = 0x1c;
      }
      if ((iStack_48 == 0) || ((int)param_3 >> 0x10 != 0)) {
        uVar12 = 6;
      }
      else {
        uVar12 = 0x88;
      }
      uStack_4c = param_3;
      iVar9 = FUN_40221258(&local_50,uVar10,uVar12);
      in_a13 = 0;
      if (iVar9 == 0) {
LAB_40225254:
        if (in_a13 != 0) {
          uVar10 = 4;
          if (((**(uint **)(in_a13 + 0x24) & 0x100000) == 0) &&
             ((**(uint **)(in_a13 + 0x24) >> 0x15 & 1) != 0)) {
            uVar10 = 5;
          }
          FUN_40220b0c(in_a13,uVar10);
        }
        return 1;
      }
      *(undefined2 *)(iVar9 + 0x14) = 0x18;
      *local_50 = (ushort)(iStack_40 != 0);
      local_50[1] = (ushort)uStack_44;
      local_50[2] = (ushort)(uStack_4c >> 0x10);
      if ((iStack_48 == 0) || ((int)uStack_4c >> 0x10 != 0)) {
        *(undefined2 *)(iVar9 + 0x16) = 6;
      }
      else {
        uVar10 = *(undefined4 *)(param_1 + 0x5c);
        local_50[3] = (ushort)DAT_402251c0;
        (*DAT_40201e88)(local_50 + 4,uVar10,0x80);
        iVar15 = *(int *)(*(int *)(iVar9 + 4) + 4);
        *(undefined2 *)(iVar9 + 0x16) = 0x88;
        *(int *)(*(int *)(iVar9 + 4) + 4) = iVar15 + 0x1c;
        puVar8 = (undefined2 *)FUN_40221190(param_1,iVar9);
        uVar17 = DAT_40207e28;
        puVar3 = *(uint **)(iVar9 + 0x24);
        iVar15 = *(int *)(*(int *)(iVar9 + 4) + 4) + -0x18;
        *(int *)(*(int *)(iVar9 + 4) + 4) = iVar15;
        *(int *)(iVar9 + 0x10) = iVar15;
        if (puVar8 == (undefined2 *)0x0) {
          uVar17 = puVar3[3] & uVar17;
        }
        else {
          bVar13 = (byte)*puVar8;
          iVar15 = *(int *)(iVar19 + 0x14);
          *puVar3 = *puVar3 & 0x3f | (*puVar3 >> 6 | 1) << 6;
          if (iVar15 == param_1) {
            bVar13 = bVar13 | 0x80;
          }
          *(byte *)(puVar3 + 3) = bVar13;
          uVar17 = puVar3[3] & uVar17 | (**(uint **)(puVar8 + 0x4c) & 0xf) << 8;
        }
        puVar3[3] = uVar17;
      }
      goto LAB_402253fd;
    }
LAB_402252ed:
    iVar15 = (uint)*(byte *)(DAT_402251bc + 0x41) - (uint)*(byte *)(DAT_402251bc + 0x40);
    if (iVar15 < 7) {
      iVar9 = iVar15 + 9;
    }
    else {
      iVar9 = 0x10;
    }
    if (iVar15 < 8) {
      iVar15 = 0;
    }
    else {
      iVar15 = iVar15 + -5;
    }
    if (*(ushort **)(DAT_40224ef8 + 0x124) == (ushort *)0x0) {
      uVar17 = 0;
    }
    else {
      uVar17 = (uint)**(ushort **)(DAT_40224ef8 + 0x124);
    }
    iVar7 = *(int *)(DAT_40224ef8 + 0x154);
    if ((iVar7 == 0) || (*(int *)(iVar7 + 4) == 0)) {
      iVar7 = 0;
    }
    else {
      iVar7 = *(byte *)(iVar7 + 8) + 10;
    }
    uStack_4c = param_3;
    iVar9 = FUN_40221258(&local_50,0x18,iVar9 + iVar15 + uVar17 + iVar7 + 0x56);
    iVar15 = DAT_402045c0;
    in_a13 = 0;
    iStack_30 = iVar9;
    if (iVar9 == 0) goto LAB_40225254;
    *(undefined2 *)(iVar9 + 0x14) = 0x18;
    uVar4 = ieee80211_getcapinfo((uint)*(byte *)(iVar15 + 0xc9) * 0xc + iVar19 + 0x78);
    *local_50 = uVar4;
    local_50[1] = (ushort)uStack_4c;
    if (uStack_4c == 0) {
      local_50[2] = *(ushort *)(iStack_2c + 0x1a);
    }
    local_50 = local_50 + 3;
    iStack_34 = DAT_40224ef8;
    local_50 = (ushort *)FUN_40224d7c();
    local_50 = (ushort *)FUN_40224dcc(local_50,iStack_34);
    uVar17 = *(uint *)(iStack_2c + 8) & 0xc0;
    if (uVar17 == 0x40) {
      local_50 = (ushort *)FUN_40223360(local_50,param_1);
      local_50 = (ushort *)FUN_4022349c(local_50,param_1);
      uVar17 = *(uint *)(iStack_2c + 8) & 0xc0;
    }
    if (uVar17 == 0xc0) {
      local_50 = (ushort *)FUN_4022337c(local_50,param_1);
      local_50 = (ushort *)FUN_402234b8(local_50,param_1);
    }
    if (*(int *)(iVar19 + 0x150) != 0) {
      local_50 = (ushort *)FUN_40224e6c();
    }
    iVar15 = *(int *)(iVar19 + 0x180);
    if ((iVar15 != 0) && (*(int *)(iVar15 + 4) != 0)) {
      local_50 = (ushort *)
                 FUN_4022af4c(local_50,iVar15 + 1,*(int *)(iVar15 + 4),*(undefined1 *)(iVar15 + 8));
    }
    sVar5 = (short)local_50;
    iVar15 = *(int *)(iStack_30 + 4);
    sVar1 = *(short *)(iStack_30 + 0x14);
  }
  *(short *)(iStack_30 + 0x16) = sVar5 - (sVar1 + (short)*(undefined4 *)(iVar15 + 4));
LAB_402253fd:
  iVar15 = DAT_40201d2c;
  iVar7 = *(int *)(iVar19 + 0x10);
  *(uint *)(*(int *)(iVar9 + 0x24) + 4) = *(uint *)(*(int *)(iVar9 + 0x24) + 4) & DAT_4021cf38;
  *(uint *)(*(int *)(iVar9 + 0x24) + 4) = *(uint *)(*(int *)(iVar9 + 0x24) + 4) & 0xfffffff0 | 7;
  puVar3 = *(uint **)(iVar9 + 0x24);
  memw();
  uVar17 = *(uint *)(iVar15 + 0x200);
  *puVar3 = *puVar3 & 0xfffffffe;
  puVar16 = *(uint **)(iVar9 + 0x24);
  puVar3[6] = uVar17;
  *puVar16 = *puVar16 & 0xfffffffd | (uint)(iVar7 != param_1) << 1;
  puVar3 = *(uint **)(iVar9 + 0x24);
  if ((*(short *)(iStack_2c + 0x1a) == 0) || ((*puVar3 >> 7 & 1) != 0)) {
    *(undefined4 *)(iVar9 + 0x1c) = 0;
  }
  else {
    uVar10 = FUN_4021fd00();
    *(undefined4 *)(iVar9 + 0x1c) = uVar10;
    puVar3 = *(uint **)(iVar9 + 0x24);
  }
  *(undefined1 *)(puVar3 + 2) = *(undefined1 *)(iVar19 + 0x7d);
  uVar10 = FUN_40224824(param_1,iVar9,param_2);
  return uVar10;
}

