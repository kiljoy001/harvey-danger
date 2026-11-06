
void esp_wifi_set_pm_config(byte *param_1)

{
  undefined *puVar1;
  byte bVar2;
  
  puVar1 = PTR_DAT_00010000;
  if (param_1 != (byte *)0x0) {
    bVar2 = *param_1;
    if (bVar2 < 0x10) {
      if (bVar2 < 3) {
        bVar2 = 3;
      }
      PTR_DAT_00010000[0x8a] = bVar2;
    }
    else {
      PTR_DAT_00010000[0x8a] = 0xf;
    }
    bVar2 = param_1[1];
    if (bVar2 < 0x21) {
      if (bVar2 < 0xd) {
        bVar2 = 0xd;
      }
      puVar1[0x8b] = bVar2;
    }
    else {
      puVar1[0x8b] = 0x20;
    }
    bVar2 = param_1[2];
    if (bVar2 < 0x65) {
      if (bVar2 < 10) {
        bVar2 = 10;
      }
      puVar1[0x86] = bVar2;
    }
    else {
      puVar1[0x86] = 100;
    }
    bVar2 = param_1[3];
    if (bVar2 < 0x15) {
      if (bVar2 == 0) {
        bVar2 = 1;
      }
      puVar1[0x87] = bVar2;
    }
    else {
      puVar1[0x87] = 0x14;
    }
    bVar2 = param_1[4];
    if (bVar2 < 0x65) {
      if (bVar2 == 0) {
        bVar2 = 1;
      }
      puVar1[0x88] = bVar2;
    }
    else {
      puVar1[0x88] = 100;
    }
    bVar2 = param_1[5];
    if (bVar2 < 0x65) {
      if (bVar2 == 0) {
        bVar2 = 1;
      }
      puVar1[0x89] = bVar2;
    }
    else {
      puVar1[0x89] = 100;
    }
    bVar2 = param_1[6];
    if (3 < bVar2) {
      bVar2 = 3;
    }
    puVar1[0x8c] = bVar2;
    puVar1[0x8d] = param_1[7];
  }
  return;
}

