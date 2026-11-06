
undefined4 FUN_40220fac(int param_1)

{
  int iVar1;
  undefined4 uVar2;
  int iVar3;
  undefined4 uVar4;
  undefined *puVar5;
  
  (*(code *)PTR_FUN_40201448)(PTR_s_mode___40220f88);
  if (param_1 == 0) {
    (*(code *)PTR_FUN_40201448)(PTR_DAT_40220f98);
LAB_40221061:
    uVar4 = 1;
    goto LAB_40221063;
  }
  iVar3 = DAT_4020101c;
  puVar5 = PTR_s_ieee80211_c_40220f8c;
  if (param_1 == 1) {
LAB_40220fdc:
    (*(code *)PTR_FUN_40201448)(PTR_DAT_40220f90);
    (*(code *)PTR_FUN_40201448)
              (PTR_s___02x__02x__02x__02x__02x__02x__40220f94,*(undefined1 *)(DAT_402056d0 + 0x1e),
               *(undefined1 *)(DAT_402056d0 + 0x1f),*(undefined1 *)(DAT_402056d0 + 0x20),
               *(undefined1 *)(DAT_402056d0 + 0x21),*(undefined1 *)(DAT_402056d0 + 0x22),
               *(undefined1 *)(DAT_402056d0 + 0x23));
    uVar4 = 1;
    if (*(int *)(iVar3 + 0x10) == 0) {
      iVar1 = (*(code *)PTR_FUN_40201764)(0x2a0,puVar5,0x22e);
      *(int *)(iVar3 + 0x10) = iVar1;
      if (iVar1 == 0) {
        uVar4 = 0;
      }
      else {
        *(undefined4 *)(iVar1 + 200) = 0;
        *(undefined4 *)(*(int *)(iVar3 + 0x10) + 0x44) = 0;
        uVar4 = (*(code *)PTR_FUN_40201764)(0x110,puVar5,0x234);
        *(undefined4 *)(*(int *)(iVar3 + 0x10) + 0x94) = uVar4;
        iVar1 = *(int *)(*(int *)(iVar3 + 0x10) + 0x94);
        if (iVar1 == 0) {
          (*(code *)PTR_FUN_40201778)(*(int *)(iVar3 + 0x10),puVar5,0x23a);
          uVar4 = 0;
          *(undefined4 *)(iVar3 + 0x10) = 0;
        }
        else {
          (*DAT_40201e88)(iVar1,PTR_DAT_40204cd4,6);
          uVar4 = 1;
        }
      }
    }
    if (param_1 == 1) {
      if (*(int *)(iVar3 + 0x14) != 0) {
        (*(code *)PTR_FUN_40201778)(*(undefined4 *)(*(int *)(iVar3 + 0x14) + 0x98),puVar5,0x244);
        (*(code *)PTR_FUN_40201778)(*(undefined4 *)(iVar3 + 0x14),puVar5,0x245);
        *(undefined4 *)(iVar3 + 0x14) = 0;
      }
      goto LAB_40221063;
    }
    (*(code *)PTR_FUN_40201448)(PTR_DAT_40220fa8);
  }
  else {
    if (param_1 != 2) {
      if (param_1 != 3) goto LAB_40221061;
      goto LAB_40220fdc;
    }
    uVar4 = 1;
  }
  (*(code *)PTR_FUN_40201448)(PTR_s_softAP_40220fa0);
  (*(code *)PTR_FUN_40201448)
            (PTR_s___02x__02x__02x__02x__02x__02x__40220fa4,*(undefined1 *)(DAT_402056d0 + 0x18),
             *(undefined1 *)(DAT_402056d0 + 0x19),*(undefined1 *)(DAT_402056d0 + 0x1a),
             *(undefined1 *)(DAT_402056d0 + 0x1b),*(undefined1 *)(DAT_402056d0 + 0x1c),
             *(undefined1 *)(DAT_402056d0 + 0x1d));
  if (*(int *)(iVar3 + 0x14) == 0) {
    iVar1 = (*(code *)PTR_FUN_40201764)(0x2a0,puVar5,0x251);
    *(int *)(iVar3 + 0x14) = iVar1;
    if (iVar1 == 0) {
      uVar4 = 0;
    }
    else {
      *(undefined4 *)(iVar1 + 200) = 1;
      uVar2 = (*(code *)PTR_FUN_40201764)(0x110,puVar5,599);
      *(undefined4 *)(*(int *)(iVar3 + 0x14) + 0x98) = uVar2;
      iVar1 = *(int *)(*(int *)(iVar3 + 0x14) + 0x98);
      if (iVar1 == 0) {
        (*(code *)PTR_FUN_40201778)(*(int *)(iVar3 + 0x14),puVar5,0x25d);
        uVar4 = 0;
        *(undefined4 *)(iVar3 + 0x14) = 0;
      }
      else {
        (*DAT_40201e88)(iVar1,PTR_DAT_40204cd4,6);
      }
    }
  }
  if ((param_1 == 2) && (*(int *)(iVar3 + 0x10) != 0)) {
    (*(code *)PTR_FUN_40201778)(*(undefined4 *)(*(int *)(iVar3 + 0x10) + 0x94),puVar5,0x268);
    uVar2 = 0;
    (*(code *)PTR_FUN_40201778)(*(undefined4 *)(iVar3 + 0x10),puVar5,0x269);
    *(undefined4 *)(iVar3 + 0x10) = uVar2;
  }
LAB_40221063:
  (*(code *)PTR_FUN_40201448)(PTR_DAT_40220f9c);
  return uVar4;
}

