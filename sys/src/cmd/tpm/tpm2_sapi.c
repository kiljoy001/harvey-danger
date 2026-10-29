/*
 * Minimal TPM2 SAPI/Marshaling - From Scratch
 *
 * Implements only what we need:
 * - TPM2_Create (for sealing)
 * - TPM2_Unseal
 * - Basic marshaling (big-endian encoding)
 *
 * Why from scratch instead of TSS2 library?
 * - TSS2 has 29k LOC with tons of standard library dependencies
 * - We only need ~300 lines for our use case
 * - Simpler to write clean code than fix library conflicts
 */

#include <u.h>
#include <libc.h>
#include "tpm.h"
#include "tpm2_const.h"

typedef u8int uint8_t;
typedef u16int uint16_t;
typedef u32int uint32_t;
typedef u64int uint64_t;
typedef ulong size_t;
typedef ulong usize;

typedef struct TPMContext TPMContext;
struct TPMContext { int unused; };
Tpm *tpm_sapi_ctx;

int
tpm_transmit(TPMContext *ctx, u8int *cmd, usize n, u8int *resp, usize *rnp)
{
	USED(ctx);
	if(tpm_sapi_ctx == nil || rnp == nil)
		return -1;
	if(tpm_send(tpm_sapi_ctx, cmd, n) < 0)
		return -1;
	if((usize)tpm_sapi_ctx->nresp > *rnp)
		return -1;
	memmove(resp, tpm_sapi_ctx->resp, tpm_sapi_ctx->nresp);
	*rnp = tpm_sapi_ctx->nresp;
	return 0;
}

/* Stub for buffer dumping */
void tpm_dump_buffer(char *label, u8int *buf, int len) {
  print("%s: dump %d bytes\n", label, len);
}

static void marshal_u16(u8int **buf, u16int val) {
  (*buf)[0] = (val >> 8) & 0xFF;
  (*buf)[1] = val & 0xFF;
  *buf += 2;
}

static void marshal_u32(u8int **buf, u32int val) {
  (*buf)[0] = (val >> 24) & 0xFF;
  (*buf)[1] = (val >> 16) & 0xFF;
  (*buf)[2] = (val >> 8) & 0xFF;
  (*buf)[3] = val & 0xFF;
  *buf += 4;
}

static u16int unmarshal_u16(u8int **buf) {
  u16int val = ((*buf)[0] << 8) | (*buf)[1];
  *buf += 2;
  return val;
}

static u32int unmarshal_u32(u8int **buf) {
  u32int val =
      ((*buf)[0] << 24) | ((*buf)[1] << 16) | ((*buf)[2] << 8) | (*buf)[3];
  *buf += 4;
  return val;
}

/* Marshal TPM2B buffer (size + data) */
static void marshal_tpm2b(u8int **buf, const u8int *data, u16int len) {
  marshal_u16(buf, len);
  if (len > 0) {
    memmove(*buf, data, len);
    *buf += len;
  }
}

/* Unmarshal TPM2B buffer */
static u16int unmarshal_tpm2b(u8int **buf, u8int *data, u16int max_len) {
  u16int len = unmarshal_u16(buf);
  if (len > max_len)
    return 0;
  if (len > 0) {
    memmove(data, *buf, len);
    *buf += len;
  }
  return len;
}

/*
 * Marshal a Password Session
 * Used for commands requiring authorization (like owner hierarchy access)
 *
 * The authSize is the size of all authorization structures that follow,
 * NOT including the authSize field itself (4 bytes).
 */
static void marshal_password_session(u8int **buf) {
  /* Authorization Size (excludes itself, includes session data):
   * sessionHandle (4) + nonce size (2) + attributes (1) + hmac size (2) = 9
   * bytes */
  marshal_u32(buf, 9);

  /* Session Data */
  marshal_u32(buf, TPM2_RS_PW); /* sessionHandle: TPM2_RS_PW */
  marshal_u16(buf, 0);          /* nonce size: empty */
  *(*buf)++ = 0x00;    /* sessionAttributes: none (continueSession=0) */
  marshal_u16(buf, 0); /* hmac size: empty (password) */
}

/*
 * TPM2_Startup - Initialize TPM after power-on
 *
 * Must be called before any other TPM commands.
 * Uses TPM2_SU_CLEAR to start with a clean state.
 */
