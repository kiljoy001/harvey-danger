
void pm_onBcnRx(int param_1,int *param_2,int param_3)

{
  byte bVar1;
  ushort uVar2;
  undefined *puVar3;
  undefined *puVar4;
  undefined *puVar5;
  undefined *puVar6;
  byte bVar7;
  int *piVar8;
  uint uVar9;
  int iVar10;
  int iVar11;
  uint uVar12;
  undefined *puVar13;
  int *piVar14;
  uint uVar15;
  int iVar16;
  
  puVar13 = PTR_DAT_00011bfc;
  if (param_3 - (int)param_2 < 0xc) {
    return;
  }
  *(uint *)(PTR_DAT_00011bfc + 0x54) = (uint)*(ushort *)(param_2 + 2) << 10;
  (*(code *)PTR_os_timer_disarm_00011c40)(puVar13);
  (*(code *)PTR_os_timer_disarm_00011c44)(PTR_DAT_00011c00);
  puVar3 = PTR_g_ic_00011c08;
  iVar11 = *(int *)(puVar13 + 0x74);
  if (iVar11 == 0) {
LAB_00011ccc:
    (*(code *)PTR_pm_send_keepalive_pkt_00011c48)();
    return;
  }
  if ((*(uint *)PTR_app_optimize_and_debug_mac_00011c04 & 1) != 0) {
    (*(code *)PTR_pm_send_keepalive_pkt_00011c4c)();
    (*(code *)PTR_pm_set_idle_timer_00011c50)();
    return;
  }
  puVar13[0x82] = 0;
  if (((**(char **)PTR_g_wifi_nvs_00011c0c != '\x01') || (*(int *)(puVar3 + 8) == 0)) ||
     (*(int *)(*(int *)(puVar3 + 8) + 0x78) != 5)) goto LAB_00011ccc;
  iVar16 = *(int *)(puVar13 + 0x54);
  piVar8 = param_2 + 3;
  piVar14 = (int *)0x0;
  if (iVar16 == 0) {
    return;
  }
  for (; 1 < param_3 - (int)piVar8; piVar8 = (int *)((int)piVar8 + *(byte *)((int)piVar8 + 1) + 2))
  {
    if (param_3 - (int)piVar8 <= (int)(*(byte *)((int)piVar8 + 1) + 1)) {
      return;
    }
    if ((char)*piVar8 == '\x05') {
      piVar14 = piVar8;
    }
  }
  if (piVar14 == (int *)0x0) {
    return;
  }
  memw();
  *(int *)PTR_DAT_00011c14 = (*param_2 - *DAT_00011c10) - *(int *)PTR_WdevTimOffSet_00011c18;
  if (*(short *)(puVar13 + 0x84) == *(short *)(puVar13 + 0x88)) {
    uVar2 = *(ushort *)(puVar13 + 0x80);
    bVar7 = *(byte *)(piVar14 + 1);
    bVar1 = *(byte *)((int)piVar14 + 1);
    if ((bVar7 & 1) != 0) {
      puVar13[0x84] = 0;
    }
    if ((uVar2 & 0x3fff) != 0) {
      uVar9 = (uVar2 & 0x3fff) >> 3;
      uVar12 = bVar7 & 0xfffffffe;
      if (((uVar9 < uVar12) || ((uVar12 + (bVar1 - 4) & 0xff) < uVar9)) ||
         (((uint)*(byte *)((int)piVar14 + (uVar9 - uVar12) + 5) & 1 << ((byte)uVar2 & 7)) == 0))
      goto LAB_00011da3;
    }
    puVar13[0x85] = 0;
  }
LAB_00011da3:
  if ((puVar13[0x84] == '\0') && (puVar13[0x8d] == '\0')) {
    puVar13[0x84] = puVar13[0x88];
  }
  puVar3 = PTR_DAT_00011c24;
  uVar9 = (uint)((*(int *)(*(int *)(param_1 + 0x28) + 4) + DAT_00011c1c) - *(int *)(puVar13 + 0x5c))
          >> 7;
  if (uVar9 - 1 < 0x176) {
    PTR_DAT_00011c20[uVar9] = PTR_DAT_00011c20[uVar9] + '\x01';
    *puVar3 = *puVar3 + '\x01';
  }
  puVar6 = PTR_DAT_00011c30;
  puVar5 = PTR_DAT_00011c2c;
  puVar4 = PTR_DAT_00011c20;
  if (99 < (byte)*puVar3) {
    bVar7 = 0;
    uVar9 = 0;
    do {
      bVar7 = bVar7 + PTR_DAT_00011c20[uVar9];
      uVar12 = uVar9;
      if (4 < bVar7) goto LAB_00011e33;
      uVar9 = uVar9 + 1;
    } while (uVar9 != 0x177);
    uVar12 = 0;
LAB_00011e33:
    do {
      uVar9 = uVar9 + 1;
      if (0x176 < uVar9) {
        uVar9 = 0;
        break;
      }
      bVar7 = bVar7 + PTR_DAT_00011c20[uVar9];
    } while (bVar7 < 0x5f);
    bVar7 = puVar13[0x8a];
    uVar15 = DAT_00011c28 + uVar12 * -0x80;
    uVar9 = (uVar9 - uVar12) * 0x80 + 0x80;
    *(uint *)PTR_DAT_00011c2c = uVar15;
    *(uint *)puVar6 = uVar9;
    uVar12 = (uint)bVar7 * 1000 + 32000;
    if (uVar12 < uVar15) {
      *(uint *)puVar5 = uVar12;
    }
    if (*(uint *)puVar5 < 0x5dc) {
      *(undefined4 *)puVar5 = 0x5dc;
    }
    if ((uint)(byte)puVar13[0x8b] * 1000 < uVar9) {
      *(uint *)puVar6 = (uint)(byte)puVar13[0x8b] * 1000;
    }
    if (*(uint *)puVar6 <= DAT_00011c34) {
      *(undefined4 *)puVar6 = DAT_00011c38;
    }
    iVar10 = 0;
    do {
      puVar4[iVar10] = 0;
      iVar10 = iVar10 + 1;
    } while (iVar10 != 0x177);
    *puVar3 = 0;
  }
  uVar9 = (*(code *)PTR___umoddi3_00011c54)(*param_2,param_2[1],iVar16,0);
  if ((uint)(iVar16 + DAT_00011c3c) < uVar9) {
    iVar10 = iVar16 + *(int *)(*(int *)(param_1 + 0x28) + 4);
  }
  else {
    iVar10 = *(int *)(*(int *)(param_1 + 0x28) + 4);
  }
  *(uint *)(puVar13 + 0x60) = iVar10 - uVar9;
  iVar10 = *(int *)(puVar13 + 0x60);
  *(int *)(puVar13 + 0x58) = iVar16 + iVar10;
  if (iVar11 == 1) {
    uVar12 = (uint)*(byte *)((int)piVar14 + 2);
LAB_00011f43:
    uVar9 = uVar12;
    if (uVar12 != 0) goto LAB_00011f4d;
    uVar9 = (uint)*(byte *)((int)piVar14 + 3);
  }
  else {
    uVar9 = (uint)(byte)puVar13[0x78];
    if (iVar11 == 2) {
      uVar12 = (uint)*(byte *)((int)piVar14 + 2);
      if (uVar9 == 0) goto LAB_00011f43;
      if (uVar12 == 0) {
        uVar12 = (uint)*(byte *)((int)piVar14 + 3);
        uVar15 = (uint)(*(byte *)((int)piVar14 + 3) >> 1);
        if (uVar9 <= uVar15) goto LAB_00011f4d;
        iVar11 = (*(code *)PTR___divsi3_00011c5c)(uVar9 + uVar15,uVar12);
        uVar12 = iVar11 * uVar12;
      }
      else {
        if (uVar9 <= uVar12) goto LAB_00011f4d;
        uVar15 = (uint)*(byte *)((int)piVar14 + 3);
        iVar11 = (*(code *)PTR___divsi3_00011c58)
                           ((uVar9 - uVar12) + (uint)(*(byte *)((int)piVar14 + 3) >> 1),uVar15);
        uVar12 = iVar11 * uVar15 + uVar12;
      }
      uVar9 = uVar12 & 0xff;
    }
  }
  if (uVar9 == 0) {
    uVar9 = 3;
  }
LAB_00011f4d:
  *(uint *)(puVar13 + 0x5c) = uVar9 * iVar16 + iVar10;
  if (puVar13[0x85] == '\0') {
    (*(code *)PTR_pm_allow_tx_00011c60)();
    return;
  }
  if (puVar13[0x84] == '\0') {
    if (*(int *)(puVar13 + 0x6c) == 6) {
      (*(code *)PTR___wifi_task_suspend_all_00011c64)();
      (*(code *)PTR_pm_change_to_state_00011c68)(4);
      (*(code *)PTR___wifi_task_resume_all_00011c6c)();
    }
    (*(code *)PTR_pm_wait_tx_rx_idle_fun_00011c70)();
    return;
  }
  iVar11 = (*(code *)PTR_pm_send_keepalive_pkt_00011c74)();
  if (iVar11 != 0) {
    return;
  }
  (*(code *)PTR___wifi_task_suspend_all_00011c78)();
  (*(code *)PTR_pm_change_to_state_00011c7c)(6);
  (*(code *)PTR___wifi_task_resume_all_00011c80)();
  return;
}

