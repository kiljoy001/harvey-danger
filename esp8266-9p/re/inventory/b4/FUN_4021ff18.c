
void FUN_4021ff18(void)

{
  int iVar1;
  undefined2 *puVar2;
  int iVar3;
  uint uVar4;
  uint *puVar5;
  uint uVar6;
  undefined4 uVar7;
  int *piVar8;
  
  piVar8 = (int *)PTR_DAT_4021fe84;
  (*DAT_4021ff10)(DAT_4021fefc,0x48,DAT_4021fefc + 0x50,DAT_4021fef8,0x644,1,
                  *(int *)PTR_DAT_4021fe84 + 8,*(int *)PTR_DAT_4021fe84 + 0xc);
  uVar4 = DAT_40215c70;
  puVar2 = (undefined2 *)*piVar8;
  puVar5 = *(uint **)(puVar2 + 4);
  *puVar2 = 6;
  uVar6 = DAT_4021cf30;
  if (puVar5 != (uint *)0x0) {
    do {
      memw();
      memw();
      memw();
      *puVar5 = *puVar5 & uVar4 | (*puVar5 & 0xfff) - 4 & 0xfff;
      memw();
      memw();
      memw();
      *puVar5 = *puVar5 & uVar6 | (*puVar5 & 0xfff) << 0xc;
      (*(code *)PTR_FUN_4021ff14)(puVar5);
      puVar5 = (uint *)puVar5[2];
    } while (puVar5 != (uint *)0x0);
    puVar2 = (undefined2 *)*piVar8;
  }
  (*DAT_4021ff10)(DAT_4021ff00,0x60,DAT_4021ff00 + 0x60,0x20,4,0,puVar2 + 0x14,puVar2 + 0x16);
  iVar3 = *piVar8;
  uVar7 = 0;
  *(undefined4 *)(iVar3 + 0x20) = 0;
  *(undefined2 *)(iVar3 + 2) = 0;
  *(int *)(iVar3 + 0x1c) = iVar3 + 0x24;
  iVar1 = DAT_4021ff04;
  uVar6 = DAT_4021cf2c;
  memw();
  memw();
  *(uint *)(iVar3 + 0x18) = *(uint *)(iVar3 + 0x18) & uVar4 | 4;
  memw();
  memw();
  *(uint *)(iVar3 + 0x18) = *(uint *)(iVar3 + 0x18) & DAT_40216950 | DAT_4021ab3c;
  memw();
  memw();
  *(uint *)(iVar3 + 0x18) = *(uint *)(iVar3 + 0x18) & DAT_40215e48;
  memw();
  memw();
  *(uint *)(iVar3 + 0x18) = *(uint *)(iVar3 + 0x18) & uVar6;
  (*DAT_4021ff10)(iVar1,0x54,iVar1 + 0x60,0x700,0x100,1,iVar3 + 0x30,iVar3 + 0x34);
  iVar3 = *piVar8;
  *(undefined1 *)(iVar3 + 4) = 7;
  iVar1 = DAT_40205580;
  memw();
  *(int *)(DAT_40205580 + 0x280) = DAT_4021fefc;
  memw();
  *(undefined4 *)(iVar1 + 0x27c) = DAT_4021ff08;
  memw();
  *(int *)(iVar1 + 0x288) = DAT_4021ff04;
  memw();
  *(undefined4 *)(iVar1 + 0x284) = DAT_4021ff0c;
  memw();
  memw();
  *(uint *)(iVar1 + 0x200) = *(uint *)(iVar1 + 0x200) & 0xffffff00;
  memw();
  *(undefined4 *)(iVar1 + 0x208) = *(undefined4 *)(iVar3 + 8);
  memw();
  *(undefined4 *)(iVar1 + 0x20c) = *(undefined4 *)(iVar3 + 0x30);
  *(char *)(iVar3 + 5) = (char)uVar7;
  memw();
  *(undefined4 *)(iVar1 + 0x210) = uVar7;
  memw();
  memw();
  *(uint *)(iVar1 + 0x200) = *(uint *)(iVar1 + 0x200) & uVar6;
  return;
}