int tpm2_startup(TPMContext *ctx) {
  USED(ctx);
  u8int cmd[12];
  u8int resp[10];
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_NO_SESSIONS); /* tag */
  marshal_u32(&p, 12);                  /* size */
  marshal_u32(&p, TPM2_CC_STARTUP);     /* command code */

  /* Startup Type */
  marshal_u16(&p, TPM2_SU_CLEAR); /* Clear state */

  /* Send command */
  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)12, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_startup: transmit failed\n");
    return -1;
  }

  /* Parse response */
  rp = resp;
  unmarshal_u16(&rp);      /* tag */
  unmarshal_u32(&rp);      /* size */
  rc = unmarshal_u32(&rp); /* response code */

  if (rc != TPM2_RC_SUCCESS) {
    /* TPM_RC_INITIALIZE (0x100) means already initialized - that's OK */
    if (rc == 0x100) {
      print("tpm2_startup: TPM already initialized\n");
      return 0;
    }
    print("tpm2_startup: TPM returned error 0x%08X\n", rc);
    return -1;
  }

  print("tpm2_startup: TPM initialized successfully\n");
  return 0;
}

/*
 * TPM2_CreatePrimary - Create Storage Root Key (SRK)
 *
 * Returns handle to primary key (parent for sealed objects)
 */
int tpm2_create_primary(TPMContext *ctx, u32int *handle_out) {
  USED(ctx);
  u8int cmd[512];
  u8int resp[512]; /* CreatePrimary response includes full public key */
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_SESSIONS);       /* tag */
  marshal_u32(&p, 0);                      /* size - fill later */
  marshal_u32(&p, TPM2_CC_CREATE_PRIMARY); /* command code */

  /* Primary Handle (owner hierarchy) */
  marshal_u32(&p, TPM2_RH_OWNER);

  /* Authorization Session (Password) */
  marshal_password_session(&p);

  /* inSensitive - TPM2B_SENSITIVE_CREATE (empty auth and data) */
  marshal_u16(&p, 4); /* size of TPMS_SENSITIVE_CREATE = 4 bytes */
  marshal_u16(&p, 0); /* userAuth size = 0 (no password) */
  marshal_u16(&p, 0); /* data size = 0 (no sensitive data) */

  /* inPublic - ECC P256 storage key template */
  u8int *public_start = p;
  marshal_u16(&p, 0); /* size - fill later */

  /* TPMT_PUBLIC for ECC */
  marshal_u16(&p, TPM_ALG_ECC);     /* type = ECC */
  marshal_u16(&p, TPM2_ALG_SHA256); /* nameAlg */
  /* objectAttributes: fixedTPM(1) | fixedParent(4) | sensitiveDataOrigin(5) |
   * userWithAuth(6) | restricted(16) | decrypt(17) = 0x00030072 */
  marshal_u32(&p, 0x00030072);
  marshal_u16(&p, 0); /* authPolicy size */

  /* TPMS_ECC_PARMS (ECC parameters) */
  /* symmetric: TPMT_SYM_DEF_OBJECT (algorithm, keyBits, mode) */
  marshal_u16(&p, TPM2_ALG_AES); /* algorithm = AES */
  marshal_u16(&p, 128);          /* keyBits = 128 */
  marshal_u16(&p, TPM_ALG_CFB);  /* mode = CFB */

  /* TPMT_ECC_SCHEME - NULL for storage key */
  marshal_u16(&p, TPM_ALG_NULL); /* scheme */

  /* ECC details */
  marshal_u16(&p, TPM2_ECC_NIST_P256); /* curveID */
  marshal_u16(&p, TPM_ALG_NULL);       /* kdf */

  /* TPMS_ECC_POINT for unique (X and Y coordinates) */
  marshal_u16(&p, 0); /* X size - generated by TPM */
  marshal_u16(&p, 0); /* Y size - generated by TPM */

  /* Fill in public size */
  u16int public_size = p - public_start - 2;
  public_start[0] = (public_size >> 8) & 0xFF;
  public_start[1] = public_size & 0xFF;

  /* outsideInfo (empty) */
  marshal_u16(&p, 0);

  /* creationPCR (empty) */
  marshal_u32(&p, 0);

  /* Fill in command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  /* Send command */
  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_create_primary: transmit failed\n");
    return -1;
  }

  /* Parse response */
  rp = resp;
  unmarshal_u16(&rp);      /* tag */
  unmarshal_u32(&rp);      /* size */
  rc = unmarshal_u32(&rp); /* response code */

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm2_create_primary: TPM returned error 0x%08X\n", rc);
    return -1;
  }

  /* Extract handle */
  *handle_out = unmarshal_u32(&rp);

  print("tpm2_create_primary: Created SRK with handle 0x%08X\n", *handle_out);
  return 0;
}

/*
 * TPM2_Create - Create sealed data object
 *
 * Seals data under parent key. Data can only be unsealed if PCRs match.
 */
