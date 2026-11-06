
void lmacInit(void)

{
  undefined2 uVar1;
  undefined *puVar2;
  
  puVar2 = PTR_lmacConfMib_00010674;
  *(undefined4 *)PTR_lmacConfMib_00010674 = 0x400;
  *(undefined4 *)(puVar2 + 4) = 0x200;
  *(undefined4 *)(puVar2 + 8) = 0x200;
  puVar2[0xd] = 0x20;
  puVar2[0xc] = 0x20;
  *(undefined2 *)(puVar2 + 0x14) = 9;
  uVar1 = (undefined2)DAT_00010678;
  *(undefined2 *)(puVar2 + 0x10) = uVar1;
  *(undefined2 *)(puVar2 + 0xe) = uVar1;
  *(undefined2 *)(puVar2 + 0x1c) = 0x15f;
  *(short *)(puVar2 + 0x1e) = (short)DAT_0001067c;
  (*(code *)PTR_lmacInitAc_00010684)(2,3,4,10,0);
  (*(code *)PTR_lmacInitAc_00010688)(3,7,4,10,0);
  (*(code *)PTR_lmacInitAc_0001068c)(1,2,3,4,DAT_00010680);
  (*(code *)PTR_lmacInitAc_00010690)(0,2,2,3,0x5e0);
  (*(code *)PTR_lmacInitAc_00010694)(4,2,4,10,0);
  (*(code *)PTR_lmacInitAc_00010698)(7,2,5,10,0);
  (*(code *)PTR_RC_SetBasicRate_0001069c)(*(undefined2 *)(puVar2 + 0x1c),1);
  (*(code *)PTR_rcAttach_000106a0)();
  return;
}

