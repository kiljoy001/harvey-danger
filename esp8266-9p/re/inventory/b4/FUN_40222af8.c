
undefined4 FUN_40222af8(int param_1)

{
  undefined *puVar1;
  undefined4 uVar2;
  int iVar3;
  int iVar4;
  undefined4 *puVar5;
  undefined1 uVar6;
  uint uVar7;
  uint uVar8;
  undefined4 *puVar9;
  
  iVar4 = DAT_402010cc;
  puVar5 = *(undefined4 **)(DAT_4020101c + 0x14);
  if (puVar5 == (undefined4 *)0x0) {
    uVar2 = 0;
  }
  else {
    uVar7 = 1;
    if ((puVar5[0x34] & 0x1000000) == 0) {
      uVar2 = 1;
    }
    else {
      if (param_1 == 0) {
        uVar7 = (uint)*(byte *)(DAT_402010cc + 0x80);
        uVar8 = 1;
        do {
          puVar9 = puVar5 + uVar8;
          if (puVar9[0x26] != 0) {
            uVar2 = puVar5[0x24];
            puVar5[0x24] = puVar9[0x26];
            ieee80211_send_mgmt(puVar5,0xa0,4);
            ieee80211_send_mgmt(puVar5,0xc0,2);
            puVar5[0x24] = uVar2;
            FUN_4022a158(puVar5,puVar9[0x26]);
            uVar7 = (uint)*(byte *)(iVar4 + 0x80);
          }
          uVar8 = uVar8 + 1 & 0xff;
        } while (uVar8 < uVar7 + 2);
        uVar6 = 1;
        iVar4 = DAT_4020101c;
        FUN_40232298(*puVar5);
        FUN_40206524(puVar5);
      }
      else {
        iVar4 = DAT_4020101c;
        (*(code *)PTR_FUN_402055e0)();
        (*DAT_40201774)(DAT_40201d34);
        memw();
        memw();
        *(uint *)(DAT_40201d2c + 0x294) = *(uint *)(DAT_40201d2c + 0x294) | uVar7;
        iVar3 = FUN_4021d4c0();
        uVar6 = (undefined1)uVar7;
        if (iVar3 != 0) {
          FUN_4021d4f4();
        }
        FUN_4021e168(*(undefined2 *)PTR_DAT_40222ae8);
      }
      uVar2 = DAT_402212b8;
      *PTR_DAT_4021eac4 = uVar6;
      (*(code *)PTR_FUN_4020177c)(uVar2);
      FUN_4021b74c(1,DAT_40201700,2,0);
      FUN_402205d4(1);
      uVar2 = 0;
      puVar1 = PTR_s_ieee80211_hostap_c_402216ac;
      if (*(int *)(iVar4 + 0x1a0) != 0) {
        (*(code *)PTR_mem_free_fileline_40201778)
                  (*(int *)(iVar4 + 0x1a0),PTR_s_ieee80211_hostap_c_402216ac,DAT_40222aec);
        puVar1 = PTR_s_ieee80211_hostap_c_402216ac;
        *(undefined4 *)(iVar4 + 0x1a0) = uVar2;
      }
      if (*(int *)(iVar4 + 0x1a4) != 0) {
        (*(code *)PTR_mem_free_fileline_40201778)(*(int *)(iVar4 + 0x1a4),puVar1,DAT_40222af0);
        *(undefined4 *)(iVar4 + 0x1a4) = uVar2;
      }
      FUN_4021b590(1,0,0,1,0);
      FUN_40220264(0,1,0);
      FUN_402202e4(1);
      if ((param_1 == 0) && ((*(byte *)(DAT_40201728 + 0x6b) & 1) != 0)) {
        (*(code *)PTR_FUN_4020177c)(DAT_40222758);
        memw();
        memw();
        *(uint *)(DAT_40205580 + 0x26c) = *(uint *)(DAT_40205580 + 0x26c) & 0xffffffef;
      }
      ieee80211_hostap_attach();
      iVar4 = *(int *)(DAT_402047e0 + 4);
      if (((*(byte *)(DAT_402047e0 + 0xf) & 1) == 0) && (iVar4 != 0)) {
        if ((**(uint **)(iVar4 + 0x24) & 0x100000) == 0) {
          if ((**(uint **)(iVar4 + 0x24) >> 0x1e & 1) != 0) {
            uVar2 = 6;
          }
        }
        else {
          uVar2 = 4;
        }
        FUN_40220b0c(iVar4,uVar2);
      }
      uVar2 = 1;
      puVar5[0x34] = puVar5[0x34] & DAT_40222af4;
    }
  }
  return uVar2;
}