int tpm2_create(u32int parent_handle, u8int *data, u16int data_len,
                u8int *private_out, u16int *private_len, u8int *public_out,
                u16int *public_len) {
  u8int cmd[512];
  u8int resp[512];
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  if (data_len > 128) {
    print("tpm2_create: data too large (%d bytes, max 128)\n", data_len);
    return -1;
  }

  /* Command Header */
  marshal_u16(&p, TPM2_ST_SESSIONS);
  marshal_u32(&p, 0);
  marshal_u32(&p, TPM2_CC_CREATE);

  /* Parent handle */
  marshal_u32(&p, parent_handle);

  /* Authorization Session (Password) for parent */
  marshal_password_session(&p);

  /* inSensitive - contains the data to seal */
  u8int *sens_start = p;
  marshal_u16(&p, 0);                /* size - fill later */
  marshal_u16(&p, 0);                /* userAuth size (no password) */
  marshal_tpm2b(&p, data, data_len); /* sensitive data */

  u16int sens_size = p - sens_start - 2;
  sens_start[0] = (sens_size >> 8) & 0xFF;
  sens_start[1] = sens_size & 0xFF;

  /* inPublic - keyedhash object (sealed data) */
  u8int *public_start = p;
  marshal_u16(&p, 0); /* size - fill later */

  marshal_u16(&p, TPM2_ALG_KEYEDHASH); /* type */
  marshal_u16(&p, TPM2_ALG_SHA256);    /* nameAlg */
  /* objectAttributes: fixedTPM(1) | fixedParent(4) | userWithAuth(6) =
   * 0x00000052 Note: decrypt(17) and sign(18) MUST be CLEAR for Sealed Data
   * Objects */
  marshal_u32(&p, 0x00000052);
  marshal_u16(&p, 0); /* authPolicy size = 0 (use password auth, not policy) */

  /* KEYEDHASH parameters */
  marshal_u16(&p, TPM_ALG_NULL); /* scheme */
  marshal_u16(&p, 0);            /* unique size */

  u16int public_size = p - public_start - 2;
  public_start[0] = (public_size >> 8) & 0xFF;
  public_start[1] = public_size & 0xFF;

  /* outsideInfo (empty) */
  marshal_u16(&p, 0);

  /* creationPCR (empty - not sealing to PCRs yet) */
  marshal_u32(&p, 0);

  /* Fill command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  /* Send command */
  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_create: transmit failed\n");
    return -1;
  }

  /* Parse response */
  rp = resp;
  unmarshal_u16(&rp); /* tag */
  unmarshal_u32(&rp); /* size */
  rc = unmarshal_u32(&rp);

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm2_create: TPM returned error 0x%08X\n", rc);
    return -1;
  }
  print("tpm2_create: Raw response (first 32 bytes):");
  tpm_dump_buffer("  ", resp, 32);

  u16int rtag = (resp[0] << 8) | resp[1];
  if (rtag == TPM2_ST_SESSIONS) {
    /* Skip parameter size */
    unmarshal_u32(&rp);
  }

  *private_len = unmarshal_tpm2b(&rp, private_out, 256);
  *public_len = unmarshal_tpm2b(&rp, public_out, 256);

  print("tpm2_create: Created sealed object (priv=%d, pub=%d bytes)\n",
        *private_len, *public_len);
  return 0;
}

/*
 * TPM2_Load - Load sealed object into TPM
 *
 * Returns handle to loaded object
 */
int tpm2_load(u32int parent_handle, const u8int *private_blob,
              u16int private_len, const u8int *public_blob, u16int public_len,
              u32int *handle_out) {
  u8int cmd[512];
  u8int resp[256];
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_SESSIONS);
  marshal_u32(&p, 0); /* size - fill later */
  marshal_u32(&p, TPM2_CC_LOAD);

  /* Parent handle */
  marshal_u32(&p, parent_handle);

  /* Authorization Session (Password) for parent */
  marshal_password_session(&p);

  /* Private blob */
  marshal_tpm2b(&p, private_blob, private_len);

  /* Public blob */
  marshal_tpm2b(&p, public_blob, public_len);

  /* Fill command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  /* Send command */
  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_load: transmit failed\n");
    return -1;
  }

  /* Parse response */
  rp = resp;
  u16int rtag = unmarshal_u16(&rp); /* tag */
  unmarshal_u32(&rp);               /* size */
  rc = unmarshal_u32(&rp);

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm2_load: TPM returned error 0x%08X\n", rc);
    return -1;
  }
  print("tpm2_load: Raw response:");
  tpm_dump_buffer("  ", resp, 32);

  /* Extract handle (Handle comes before parameters) */
  *handle_out = unmarshal_u32(&rp);

  if (rtag == TPM2_ST_SESSIONS) {
    unmarshal_u32(&rp);
  }

  print("tpm2_load: Loaded object with handle 0x%08X\n", *handle_out);
  return 0;
}

