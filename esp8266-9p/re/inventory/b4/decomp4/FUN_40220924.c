
int * FUN_40220924(int param_1,int param_2,undefined4 param_3)

{
  char cVar1;
  undefined2 uVar2;
  int *piVar3;
  uint uVar4;
  uint *puVar5;
  uint uVar6;
  int iVar7;
  undefined *puVar8;
  int *piVar9;
  
  if ((param_2 == 1) && (param_1 != 0)) {
    if (*DAT_4022091c == 0) {
      return (int *)0x0;
    }
    (*DAT_40201d38)();
    piVar9 = (int *)*DAT_4022091c;
    *DAT_4022091c = piVar9[8];
    piVar9[8] = 0;
    (*DAT_40201d40)();
    piVar3 = piVar9;
    (*DAT_402010d0)(piVar9[9],0,0x20);
    cVar1 = *(char *)(param_1 + 0xc);
    *piVar9 = param_1;
    if (cVar1 == '\0') {
      *(int **)(param_1 + 0x10) = piVar9;
    }
    uVar6 = DAT_4020be54;
    uVar2 = *(undefined2 *)(param_1 + 10);
    puVar5 = (uint *)piVar9[9];
    iVar7 = *(int *)(param_1 + 4);
    uVar4 = *puVar5;
    *(int *)(piVar9[1] + 4) = iVar7;
    *(undefined2 *)((int)piVar9 + 0x16) = uVar2;
    piVar9[4] = iVar7 + -0x24;
  }
  else {
    if (param_2 == 4) {
      puVar8 = PTR_s_esf_buf_c_40220920;
      piVar3 = (int *)(*(code *)PTR_mem_alloc_fileline_iram_40202384)
                                (0x28,PTR_s_esf_buf_c_40220920,0x157);
      if (piVar3 == (int *)0x0) {
        return (int *)0x0;
      }
      iVar7 = (*(code *)PTR_mem_alloc_fileline_iram_40202384)(0xc,puVar8,0x159);
      piVar3[1] = iVar7;
      piVar3[2] = iVar7;
      *(undefined2 *)(piVar3 + 3) = 1;
      iVar7 = (*(code *)PTR_mem_alloc_fileline_40201764)(0x20,puVar8,0x15c);
      piVar3[9] = iVar7;
      (*DAT_402010d0)(iVar7,0,0x20);
      uVar6 = *(uint *)piVar3[9];
      *(uint *)piVar3[9] = uVar6 & 0x3f | (uVar6 >> 6 | DAT_4021a2ac) << 6;
      iVar7 = (*(code *)PTR_mem_alloc_fileline_iram_40202384)(param_3,puVar8,0x15f);
      piVar3[4] = iVar7;
      *(int *)(piVar3[1] + 4) = iVar7;
      return piVar3;
    }
    if (param_2 == 5) {
      if (DAT_4022091c[2] == 0) {
        return (int *)0x0;
      }
      piVar3 = DAT_4022091c;
      (*DAT_40201d38)();
      piVar9 = (int *)piVar3[2];
      piVar3[2] = piVar9[8];
      piVar9[8] = 0;
      piVar3 = piVar9;
      (*DAT_40201d40)();
      (*DAT_402010d0)(piVar3[9],0,0x20);
      puVar5 = (uint *)piVar9[9];
      *(int *)(piVar9[1] + 4) = piVar9[4];
      uVar4 = *puVar5;
      uVar6 = DAT_40202794;
    }
    else {
      if (param_2 == 6) {
        puVar8 = PTR_s_esf_buf_c_40220920;
        piVar3 = (int *)(*(code *)PTR_mem_alloc_fileline_iram_40202384)
                                  (0x28,PTR_s_esf_buf_c_40220920,0x17c);
        if (piVar3 != (int *)0x0) {
          iVar7 = (*(code *)PTR_mem_alloc_fileline_iram_40202384)(0xc,puVar8,0x17f);
          piVar3[1] = iVar7;
          piVar3[2] = iVar7;
          *(undefined2 *)(piVar3 + 3) = 1;
          iVar7 = (*(code *)PTR_mem_alloc_fileline_iram_40202384)(0x20,puVar8,0x182);
          piVar3[9] = iVar7;
          (*DAT_402010d0)(iVar7,0,0x20);
          uVar6 = *(uint *)piVar3[9];
          *(uint *)piVar3[9] = uVar6 & 0x3f | (uVar6 >> 6 | DAT_40207b7c) << 6;
          iVar7 = (*(code *)PTR_mem_alloc_fileline_iram_40202384)(param_3,puVar8,0x185);
          piVar3[4] = iVar7;
          *(int *)(piVar3[1] + 4) = iVar7;
          return piVar3;
        }
        return (int *)0x0;
      }
      if (param_2 != 7) {
        return (int *)0x0;
      }
      if (DAT_4022091c[3] == 0) {
        return (int *)0x0;
      }
      piVar9 = DAT_4022091c;
      (*DAT_40201d38)(0);
      piVar3 = (int *)piVar9[3];
      piVar9[3] = piVar3[8];
      piVar3[8] = 0;
      (*DAT_40201d40)();
      puVar5 = (uint *)piVar3[9];
      uVar4 = *puVar5;
      uVar6 = DAT_40207b70;
    }
  }
  *puVar5 = uVar4 & 0x3f | (uVar4 >> 6 | uVar6) << 6;
  return piVar3;
}

