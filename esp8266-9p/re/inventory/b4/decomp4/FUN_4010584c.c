
void FUN_4010584c(undefined4 param_1,uint *param_2)

{
  bool bVar1;
  uint uVar2;
  uint uVar3;
  uint *puVar4;
  byte bVar5;
  undefined4 uVar6;
  undefined4 extraout_a3;
  undefined4 uVar7;
  undefined4 extraout_a4;
  int iVar8;
  uint uVar9;
  uint uVar10;
  uint uVar11;
  uint *puVar12;
  uint uVar13;
  uint *puVar14;
  uint local_30;
  uint *puStack_10;
  
  uVar10 = DAT_401022c0;
  puStack_10 = param_2;
  do {
    memw();
    uVar9 = *(uint *)(DAT_40101abc + 0x220);
    if (uVar9 == 0) {
      return;
    }
    memw();
    uVar3 = *(uint *)(DAT_40101abc + 0x284);
    memw();
    *(uint *)(DAT_40101abc + 0x224) = uVar9;
    if ((uVar9 >> 0x1c & 1) != 0) {
      (*DAT_40100180)(PTR_s__s__u_4010556c,PTR_DAT_40105570,0x434);
      do {
                    /* WARNING: Do nothing block with infinite loop */
      } while( true );
    }
    if ((uVar9 & 0x8000000) != 0) {
      FUN_40105820(0);
    }
    if ((uVar9 & 0x4000000) != 0) {
      FUN_40105820(1);
    }
    if (((uVar9 & 8) == 0) || ((uVar9 & 4) != 0)) {
      uVar11 = uVar9 & 0x100;
      if (((uVar9 & 4) != 0) && ((uVar9 & 0x100) == 0)) {
        wdev_frame_process();
      }
    }
    else {
      FUN_401060b0();
      uVar11 = uVar9 & 0x100;
    }
    if (uVar11 != 0) {
      memw();
      uVar6 = *(undefined4 *)(DAT_40102780 + 0x21c);
      if (*(char *)(DAT_40105508 + 5) == '\0') {
        memw();
        puVar14 = *(uint **)(DAT_40102780 + 0x214);
        memw();
        uVar6 = *(undefined4 *)(DAT_40103ec0 + 0x2fc);
        memw();
        if (puVar14 == (uint *)0x0) {
          (*DAT_40100180)(PTR_s__s__u_4010556c,PTR_DAT_40105570,0x476,
                          *(undefined4 *)(DAT_40102780 + 0x218));
          do {
                    /* WARNING: Do nothing block with infinite loop */
          } while( true );
        }
        if (puVar14[2] == 0) {
          (*DAT_40100180)(PTR_s__s__u_4010556c,PTR_DAT_40105570,0x477);
          do {
                    /* WARNING: Do nothing block with infinite loop */
          } while( true );
        }
        puVar4 = *(uint **)(DAT_40105508 + 8);
        do {
          bVar5 = 1;
          for (; puVar4 != (uint *)0x0; puVar4 = (uint *)puVar4[2]) {
            memw();
            if ((*puVar4 & uVar10) >> 0x1e != 0) {
              if (puVar4 != (uint *)0x0) {
                puStack_10 = (uint *)puVar4[2];
              }
              break;
            }
            bVar5 = bVar5 + 1;
          }
          if (bVar5 < 2) {
            FUN_40105578(puVar4,1,uVar6,puVar14 == puVar4);
            *(int *)(DAT_40105508 + 0x164) = *(int *)(DAT_40105508 + 0x164) + 1;
          }
          else {
            if (puVar4 == (uint *)0x0) {
              memw();
              if ((*puVar14 & uVar10) >> 0x1e != 0) {
                FUN_4010550c(puVar14);
              }
              break;
            }
            FUN_4010550c(puVar4);
          }
          bVar1 = puVar14 != puVar4;
          puVar4 = puStack_10;
        } while (bVar1);
      }
      else {
        memw();
        puVar14 = *(uint **)(DAT_40102780 + 0x214);
        memw();
        memw();
        uVar7 = *(undefined4 *)(DAT_40102780 + 0x218);
        if (puVar14 == (uint *)0x0) {
          (*DAT_40100180)(PTR_s__s__u_4010556c,PTR_DAT_40105570,0x455,
                          *(undefined4 *)(DAT_40103ec0 + 0x2fc));
          do {
                    /* WARNING: Do nothing block with infinite loop */
          } while( true );
        }
        if (puVar14[2] == 0) {
          (*DAT_40100180)(PTR_s__s__u_4010556c,PTR_DAT_40105570,0x456);
          do {
                    /* WARNING: Do nothing block with infinite loop */
          } while( true );
        }
        memw();
        if ((*puVar14 & uVar10) >> 0x1e != 1) {
          (*DAT_40100180)(PTR_s__s__u_4010556c,PTR_DAT_40105570,0x457);
          do {
                    /* WARNING: Do nothing block with infinite loop */
          } while( true );
        }
        puVar4 = *(uint **)(DAT_40105508 + 8);
        while (puVar4 != (uint *)0x0) {
          memw();
          uVar11 = *puVar4;
          puVar12 = puVar4;
          while ((uVar11 & uVar10) >> 0x1e == 0) {
            puVar12 = (uint *)puVar12[2];
            memw();
            uVar11 = *puVar12;
          }
          FUN_40106200(puVar4,uVar6,uVar7);
          *(int *)(DAT_40105508 + 0x164) = *(int *)(DAT_40105508 + 0x164) + 1;
          if (puVar14 == puVar12) break;
          uVar6 = extraout_a3;
          uVar7 = extraout_a4;
          puVar4 = (uint *)puVar12[2];
        }
      }
    }
    if ((DAT_4010583c & uVar9) != 0) {
      FUN_40103d88(uVar3 >> 0xc & 0xf);
    }
    if ((uVar9 & 0x80000) == 0) {
      local_30 = uVar9 & DAT_40100544;
    }
    else {
      memw();
      if ((*(uint *)(DAT_40103ec0 + 4) & 0x1000000) == 0) {
        uVar11 = 0x7f;
      }
      else {
        uVar11 = *(uint *)(DAT_40103ec0 + 4) >> 0x10;
      }
      uVar13 = uVar3 >> 0xc & 0xf;
      if ((7 < uVar13) && (uVar13 != 10)) {
        (*DAT_40100180)(PTR_s__s__u_4010556c,PTR_DAT_40105570,0x4b3);
        do {
                    /* WARNING: Do nothing block with infinite loop */
        } while( true );
      }
      local_30 = uVar9 & DAT_40100544;
      if ((*DAT_40105840 == 0) && ((uVar9 & DAT_40100544) != 0)) {
        FUN_40102f44(uVar13);
        uVar9 = uVar9 & DAT_40105844;
        local_30 = uVar9 & DAT_40100544;
      }
      uVar2 = uVar3 >> 0x1c;
      if (uVar2 == 0) {
        FUN_401030f0(uVar13,uVar11 & 0xff);
      }
      else if (uVar2 == 1) {
        FUN_40103d94(uVar3 >> 0x10 & 0xff,uVar13);
      }
      else if (uVar2 == 2) {
        FUN_40103cf8(uVar13);
      }
      else if (uVar2 == 4) {
        FUN_40103e34(uVar3 >> 0x10 & 0xff);
      }
      else {
        if (uVar2 != 5) {
          (*DAT_40100180)(PTR_s__s__u_4010556c,PTR_DAT_40105570,0x4ce);
          do {
                    /* WARNING: Do nothing block with infinite loop */
          } while( true );
        }
        FUN_40103d30();
      }
      iVar8 = uVar13 * 4 + DAT_40105848;
      *(int *)(iVar8 + 0x30) = *(int *)(iVar8 + 0x30) + 1;
    }
    if (local_30 != 0) {
      uVar3 = uVar3 >> 0xc & 0xf;
      if ((7 < uVar3) && (uVar3 != 10)) {
        (*DAT_40100180)(PTR_s__s__u_4010556c,PTR_DAT_40105570,0x4d5);
        do {
                    /* WARNING: Do nothing block with infinite loop */
        } while( true );
      }
      FUN_40102f44(uVar3);
    }
    if ((uVar9 & 0x20000000) != 0) {
      FUN_40103884();
    }
    if ((uVar9 & 0x100000) != 0) {
      FUN_40103898();
    }
    if ((uVar9 & 0x200) != 0) {
      *(int *)(DAT_40105508 + 0x194) = *(int *)(DAT_40105508 + 0x194) + 1;
    }
    if ((uVar9 >> 0x17 & 1) != 0) {
      *(int *)(DAT_40105508 + 0x198) = *(int *)(DAT_40105508 + 0x198) + 1;
    }
  } while( true );
}