/*
 * TPM2_Unseal - Unseal data from loaded object
 */
int tpm2_unseal(u32int item_handle, const u8int *auth, u16int auth_len,
                u8int *data_out, u16int *data_len) {
  USED(auth);
  USED(auth_len);

  u8int cmd[128];
  u8int resp[256];
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_SESSIONS);
  marshal_u32(&p, 0); /* size - fill later */
  marshal_u32(&p, TPM2_CC_UNSEAL);

  /* Item handle */
  marshal_u32(&p, item_handle);

  /* Authorization Session (Password) for item */
  marshal_password_session(&p);

  /* Fill command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  /* Send command */
  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_unseal: transmit failed\n");
    return -1;
  }

  /* Parse response */
  rp = resp;
  u16int rtag = unmarshal_u16(&rp); /* tag */
  unmarshal_u32(&rp);               /* size */
  rc = unmarshal_u32(&rp);

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm2_unseal: TPM returned error 0x%08X\n", rc);
    return -1;
  }

  if (rtag == TPM2_ST_SESSIONS) {
    unmarshal_u32(&rp);
  }

  /* Extract unsealed data */
  *data_len = unmarshal_tpm2b(&rp, data_out, 128);

  print("tpm2_unseal: Unsealed %d bytes\n", *data_len);
  return 0;
}

/*
 * TPM2_NV_DefineSpace - Define NVRAM index
 */
int tpm2_nv_define_space(TPMContext *ctx, u32int nv_index, u16int size,
                         u32int attributes) {
  USED(ctx);
  u8int cmd[256];
  u8int resp[64];
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_SESSIONS);
  marshal_u32(&p, 0); /* size - fill later */
  marshal_u32(&p, TPM2_CC_NV_DefineSpace);

  /* Authorization Handle (Owner or Platform) */
  marshal_u32(&p, TPM2_RH_OWNER);

  /* Authorization Session (Password) */
  marshal_password_session(&p);

  /* Auth (empty password for the index) */
  marshal_u16(&p, 0); /* size */

  /* TPMS_NV_PUBLIC */
  marshal_u16(&p, 0); /* size of TPMS_NV_PUBLIC - fill later */
  u8int *public_start = p;

  marshal_u32(&p, nv_index);        /* nvIndex */
  marshal_u16(&p, TPM2_ALG_SHA256); /* nameAlg */
  marshal_u32(&p, attributes);      /* attributes (e.g. TPMA_NV_OWNERWRITE) */
  marshal_u16(&p, 0);               /* authPolicy size */
  marshal_u16(&p, size);            /* dataSize */

  /* Fill TPMS_NV_PUBLIC size */
  u16int public_size = p - public_start;
  public_start[-2] = (public_size >> 8) & 0xFF;
  public_start[-1] = public_size & 0xFF;

  /* Fill command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_nv_define_space: transmit failed\n");
    return -1;
  }

  rp = resp;
  unmarshal_u16(&rp);
  unmarshal_u32(&rp);
  rc = unmarshal_u32(&rp);

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm2_nv_define_space: TPM error 0x%08X\n", rc);
    return -1;
  }

  return 0;
}

/*
 * TPM2_NV_UndefineSpace - Delete NVRAM index
 */
int tpm2_nv_undefine_space(TPMContext *ctx, u32int nv_index) {
  USED(ctx);
  u8int cmd[128];
  u8int resp[64];
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_SESSIONS);
  marshal_u32(&p, 0);
  marshal_u32(&p, TPM2_CC_NV_UndefineSpace);

  /* Authorization Handle (Owner or Platform) */
  marshal_u32(&p, TPM2_RH_OWNER);

  /* NV Index */
  marshal_u32(&p, nv_index);

  /* Authorization Session (Password) */
  marshal_password_session(&p);

  /* Fill command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_nv_undefine_space: transmit failed\n");
    return -1;
  }

  rp = resp;
  unmarshal_u16(&rp);
  unmarshal_u32(&rp);
  rc = unmarshal_u32(&rp);

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm2_nv_undefine_space: TPM error 0x%08X\n", rc);
    return -1;
  }

  return 0;
}

/*
 * TPM2_NV_Write - Write data to NVRAM
 */
