
void FUN_40221810(void)

{
  int iVar1;
  int iVar2;
  int *piVar3;
  int iVar4;
  undefined *puVar5;
  
  iVar4 = *(int *)(DAT_4020101c + 0x14);
  iVar2 = *(int *)(iVar4 + 0xcc);
  *(undefined1 *)(iVar4 + 0x54) = 0;
  *(uint *)(iVar4 + 0x50) = *(uint *)(iVar4 + 0x50) & 0xffffffef;
  *(undefined4 *)(*(int *)(iVar4 + 0x98) + 8) = 0;
  if (iVar2 != 0) {
    piVar3 = *(int **)(iVar2 + 0x18);
    puVar5 = PTR_s_ieee80211_hostap_c_402216ac;
    if (piVar3[0x14] != 0) {
      (*(code *)PTR_FUN_40201778)(piVar3[0x14],PTR_s_ieee80211_hostap_c_402216ac,0x1ab);
      iVar2 = *(int *)(iVar4 + 0xcc);
      piVar3 = *(int **)(iVar2 + 0x18);
    }
    if (*piVar3 != 0) {
      (*(code *)PTR_FUN_40201778)(*piVar3,puVar5,0x1af);
      iVar2 = *(int *)(iVar4 + 0xcc);
      piVar3 = *(int **)(iVar2 + 0x18);
    }
    if (piVar3 != (int *)0x0) {
      (*(code *)PTR_FUN_40201778)(piVar3,puVar5,0x1b3);
      iVar2 = *(int *)(iVar4 + 0xcc);
    }
    iVar1 = *(int *)(iVar2 + 4);
    if (*(int *)(iVar1 + 0x38) != 0) {
      (*(code *)PTR_FUN_40201778)(*(int *)(iVar1 + 0x38),puVar5,0x1b7);
      iVar2 = *(int *)(iVar4 + 0xcc);
      iVar1 = *(int *)(iVar2 + 4);
    }
    if (*(int *)(iVar1 + 0x3c) != 0) {
      (*(code *)PTR_FUN_40201778)(*(int *)(iVar1 + 0x3c),puVar5,0x1bb);
      iVar2 = *(int *)(iVar4 + 0xcc);
      iVar1 = *(int *)(iVar2 + 4);
    }
    if (iVar1 != 0) {
      (*(code *)PTR_FUN_40201778)(iVar1,puVar5,0x1bf);
      iVar2 = *(int *)(iVar4 + 0xcc);
    }
    if (iVar2 != 0) {
      (*(code *)PTR_FUN_40201778)(iVar2,puVar5,0x1c3);
      *(undefined4 *)(iVar4 + 0xcc) = 0;
    }
  }
  return;
}

