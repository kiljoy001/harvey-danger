
undefined4 FUN_402278f4(int *param_1,int param_2,uint param_3)

{
  int iVar1;
  uint uVar2;
  int iVar3;
  undefined4 *puVar4;
  int iVar5;
  undefined1 uVar6;
  undefined1 uVar7;
  uint uVar8;
  int iVar9;
  undefined *puVar10;
  
  iVar5 = param_1[4];
  uVar8 = *(uint *)(iVar5 + 0x44);
  (*(code *)PTR_printf_like_401018a0_40201448)
            (PTR_s_state___d__>__d___x__402278e4,uVar8,param_2,param_3);
  *(int *)(iVar5 + 0x44) = param_2;
  iVar3 = DAT_4020101c;
  iVar1 = *(int *)(*param_1 + 0x348);
  if (param_2 == 0) {
    puVar10 = PTR_s_ieee80211_sta_c_402278e8;
    if (uVar8 == 2) {
      if (*(char *)(*(int *)(DAT_4020101c + 0x10) + 0xd0) != '\x02') {
        sta_status_set(4);
      }
      (*(code *)PTR_FUN_4020177c)(iVar5 + 4);
      (*(code *)PTR_FUN_4020177c)(iVar5 + 0x18);
      uVar7 = 0xca;
      if (param_3 == 2) {
        uVar7 = 2;
      }
      FUN_402297ec();
    }
    else if (uVar8 == 3) {
      if (*(int *)(iVar5 + 0x90) != 0) {
        (*(code *)PTR_FUN_4020177c)(iVar5 + 4);
        (*(code *)PTR_FUN_4020177c)(iVar5 + 0x18);
        FUN_40229c40(iVar5,*(undefined4 *)(iVar5 + 0x90));
      }
      if (param_3 == 0x11) {
        uVar7 = 5;
      }
      else {
        uVar7 = 0xcb;
        if (param_3 == 4) {
          uVar7 = 4;
        }
      }
    }
    else {
      if ((uVar8 != 5) && (uVar8 != 7)) {
        return 0;
      }
      if ((param_3 & 0xff) == 1) {
        sta_status_set(4);
        ieee80211_send_mgmt(iVar5,0xa0,4);
        uVar7 = 200;
      }
      else {
        iVar3 = iVar5 + 4;
        iVar9 = iVar5 + 0x18;
        if ((param_3 & 0xff) == 2) {
          sta_status_set(2);
          (*(code *)PTR_FUN_4020177c)(iVar3);
          (*(code *)PTR_FUN_4020177c)(iVar9);
          ieee80211_send_mgmt(iVar5,0xa0,8);
          uVar7 = 0xcc;
        }
        else {
          (*(code *)PTR_FUN_4020177c)(iVar3);
          (*(code *)PTR_FUN_4020177c)(iVar9);
          ieee80211_send_mgmt(iVar5,0xa0,8);
          uVar7 = 8;
        }
      }
      FUN_40229c40(iVar5,iVar1);
    }
    if (((*DAT_40222028 != 0) && (*DAT_402278f0 == '\0')) &&
       (puVar4 = (undefined4 *)(*(code *)PTR_mem_alloc_fileline_40201764)(0x2c,puVar10,0x11e),
       puVar4 != (undefined4 *)0x0)) {
      *(undefined1 *)((int)puVar4 + 0x2b) = uVar7;
      *puVar4 = 1;
      (*DAT_40201e88)((int)puVar4 + 0x25,iVar5 + 0x48,6);
      (*DAT_40201e88)(puVar4 + 1,DAT_40203e14,0x20);
      *(char *)(puVar4 + 9) = (char)*(undefined4 *)(DAT_4020101c + 0x220);
      iVar3 = (*DAT_40202c8c)(0x15,1,puVar4);
      if (iVar3 != 0) {
        (*(code *)PTR_mem_free_fileline_40201778)(puVar4,puVar10,0x129);
      }
    }
  }
  else if (param_2 == 2) {
    if (uVar8 < 2) {
      ieee80211_send_mgmt(iVar5,0xb0,1);
    }
    else {
      iVar3 = (int)param_3 >> 8;
      if ((uVar8 < 4) && (1 < uVar8)) {
        if ((param_3 & 0xff) == 0xb0) {
          ieee80211_send_mgmt(iVar5,0xb0,2);
        }
        else if ((param_3 & 0xff) == 0xc0) {
          FUN_40227860(iVar5,iVar3);
        }
      }
      else if (uVar8 == 5) {
        uVar2 = param_3 & 0xff;
        if (uVar2 != 0xa0) {
          if (uVar2 == 0xb0) {
            ieee80211_send_mgmt(iVar5,0xb0,2);
            *(uint *)(iVar5 + 0x44) = uVar8;
            return 0;
          }
          if (uVar2 != 0xc0) {
            return 0;
          }
        }
        puVar10 = PTR_s_ieee80211_sta_c_402278e8;
        if ((*(int *)(DAT_40201018 + 0x388) == 0) &&
           (((iVar3 == 0xf || (iVar3 == 0xe)) || (iVar3 == 6)))) {
          sta_status_set(2);
        }
        uVar7 = (undefined1)(param_3 >> 8);
        (*(code *)PTR_FUN_4020177c)(iVar5 + 0x18);
        iVar9 = iVar5 + 4;
        (*(code *)PTR_FUN_4020177c)(iVar9);
        iVar3 = FUN_402043d0();
        if ((iVar3 == 1) && (*(int *)(DAT_40201018 + 0x388) == 0)) {
          (*(code *)PTR_FUN_40201760)(iVar9,PTR_FUN_40226968,0);
          (*(code *)PTR_FUN_40201780)(iVar9,1000,0,1);
        }
        FUN_40229c40(iVar5,iVar1);
        if ((*DAT_40222028 != 0) &&
           (puVar4 = (undefined4 *)(*(code *)PTR_mem_alloc_fileline_40201764)(0x2c,puVar10,0x166),
           puVar4 != (undefined4 *)0x0)) {
          *puVar4 = 1;
          *(undefined1 *)((int)puVar4 + 0x2b) = uVar7;
          (*DAT_40201e88)((int)puVar4 + 0x25,iVar5 + 0x48,6);
          (*DAT_40201e88)(puVar4 + 1,DAT_40203e14,0x20);
          *(char *)(puVar4 + 9) = (char)*(undefined4 *)(DAT_4020101c + 0x220);
          iVar3 = (*DAT_40202c8c)(0x15,1,puVar4);
          if (iVar3 != 0) {
            (*(code *)PTR_mem_free_fileline_40201778)(puVar4,puVar10,0x171);
          }
        }
      }
    }
  }
  else if (param_2 == 3) {
    if ((uVar8 < 4) && (1 < uVar8)) {
      ieee80211_send_mgmt(iVar5,0,0);
    }
    else if ((uVar8 == 5) || (uVar8 == 7)) {
      ieee80211_send_mgmt(iVar5,0x20,0);
    }
  }
  else if (((param_2 == 5) && (uVar7 = 1, uVar8 < 4)) && (1 < uVar8)) {
    uVar6 = 0;
    if (*(char *)(*(int *)(DAT_4020101c + 0x10) + 0xd2) == '\x04') {
      *(undefined1 *)(*(int *)(DAT_4020101c + 0x10) + 0xd2) = 1;
      *(undefined1 *)(*(int *)(iVar3 + 0x10) + 0xd1) = 0;
    }
    FUN_40229ed4(iVar5,iVar1,param_3 == 0x10);
    *DAT_402278ec = uVar6;
    if ((char)param_1[0x91] == '\x03') {
      *(undefined1 *)(iVar1 + 0x18) = uVar7;
    }
    else {
      FUN_4023b50c();
    }
  }
  return 0;
}