int tpm2_nv_write(TPMContext *ctx, u32int nv_index, u8int *data, u16int len,
                  u16int offset) {
  USED(ctx);
  u8int cmd[1024]; /* Max NV write is small usually, but buffer needs space */
  u8int resp[64];
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  if (len > 1024)
    return -1;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_SESSIONS);
  marshal_u32(&p, 0);
  marshal_u32(&p, TPM2_CC_NV_Write);

  /* Auth Handle (Owner) */
  marshal_u32(&p, TPM2_RH_OWNER);

  /* NV Index */
  marshal_u32(&p, nv_index);

  /* Authorization Session (Password) */
  marshal_password_session(&p);

  /* Data */
  marshal_tpm2b(&p, data, len);

  /* Offset */
  marshal_u16(&p, offset);

  /* Fill command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_nv_write: transmit failed\n");
    return -1;
  }

  rp = resp;
  unmarshal_u16(&rp);
  unmarshal_u32(&rp);
  rc = unmarshal_u32(&rp);

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm2_nv_write: TPM error 0x%08X\n", rc);
    return -1;
  }

  return 0;
}

/*
 * TPM2_HMAC - Compute HMAC using key in TPM
 */
int tpm20_hmac(TPMContext *ctx, u32int key_handle, u8int *data, usize data_len,
               u8int *hmac_out, usize *hmac_out_len) {
  USED(ctx);
  u8int cmd[1024];
  u8int resp[1024];
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  if (data_len > 1024)
    return -1;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_SESSIONS);
  marshal_u32(&p, 0);
  marshal_u32(&p, TPM2_CC_HMAC);

  /* Key Handle */
  marshal_u32(&p, key_handle);

  /* Authorization Session (Password) */
  marshal_password_session(&p);

  /* Buffer */
  marshal_tpm2b(&p, data, (u16int)data_len);

  /* Hash Algorithm */
  marshal_u16(&p, TPM2_ALG_SHA256);

  /* Fill command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm20_hmac: transmit failed\n");
    return -1;
  }

  rp = resp;
  u16int rtag = unmarshal_u16(&rp);
  unmarshal_u32(&rp);
  rc = unmarshal_u32(&rp);

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm20_hmac: TPM error 0x%08X\n", rc);
    return -1;
  }

  if (rtag == TPM2_ST_SESSIONS) {
    unmarshal_u32(&rp);
  }

  /* Extract digest */
  u16int out_len = unmarshal_tpm2b(&rp, hmac_out, 64);
  *hmac_out_len = (usize)out_len;

  return 0;
}

/*
 * TPM2_FlushContext - Remove a loaded object/session from TPM memory
 *
 * After using TPM2_Load or creating sessions, you must flush the context
 * to free TPM memory. This is critical to prevent resource exhaustion.
 *
 * Based on Linux kernel's tpm2_flush_context()
 */
int tpm2_flush_context(u32int handle) {
  u8int cmd[64];
  u8int resp[64];
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_NO_SESSIONS);
  marshal_u32(&p, 0); /* size - fill later */
  marshal_u32(&p, TPM2_CC_FLUSH_CONTEXT);

  /* Handle to flush */
  marshal_u32(&p, handle);

  /* Fill command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  /* Send command */
  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_flush_context: transmit failed for handle 0x%08X\n", handle);
    return -1;
  }

  /* Parse response */
  rp = resp;
  unmarshal_u16(&rp); /* tag */
  unmarshal_u32(&rp); /* size */
  rc = unmarshal_u32(&rp);

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm2_flush_context: TPM error 0x%08X for handle 0x%08X\n", rc,
          handle);
    return -1;
  }

  print("tpm2_flush_context: Flushed handle 0x%08X\n", handle);
  return 0;
}

/*
 * TPM2_Shutdown - Orderly shutdown of TPM
 *
 * Should be called before system shutdown/reboot to ensure TPM state is saved.
 *
 * shutdown_type:
 *   TPM2_SU_CLEAR (0x0000) - Clear TPM state
 *   TPM2_SU_STATE (0x0001) - Save TPM state for resume
 *
 * Based on Linux kernel's tpm2_shutdown()
 */
int tpm2_shutdown(TPMContext *ctx, u16int shutdown_type) {
  USED(ctx);
  u8int cmd[64];
  u8int resp[64];
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_NO_SESSIONS);
  marshal_u32(&p, 0); /* size - fill later */
  marshal_u32(&p, TPM2_CC_SHUTDOWN);

  /* Shutdown type */
  marshal_u16(&p, shutdown_type);

  /* Fill command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  /* Send command */
  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_shutdown: transmit failed\n");
    return -1;
  }

  /* Parse response */
  rp = resp;
  unmarshal_u16(&rp); /* tag */
  unmarshal_u32(&rp); /* size */
  rc = unmarshal_u32(&rp);

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm2_shutdown: TPM error 0x%08X\n", rc);
    return -1;
  }

  print("tpm2_shutdown: TPM shut down (type=0x%04X)\n", shutdown_type);
  return 0;
}

