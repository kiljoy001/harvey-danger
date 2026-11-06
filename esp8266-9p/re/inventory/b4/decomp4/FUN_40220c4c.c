
void FUN_40220c4c(void)

{
  int iVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  undefined4 uVar5;
  uint *puVar6;
  
  iVar4 = 0x60;
  uVar5 = 1;
  iVar2 = DAT_40220c40;
  iVar3 = DAT_40220c3c;
  do {
    *(short *)(iVar2 + 0xc) = (short)uVar5;
    *(int *)(iVar2 + 0x24) = iVar3;
    iVar1 = iVar4 + *(int *)(DAT_4021dfe8 + 0x28) + -0xc;
    *(undefined4 *)(iVar2 + 0x10) = *(undefined4 *)(*(int *)(DAT_4021dfe8 + 0x28) + 4);
    *(int *)(iVar2 + 4) = iVar1;
    *(int *)(iVar2 + 8) = iVar1;
    FUN_40220b0c(iVar2,1);
    iVar2 = iVar2 + 0x28;
    iVar3 = iVar3 + 0x20;
    iVar4 = iVar4 + -0xc;
  } while (0 < iVar4);
  iVar4 = 8;
  puVar6 = DAT_40220c44;
  do {
    memw();
    memw();
    *puVar6 = *puVar6 & DAT_40215c70 | 0x40;
    *(uint **)(iVar2 + 4) = puVar6;
    *(uint **)(iVar2 + 8) = puVar6;
    *(int *)(iVar2 + 0x24) = iVar3;
    *(undefined2 *)(iVar2 + 0xc) = 1;
    *(uint **)(iVar2 + 0x10) = puVar6 + 3;
    FUN_40220b0c(iVar2,5);
    iVar2 = iVar2 + 0x28;
    puVar6 = puVar6 + 0x13;
    iVar3 = iVar3 + 0x20;
    iVar4 = iVar4 + -1;
  } while (0 < iVar4);
  iVar4 = 4;
  do {
    *(int *)(iVar2 + 0x24) = iVar3;
    FUN_40220b0c(iVar2,7);
    iVar2 = iVar2 + 0x28;
    iVar3 = iVar3 + 0x20;
    iVar4 = iVar4 + -1;
  } while (0 < iVar4);
  iVar4 = 7;
  iVar3 = DAT_40220c48;
  do {
    *(int *)(iVar2 + 0x24) = iVar3;
    FUN_40220b0c(iVar2,8);
    iVar2 = iVar2 + 0x28;
    iVar3 = iVar3 + 0xc;
    iVar4 = iVar4 + -1;
  } while (0 < iVar4);
  return;
}

