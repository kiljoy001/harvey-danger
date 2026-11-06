
void FUN_4022013c(void)

{
  int iVar1;
  
  iVar1 = DAT_40201d2c;
  memw();
  *(undefined4 *)(DAT_40201d2c + 0x218) = 0;
  memw();
  *(undefined4 *)(iVar1 + 0x224) = 0xffffffff;
  wDev_Option_Init();
  FUN_40220664();
  FUN_4021ff18();
  FUN_402200bc();
  FUN_402200dc();
  iVar1 = DAT_40205580;
  memw();
  *(undefined4 *)(DAT_40201d2c + 0x218) = DAT_40220134;
  memw();
  memw();
  *(uint *)(iVar1 + 0x378) = *(uint *)(iVar1 + 0x378) | 2;
  memw();
  memw();
  *(uint *)(iVar1 + 0x204) = *(uint *)(iVar1 + 0x204) & DAT_40216950;
  *(undefined1 *)(DAT_40220138 + 0x80) = 1;
  return;
}