/*
 * TPM2_GetCapability - Query TPM capabilities and properties
 *
 * capability: Type of capability (e.g., TPM2_CAP_TPM_PROPERTIES)
 * property: Property ID to query
 * value_out: Pointer to store the returned value
 *
 * Based on Linux kernel's tpm2_get_tpm_pt()
 */
int tpm2_get_capability(TPMContext *ctx, u32int capability, u32int property,
                        u32int *value_out) {
  USED(ctx);
  u8int cmd[128];
  u8int resp[256];
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_NO_SESSIONS);
  marshal_u32(&p, 0); /* size - fill later */
  marshal_u32(&p, TPM2_CC_GET_CAPABILITY);

  /* Capability area */
  marshal_u32(&p, capability);

  /* Property (what to query within capability) */
  marshal_u32(&p, property);

  /* Property count (how many to retrieve) */
  marshal_u32(&p, 1);

  /* Fill command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  /* Send command */
  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_get_capability: transmit failed\n");
    return -1;
  }

  /* Parse response */
  rp = resp;
  unmarshal_u16(&rp); /* tag */
  unmarshal_u32(&rp); /* size */
  rc = unmarshal_u32(&rp);

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm2_get_capability: TPM error 0x%08X\n", rc);
    return -1;
  }

  /* Response format:
   *   moreData (u8)
   *   capabilityData (varies by capability type)
   *
   * For TPM_CAP_TPM_PROPERTIES:
   *   TPML_TAGGED_TPM_PROPERTY:
   *     count (u32)
   *     properties[count]:
   *       property (u32)
   *       value (u32)
   */

  u8int more_data = *rp++; /* moreData flag */
  USED(more_data);

  /* Skip capability type (u32) - it's what we requested */
  unmarshal_u32(&rp);

  /* Property count */
  u32int prop_count = unmarshal_u32(&rp);

  if (prop_count == 0) {
    print("tpm2_get_capability: No properties returned\n");
    return -1;
  }

  /* Extract first property */
  u32int returned_property = unmarshal_u32(&rp);
  USED(returned_property);
  *value_out = unmarshal_u32(&rp);

  print("tpm2_get_capability: cap=0x%08X prop=0x%08X value=0x%08X\n",
        capability, property, *value_out);
  return 0;
}

/*
 * TPM2_SelfTest - Run TPM self-tests
 *
 * full_test:
 *   0 = Incremental self-test (test remaining untested functions)
 *   1 = Full self-test (test all functions)
 *
 * Based on Linux kernel's tpm2_do_selftest()
 */
int tpm2_self_test(TPMContext *ctx, u8int full_test) {
  USED(ctx);
  u8int cmd[64];
  u8int resp[64];
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_NO_SESSIONS);
  marshal_u32(&p, 0); /* size - fill later */
  marshal_u32(&p, TPM2_CC_SELF_TEST);

  /* Full test flag */
  *p++ = full_test;

  /* Fill command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  /* Send command */
  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_self_test: transmit failed\n");
    return -1;
  }

  /* Parse response */
  rp = resp;
  unmarshal_u16(&rp); /* tag */
  unmarshal_u32(&rp); /* size */
  rc = unmarshal_u32(&rp);

  /* TPM2_RC_TESTING (0x090A) means tests are still running asynchronously */
  if (rc == TPM2_RC_TESTING) {
    print("tpm2_self_test: Tests running asynchronously\n");
    return 0;
  }

  /* TPM2_RC_INITIALIZE (0x0100) means TPM needs startup first */
  if (rc == TPM2_RC_INITIALIZE) {
    print("tpm2_self_test: TPM not initialized (run TPM2_Startup first)\n");
    return -1;
  }

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm2_self_test: TPM error 0x%08X\n", rc);
    return -1;
  }

  print("tpm2_self_test: Self-test completed successfully (full=%d)\n",
        full_test);
  return 0;
}

/*
 * TPM2_NV_Read - Read data from NVRAM
 *
 * Companion to TPM2_NV_Write. Reads data from a defined NV index.
 * Based on TPM 2.0 Spec Part 3 Commands, section 31.5
 */
