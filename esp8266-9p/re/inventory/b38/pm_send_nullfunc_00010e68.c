
void pm_send_nullfunc(uint param_1)

{
  int iVar1;
  undefined4 *puVar2;
  uint *puVar3;
  undefined4 uVar4;
  uint uVar5;
  int iVar6;
  undefined4 uVar7;
  undefined *puVar8;
  undefined4 auStack_40 [4];
  int iStack_30;
  
  param_1 = param_1 & 0xff;
  auStack_40[0] = 0;
  iVar1 = *(int *)(PTR_g_ic_00010e28 + 8);
  if ((iVar1 != 0) && (*(int *)(iVar1 + 0x78) == 5)) {
    iVar6 = 0x18;
    if ((*(int *)(iVar1 + 0xc0) != 0) &&
       (iVar6 = 0x18, (*(uint *)(*(int *)(iVar1 + 0xc0) + 8) & 0x42) != 0)) {
      iVar6 = 0x1a;
    }
    puVar8 = PTR_g_ic_00010e28;
    puVar2 = (undefined4 *)(*(code *)PTR_ieee80211_getmgtframe_00010e50)(auStack_40,iVar6,0);
    if (puVar2 == (undefined4 *)0x0) {
      (*(code *)PTR_esp_log_write_00010e54)(1,PTR_DAT_00010e30,PTR_s_n_buf_for_null_00010e2c);
    }
    else {
      iStack_30 = puVar2[3];
      (*(code *)PTR_memset_00010e58)(iStack_30,0,iVar6);
      *(undefined2 *)((int)puVar2 + 0x1a) = 0;
      puVar3 = (uint *)*puVar2;
      *(short *)(puVar2 + 6) = (short)iVar6;
      memw();
      memw();
      *puVar3 = *puVar3 | DAT_00010e34;
      memw();
      memw();
      *puVar3 = *puVar3 | DAT_00010e38;
      memw();
      memw();
      *puVar3 = *puVar3 & DAT_00010e3c;
      memw();
      memw();
      *puVar3 = *puVar3 & DAT_00010e40 | iVar6 << 0xc;
      uVar7 = *(undefined4 *)(puVar8 + 8);
      uVar4 = (*(code *)PTR_ic_get_addr_00010e5c)(0);
      (*(code *)PTR_ieee80211_send_setup_00010e60)
                (uVar7,puVar2,0x48,0,uVar4,PTR_DAT_00010e44,PTR_DAT_00010e44);
      puVar3 = (uint *)puVar2[10];
      memw();
      puVar3[5] = *DAT_00010e48;
      puVar3[2] = puVar3[2] & DAT_00010e4c & 0xfffffff0 | 6;
      *puVar3 = *puVar3 & 0xfffffff7;
      puVar3[1] = puVar3[1] & 0xfffffffd;
      puVar2[8] = 0;
      if (param_1 == 0) {
        uVar5 = 0x40;
      }
      else {
        *(byte *)(iStack_30 + 1) = *(byte *)(iStack_30 + 1) | 0x10;
        puVar3 = (uint *)puVar2[10];
        uVar5 = 0x20;
      }
      puVar3[4] = uVar5;
      (*(code *)PTR_ppTxPkt_00010e64)(puVar2);
    }
  }
  return;
}