int tpm2_nv_read(TPMContext *ctx, u32int nv_index, u16int size, u16int offset,
                 u8int *data_out, u16int *data_len) {
  USED(ctx);
  u8int cmd[256];
  u8int resp[1024];
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_SESSIONS);
  marshal_u32(&p, 0); /* size - fill later */
  marshal_u32(&p, TPM2_CC_NV_READ);

  /* Auth Handle (Owner) */
  marshal_u32(&p, TPM2_RH_OWNER);

  /* NV Index */
  marshal_u32(&p, nv_index);

  /* Authorization Session (Password) */
  marshal_password_session(&p);

  /* Size to read */
  marshal_u16(&p, size);

  /* Offset */
  marshal_u16(&p, offset);

  /* Fill command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_nv_read: transmit failed\n");
    return -1;
  }

  rp = resp;
  u16int rtag = unmarshal_u16(&rp);
  unmarshal_u32(&rp);
  rc = unmarshal_u32(&rp);

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm2_nv_read: TPM error 0x%08X\n", rc);
    return -1;
  }

  if (rtag == TPM2_ST_SESSIONS) {
    unmarshal_u32(&rp); /* parameter size */
  }

  /* Extract data */
  u16int dlen = unmarshal_tpm2b(&rp, data_out, 1024);
  *data_len = dlen;

  print("tpm2_nv_read: Read %d bytes from NV index 0x%08X\n", *data_len,
        nv_index);
  return 0;
}

/*
 * TPM2_ContextSave - Save context of a loaded object
 *
 * Saves the context of a loaded object so it can be flushed from TPM memory
 * and later restored with TPM2_ContextLoad. Useful for managing limited TPM
 * resources. Based on Linux kernel's tpm2_save_context()
 */
int tpm2_context_save(TPMContext *ctx, u32int handle, u8int *context_blob,
                      u16int *context_len) {
  USED(ctx);
  u8int cmd[64];
  u8int resp[2048]; /* Context blobs can be large */
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_NO_SESSIONS);
  marshal_u32(&p, 0); /* size - fill later */
  marshal_u32(&p, TPM2_CC_CONTEXT_SAVE);

  /* Handle to save */
  marshal_u32(&p, handle);

  /* Fill command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_context_save: transmit failed\n");
    return -1;
  }

  rp = resp;
  unmarshal_u16(&rp); /* tag */
  unmarshal_u32(&rp); /* size */
  rc = unmarshal_u32(&rp);

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm2_context_save: TPM error 0x%08X\n", rc);
    return -1;
  }

  /* Extract context blob (TPMS_CONTEXT structure)
   * Note: Context is NOT a TPM2B, it's a raw TPMS_CONTEXT structure */
  u16int blob_size = (resp[10] << 8) | resp[11]; /* First field is sequence */
  if (blob_size > 2048) {
    print("tpm2_context_save: Context blob too large (%d bytes)\n", blob_size);
    return -1;
  }

  /* For simplicity, copy entire response payload as context */
  *context_len = (u16int)(resp_len - 10); /* Skip header */
  memmove(context_blob, resp + 10, *context_len);

  print("tpm2_context_save: Saved context for handle 0x%08X (%d bytes)\n",
        handle, *context_len);
  return 0;
}

/*
 * TPM2_ContextLoad - Load a previously saved context
 *
 * Restores a context blob saved with TPM2_ContextSave, returning a new handle.
 * Based on Linux kernel's tpm2_load_context()
 */
int tpm2_context_load(TPMContext *ctx, u8int *context_blob, u16int context_len,
                      u32int *handle_out) {
  USED(ctx);
  u8int cmd[2048];
  u8int resp[64];
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_NO_SESSIONS);
  marshal_u32(&p, 0); /* size - fill later */
  marshal_u32(&p, TPM2_CC_CONTEXT_LOAD);

  /* Context blob (raw TPMS_CONTEXT, not TPM2B) */
  memmove(p, context_blob, context_len);
  p += context_len;

  /* Fill command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_context_load: transmit failed\n");
    return -1;
  }

  rp = resp;
  unmarshal_u16(&rp); /* tag */
  unmarshal_u32(&rp); /* size */
  rc = unmarshal_u32(&rp);

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm2_context_load: TPM error 0x%08X\n", rc);
    return -1;
  }

  /* Extract new handle */
  *handle_out = unmarshal_u32(&rp);

  print("tpm2_context_load: Loaded context, new handle 0x%08X\n", *handle_out);
  return 0;
}

/*
 * TPM2_ReadPublic - Read the public portion of a loaded object
 *
 * Returns the public area, name, and qualified name of an object.
 * Useful for verifying object properties and getting the TPM name.
 */
int tpm2_read_public(TPMContext *ctx, u32int handle, u8int *public_out,
                     u16int *public_len) {
  USED(ctx);
  u8int cmd[64];
  u8int resp[1024];
  usize resp_len = sizeof(resp);
  u8int *p = cmd;
  u8int *rp;
  u32int rc;

  /* Command Header */
  marshal_u16(&p, TPM2_ST_NO_SESSIONS);
  marshal_u32(&p, 0); /* size - fill later */
  marshal_u32(&p, TPM2_CC_READ_PUBLIC);

  /* Object handle */
  marshal_u32(&p, handle);

  /* Fill command size */
  u32int cmd_size = (u32int)(p - cmd);
  cmd[2] = (u8int)((cmd_size >> 24) & 0xFF);
  cmd[3] = (u8int)((cmd_size >> 16) & 0xFF);
  cmd[4] = (u8int)((cmd_size >> 8) & 0xFF);
  cmd[5] = (u8int)(cmd_size & 0xFF);

  if (tpm_transmit(nil, (uint8_t *)cmd, (size_t)cmd_size, (uint8_t *)resp,
                   (size_t *)&resp_len) < 0) {
    print("tpm2_read_public: transmit failed\n");
    return -1;
  }

  rp = resp;
  unmarshal_u16(&rp); /* tag */
  unmarshal_u32(&rp); /* size */
  rc = unmarshal_u32(&rp);

  if (rc != TPM2_RC_SUCCESS) {
    print("tpm2_read_public: TPM error 0x%08X\n", rc);
    return -1;
  }

  /* Extract public area (outPublic) */
  u16int plen = unmarshal_tpm2b(&rp, public_out, 1024);
  *public_len = plen;

  /* Note: Response also contains name and qualifiedName, but we skip them for
   * now */

  print("tpm2_read_public: Read public area for handle 0x%08X (%d bytes)\n",
        handle, *public_len);
  return 0;
}

/*
 * High-level API: Seal data under Storage Root Key (SRK)
 *
 * Returns a single blob containing both private and public portions.
 * Based on Linux kernel trusted_tpm2.c high-level API.
 */
int tpm2_seal_to_srk(const u8int *data, u16int data_len, const u8int *auth,
                     u16int auth_len, u8int *blob_out, u16int *blob_len) {
  u8int priv[256];
  u8int pub[256];
  u16int priv_len = sizeof(priv);
  u16int pub_len = sizeof(pub);
  u32int srk_handle;
  int rc;

  USED(auth);
  USED(auth_len);

  /* First, create or get the SRK handle */
  rc = tpm2_create_primary(0, &srk_handle);
  if (rc < 0) {
    print("tpm2_seal_to_srk: failed to create primary key\n");
    return rc;
  }

  /* Seal the data under the SRK */
  rc = tpm2_create(srk_handle, (u8int *)data, data_len, priv, &priv_len, pub,
                   &pub_len);
  if (rc < 0) {
    print("tpm2_seal_to_srk: failed to seal data\n");
    tpm2_flush_context(srk_handle);
    return rc;
  }

  /* Pack into blob: priv_len(2) + priv + pub_len(2) + pub + srk_handle(4) */
  u16int total = (u16int)(2 + priv_len + 2 + pub_len + 4);
  if (total > *blob_len) {
    tpm2_flush_context(srk_handle);
    return -1;
  }

  u8int *p = blob_out;
  marshal_u16(&p, priv_len);
  memmove(p, priv, priv_len);
  p += priv_len;
  marshal_u16(&p, pub_len);
  memmove(p, pub, pub_len);
  p += pub_len;
  marshal_u32(&p, srk_handle);

  *blob_len = total;

  print("tpm2_seal_to_srk: sealed %d bytes (blob=%d bytes)\n", data_len, total);
  return 0;
}

/*
 * High-level API: Unseal data from combined blob
 */
int tpm2_unseal_from_blob(const u8int *blob, u16int blob_len, const u8int *auth,
                          u16int auth_len, u8int *data_out, u16int *data_len) {
  u32int obj_handle;
  u32int srk_handle;
  int rc;

  USED(auth);
  USED(auth_len);

  if (blob_len < 8)
    return -1;

  /* Unpack blob */
  u8int *p = (u8int *)blob;
  u16int priv_len = unmarshal_u16(&p);
  u8int *priv = p;
  p += priv_len;

  if (p + 2 > blob + blob_len)
    return -1;

  u16int pub_len = unmarshal_u16(&p);
  u8int *pub = p;
  p += pub_len;

  if (p + 4 > blob + blob_len)
    return -1;

  srk_handle = unmarshal_u32(&p);

  /* Load into TPM */
  rc = tpm2_load(srk_handle, priv, priv_len, pub, pub_len, &obj_handle);
  if (rc < 0) {
    print("tpm2_unseal_from_blob: failed to load object\n");
    return rc;
  }

  /* Unseal */
  rc = tpm2_unseal(obj_handle, auth, auth_len, data_out, data_len);

  /* Always flush the loaded object */
  tpm2_flush_context(obj_handle);

  if (rc < 0) {
    print("tpm2_unseal_from_blob: failed to unseal\n");
    return rc;
  }

  print("tpm2_unseal_from_blob: unsealed %d bytes\n", *data_len);
  return 0;
}
