/* This program is released under the Common Public License V1.0
 *
 * You should have received a copy of Common Public License V1.0 along with
 * with this program.
 */

/**
 * Authors: Joerg Schmidbauer <jschmidb@de.ibm.com>
 *
 * Copyright IBM Corp. 2017
 */

#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/ecdsa.h>
#include <openssl/opensslconf.h>
#ifdef OPENSSL_FIPS
#include <openssl/fips.h>
#endif /* OPENSSL_FIPS */

#include "fips.h"
#include "s390_ecc.h"
#include "s390_crypto.h"
#include "rng.h"
#include "init.h"
#include "icastats.h"
#include "s390_sha.h"

#define CPRBXSIZE (sizeof(struct CPRBX))
#define PARMBSIZE (2048)

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define ECDSA_SIG_get0(sig,pr,ps) \
do { \
*(pr)=(sig)->r; \
*(ps)=(sig)->s; \
} while (0)
#define ECDSA_SIG_set0(sig,pr,ps) \
do { \
(sig)->r=pr; \
(sig)->s=ps; \
} while (0)
#endif

static int eckeygen_cpacf(ICA_EC_KEY *key);
static int ecdsa_sign_cpacf(const ICA_EC_KEY *priv, const unsigned char *hash,
			    size_t hashlen, unsigned char *sig,
			    void (*rng_cb)(unsigned char *, size_t));
static int ecdsa_verify_cpacf(const ICA_EC_KEY *pub, const unsigned char *hash,
			      size_t hashlen, const unsigned char *sig);
static int scalar_mul_cpacf(unsigned char *res_x, unsigned char *res_y,
			    const unsigned char *scalar,
			    const unsigned char *x,
			    const unsigned char *y, int curve_nid);
int scalar_mulx_cpacf(unsigned char *res_u,
		      const unsigned char *scalar,
		      const unsigned char *u,
		      int curve_nid);

/**
 * Check if openssl does support this ec curve
 */
static int is_supported_openssl_curve(int nid)
{
	EC_GROUP *ptr = EC_GROUP_new_by_curve_name(nid);
	if (ptr)
		EC_GROUP_free(ptr);
	return ptr ? 1 : 0;
}

/**
 * makes a private EC_KEY.
 */
static EC_KEY *make_eckey(int nid, const unsigned char *p, size_t plen)
{
	int ok = 0;
	EC_KEY *k = NULL;
	BIGNUM *priv = NULL;
	EC_POINT *pub = NULL;
	const EC_GROUP *grp;

	if ((k = EC_KEY_new_by_curve_name(nid)) == NULL) {
		goto err;
	}

	if ((priv = BN_bin2bn(p, plen, NULL)) == NULL) {
		goto err;
	}

	if (!EC_KEY_set_private_key(k, priv)) {
		goto err;
	}

	grp = EC_KEY_get0_group(k);
	if ((pub = EC_POINT_new(grp)) == NULL) {
		goto err;
	}

	if (!EC_POINT_mul(grp, pub, priv, NULL, NULL, NULL)) {
		goto err;
	}

	if (!EC_KEY_set_public_key(k, pub)) {
		goto err;
	}
	ok = 1;

err:
	if (priv)
		BN_clear_free(priv);
	if (pub)
		EC_POINT_free(pub);

	if (ok)
		return k;
	else if (k)
		EC_KEY_free(k);

	return NULL;
}

/**
 * makes a public EC_KEY.
 */
static EC_KEY *make_public_eckey(int nid, BIGNUM *x, BIGNUM *y, size_t plen)
{
	int ok = 0;
	EC_KEY *k = NULL;
	EC_POINT *pub = NULL;
	const EC_GROUP *grp;

	(void)plen;	/* suppress unused param warning. XXX remove plen? */

	k = EC_KEY_new_by_curve_name(nid);
	if (!k)
		goto err;

	grp = EC_KEY_get0_group(k);
	pub = EC_POINT_new(grp);
	if (!pub)
		goto err;

	if (x && y) {
		BN_CTX* ctx = BN_CTX_new();
		if (ctx == NULL)
			goto err;

		EC_POINT_set_affine_coordinates_GFp(grp, pub, x, y, ctx);

		BN_CTX_free(ctx);
	}

	if (!EC_KEY_set_public_key(k, pub))
		goto err;
	ok = 1;

err:
	if (pub)
		EC_POINT_free(pub);

	if (ok)
		return k;
	else if (k)
		EC_KEY_free(k);

	return NULL;
}

/**
 * makes a keyblock length field at given struct and returns its length.
 */
static unsigned int make_keyblock_length(ECC_KEYBLOCK_LENGTH *kb, unsigned int len)
{
	kb->keyblock_len = len;

	return sizeof(ECC_KEYBLOCK_LENGTH);
}

/**
 * makes a nullkey token at given struct and returns its length.
 */
static unsigned int make_nullkey(ECDH_NULLKEY* nkey)
{
	nkey->nullkey_len[0] = 0x00;
	nkey->nullkey_len[1] = 0x44;

	return sizeof(ECDH_NULLKEY);
}

/**
 * makes an ecc null token at given struct.
 */
static unsigned int make_ecc_null_token(ECC_NULL_TOKEN *kb)
{
	kb->len = 0x0005;
	kb->flags = 0x0010;
	kb->nulltoken = 0x00;

	return sizeof(ECC_NULL_TOKEN);
}

/**
 * determines and returns the default domain. With older zcrypt drivers
 * it's not possible to specify 0xffff to indicate 'any domain' in a
 * request CPRB.
 *
 * @return domain number (0 ... n, machine dependent) if success
 *         -1 if error or driver not loaded
 */
static short get_default_domain(void)
{
	const char *domainfile = "/sys/bus/ap/ap_domain";
	static short domain = -1;
	int temp;
	FILE *f = NULL;

	if (domain >= 0)
		return domain;

	f = fopen(domainfile, "r");
	if (!f)
		return domain;

	if (fscanf(f, "%d", &temp) != 1)
		return domain;

	domain = (short)temp;

	if (f)
		fclose(f);

	return domain;
}

/**
 * makes a T2 CPRBX at given struct and returns its length.
 */
static unsigned int make_cprbx(struct CPRBX* cprbx, unsigned int parmlen,
		struct CPRBX *preqcblk, struct CPRBX *prepcblk)
{
	cprbx->cprb_len = CPRBXSIZE;
	cprbx->cprb_ver_id = 0x02;
	memcpy(&(cprbx->func_id), "T2", 2);
	cprbx->req_parml = parmlen;
	cprbx->domain = get_default_domain();

	cprbx->rpl_msgbl = CPRBXSIZE + PARMBSIZE;
	cprbx->req_parmb = ((uint8_t *) preqcblk) + CPRBXSIZE;
	cprbx->rpl_parmb = ((uint8_t *) prepcblk) + CPRBXSIZE;

	return CPRBXSIZE;
}

/**
 * makes an ECDH parmblock at given struct and returns its length.
 */
static unsigned int make_ecdh_parmblock(ECDH_PARMBLOCK *pb)
{
	typedef struct {
		uint16_t vud_len;
		uint8_t vud1[4];
		uint8_t vud2[6];
		uint8_t vud3[4];
		uint8_t vud4[4];
	} vud_data;

	vud_data static_vud = {
		0x0014,
		{0x00,0x04,0x00,0x91},
		{0x00,0x06,0x00,0x93,0x00,0x00},
		{0x00,0x04,0x00,0x90},
		{0x00,0x04,0x00,0x92}
	};

	pb->subfunc_code = 0x4448; /* 'DH' in ASCII */
	pb->rule_array.rule_array_len = 0x000A;
	memcpy(&(pb->rule_array.rule_array_cmd), "PASSTHRU", 8);
	memcpy(&(pb->vud_data), (char*)&static_vud, sizeof(vud_data));

	return sizeof(ECDH_PARMBLOCK);
}

/**
 * makes an ECDH key structure at given struct and returns its length.
 */
static unsigned int make_ecdh_key_token(unsigned char *kb, unsigned int keyblock_length,
		const ICA_EC_KEY  *privkey_A, const ICA_EC_KEY *pubkey_B,
		uint8_t curve_type)
{
	ECC_PRIVATE_KEY_TOKEN* kp1;
	ECC_PUBLIC_KEY_TOKEN* kp2;
	unsigned int privlen = privlen_from_nid(privkey_A->nid);

	unsigned int this_length = sizeof(ECC_PRIVATE_KEY_TOKEN) + privlen
			+ sizeof(ECC_PUBLIC_KEY_TOKEN) + 2*privlen;

	unsigned int ecdhkey_length = 2 + 2 + sizeof(CCA_TOKEN_HDR)
			+ sizeof(ECC_PRIVATE_KEY_SECTION)
			+ sizeof(ECC_ASSOCIATED_DATA) + privlen
			+ sizeof(ECC_PUBLIC_KEY_TOKEN) + 2*privlen;

	unsigned int priv_bitlen = privlen*8;

	(void)keyblock_length;	/* suppress unused param warning. XXX remove param? */

	if (privkey_A->nid == NID_secp521r1) {
		priv_bitlen = 521;
	}

	kp1 = (ECC_PRIVATE_KEY_TOKEN*)kb;
	kp2 = (ECC_PUBLIC_KEY_TOKEN*)(kb + sizeof(ECC_PRIVATE_KEY_TOKEN) + privlen);

	kp1->key_len = ecdhkey_length;
	kp1->tknhdr.tkn_hdr_id = 0x1E;
	kp1->tknhdr.tkn_length = ecdhkey_length - 2 - 2; /* 2x len field */

	kp1->privsec.section_id = 0x20;
	kp1->privsec.version = 0x00;
	kp1->privsec.section_len =  sizeof(ECC_PRIVATE_KEY_SECTION) + sizeof(ECC_ASSOCIATED_DATA) + privlen;
	kp1->privsec.key_usage = 0xC0;
	kp1->privsec.curve_type = curve_type;
	kp1->privsec.key_format = 0x40; /* unencrypted key */
	kp1->privsec.priv_p_bitlen = priv_bitlen;
	kp1->privsec.associated_data_len = sizeof(ECC_ASSOCIATED_DATA);
	kp1->privsec.ibm_associated_data_len = sizeof(ECC_ASSOCIATED_DATA);
	kp1->privsec.formatted_data_len = privlen;

	kp1->adata.ibm_data_len = sizeof(ECC_ASSOCIATED_DATA);
	kp1->adata.curve_type = curve_type;
	kp1->adata.p_bitlen = priv_bitlen;
	kp1->adata.usage_flag = 0xC0;
	kp1->adata.format_and_sec_flag = 0x40;

	memcpy(&kp1->privkey[0], privkey_A->D, privlen);

	kp2->pubsec.section_id = 0x21;
	kp2->pubsec.section_len = sizeof(ECC_PUBLIC_KEY_TOKEN) + 2*privlen;
	kp2->pubsec.curve_type = curve_type;
	kp2->pubsec.pub_p_bitlen = priv_bitlen;
	kp2->pubsec.pub_q_bytelen = 2*privlen + 1; /* pub bytelen + compress flag */

	kp2->compress_flag = 0x04; /* uncompressed key */
	memcpy(&kp2->pubkey[0], pubkey_B->X, privlen);
	memcpy(&kp2->pubkey[privlen+0], pubkey_B->Y, privlen);

	return this_length;
}

/**
 * finalizes an ica_xcRB struct that is sent to the card.
 */
static void finalize_xcrb(struct ica_xcRB* xcrb, struct CPRBX *preqcblk, struct CPRBX *prepcblk)
{
	memset(xcrb, 0, sizeof(struct ica_xcRB));
	xcrb->agent_ID = 0x4341;
	xcrb->user_defined = AUTOSELECT; /* use any card number */
	xcrb->request_control_blk_length = preqcblk->cprb_len + preqcblk->req_parml;
	xcrb->request_control_blk_addr = (void *) preqcblk;
	xcrb->reply_control_blk_length = preqcblk->rpl_msgbl;
	xcrb->reply_control_blk_addr = (void *) prepcblk;
}

/**
 * creates an ECDH xcrb request message for zcrypt.
 *
 * returns a pointer to the control block where the card
 * provides its reply.
 *
 * The function allocates len bytes at cbcbmem. The caller
 * is responsible to erase sensible data and free the
 * memory.
 */
static ECDH_REPLY* make_ecdh_request(const ICA_EC_KEY *privkey_A, const ICA_EC_KEY *pubkey_B,
		struct ica_xcRB* xcrb, uint8_t **cbrbmem, size_t *len)
{
	struct CPRBX *preqcblk, *prepcblk;
	unsigned int privlen = privlen_from_nid(privkey_A->nid);

	unsigned int ecdh_key_token_len = 2 + 2 + sizeof(CCA_TOKEN_HDR)
		+ sizeof(ECC_PRIVATE_KEY_SECTION)
		+ sizeof(ECC_ASSOCIATED_DATA) + privlen
		+ sizeof(ECC_PUBLIC_KEY_TOKEN) + 2*privlen;

	unsigned int keyblock_len = 2 + 2*ecdh_key_token_len + 4*sizeof(ECDH_NULLKEY);
	unsigned int parmblock_len = sizeof(ECDH_PARMBLOCK) + keyblock_len;

	int curve_type = curve_type_from_nid(privkey_A->nid);
	if (curve_type < 0)
		return NULL;

	/* allocate buffer space for req cprb, req parm, rep cprb, rep parm */
	*len = 2 * (CPRBXSIZE + PARMBSIZE);
	*cbrbmem = malloc(*len);
	if (!*cbrbmem)
		return NULL;

	memset(*cbrbmem, 0, *len);
	preqcblk = (struct CPRBX *) *cbrbmem;
	prepcblk = (struct CPRBX *) (*cbrbmem + CPRBXSIZE + PARMBSIZE);

	/* make ECDH request */
	unsigned int offset = 0;
	offset = make_cprbx((struct CPRBX *)*cbrbmem, parmblock_len, preqcblk, prepcblk);
	offset += make_ecdh_parmblock((ECDH_PARMBLOCK*)(*cbrbmem+offset));
	offset += make_keyblock_length((ECC_KEYBLOCK_LENGTH*)(*cbrbmem+offset), keyblock_len);
	offset += make_ecdh_key_token(*cbrbmem+offset, ecdh_key_token_len, privkey_A, pubkey_B, curve_type);
	offset += make_nullkey((ECDH_NULLKEY*)(*cbrbmem+offset));
	offset += make_ecdh_key_token(*cbrbmem+offset, ecdh_key_token_len, privkey_A, pubkey_B, curve_type);
	offset += make_nullkey((ECDH_NULLKEY*)(*cbrbmem+offset));
	offset += make_nullkey((ECDH_NULLKEY*)(*cbrbmem+offset));
	offset += make_nullkey((ECDH_NULLKEY*)(*cbrbmem+offset));
	finalize_xcrb(xcrb, preqcblk, prepcblk);

	return (ECDH_REPLY*)prepcblk;
}

static int scalar_mul_cpacf(unsigned char *res_x, unsigned char *res_y,
			    const unsigned char *scalar,
			    const unsigned char *x,
			    const unsigned char *y, int curve_nid)
{
#define DEF_PARAM(curve, size)		\
struct {				\
	unsigned char res_x[size];	\
	unsigned char res_y[size];	\
	unsigned char x[size];		\
	unsigned char y[size];		\
	unsigned char scalar[size];	\
} curve

	union {
		long long buff[512];	/* 4k buffer: params + reserved area */
		DEF_PARAM(P256, 32);
		DEF_PARAM(P384, 48);
		DEF_PARAM(P521, 80);
		DEF_PARAM(ED25519, 32);
		DEF_PARAM(ED448, 64);
	} param;

#undef DEF_PARAM

	unsigned long fc;
	size_t off;
	int rc;

	const size_t len = privlen_from_nid(curve_nid);

	memset(&param, 0, sizeof(param));

	switch (curve_nid) {
	case NID_X9_62_prime256v1:
		off = sizeof(param.P256.scalar) - len;

		memcpy(param.P256.x + off, x,
		       sizeof(param.P256.x) - off);
		memcpy(param.P256.y + off, y,
		       sizeof(param.P256.y) - off);
		memcpy(param.P256.scalar + off, scalar,
		       sizeof(param.P256.scalar) - off);

		fc = s390_pcc_functions[SCALAR_MULTIPLY_P256].hw_fc;
		rc = s390_pcc(fc, &param) ? EIO : 0;

		if (res_x != NULL)
			memcpy(res_x, param.P256.res_x + off, len);
		if (res_y != NULL)
			memcpy(res_y, param.P256.res_y + off, len);
		break;

	case NID_secp384r1:
		off = sizeof(param.P384.scalar) - len;

		memcpy(param.P384.x + off, x,
		       sizeof(param.P384.x) - off);
		memcpy(param.P384.y + off, y,
		       sizeof(param.P384.y) - off);
		memcpy(param.P384.scalar + off, scalar,
		       sizeof(param.P384.scalar) - off);

		fc = s390_pcc_functions[SCALAR_MULTIPLY_P384].hw_fc;
		rc = s390_pcc(fc, &param) ? EIO : 0;

		if (res_x != NULL)
			memcpy(res_x, param.P384.res_x + off, len);
		if (res_y != NULL)
			memcpy(res_y, param.P384.res_y + off, len);
		break;

	case NID_secp521r1:
		off = sizeof(param.P521.scalar) - len;

		memcpy(param.P521.x + off, x,
		       sizeof(param.P521.x) - off);
		memcpy(param.P521.y + off, y,
		       sizeof(param.P521.y) - off);
		memcpy(param.P521.scalar + off, scalar,
		       sizeof(param.P521.scalar) - off);

		fc = s390_pcc_functions[SCALAR_MULTIPLY_P521].hw_fc;
		rc = s390_pcc(fc, &param) ? EIO : 0;

		if (res_x != NULL)
			memcpy(res_x, param.P521.res_x + off, len);
		if (res_y != NULL)
			memcpy(res_y, param.P521.res_y + off, len);
		break;

	case NID_ED25519:
		off = sizeof(param.ED25519.scalar) - len;

		memcpy(param.ED25519.x + off, x,
		       sizeof(param.ED25519.x) - off);
		memcpy(param.ED25519.y + off, y,
		       sizeof(param.ED25519.y) - off);
		memcpy(param.ED25519.scalar + off, scalar,
		       sizeof(param.ED25519.scalar) - off);

		fc = s390_pcc_functions[SCALAR_MULTIPLY_ED25519].hw_fc;
		rc = s390_pcc(fc, &param) ? EIO : 0;

		if (res_x != NULL)
			memcpy(res_x, param.ED25519.res_x + off, len);
		if (res_y != NULL)
			memcpy(res_y, param.ED25519.res_y + off, len);
		break;

	case NID_ED448:
		off = sizeof(param.ED448.scalar) - len;

		memcpy(param.ED448.x + off, x,
		       sizeof(param.ED448.x) - off);
		memcpy(param.ED448.y + off, y,
		       sizeof(param.ED448.y) - off);
		memcpy(param.ED448.scalar + off, scalar,
		       sizeof(param.ED448.scalar) - off);

		fc = s390_pcc_functions[SCALAR_MULTIPLY_ED448].hw_fc;
		rc = s390_pcc(fc, &param) ? EIO : 0;

		if (res_x != NULL)
			memcpy(res_x, param.ED448.res_x + off, len);
		if (res_y != NULL)
			memcpy(res_y, param.ED448.res_y + off, len);
		break;

	default:
		rc = EINVAL;
	}

	OPENSSL_cleanse(param.buff, sizeof(param.buff));
	return rc;
}

int scalar_mulx_cpacf(unsigned char *res_u,
		      const unsigned char *scalar,
		      const unsigned char *u,
		      int curve_nid)
{
#define DEF_PARAM(curve, size)		\
struct {				\
	unsigned char res_u[size];	\
	unsigned char u[size];		\
	unsigned char scalar[size];	\
} curve

	union {
		long long buff[512];	/* 4k buffer: params + reserved area */
		DEF_PARAM(X25519, 32);
		DEF_PARAM(X448, 64);
	} param;

#undef DEF_PARAM

	unsigned long fc;
	int rc;

	const size_t len = privlen_from_nid(curve_nid);

	memset(&param, 0, sizeof(param));

	switch (curve_nid) {
	case NID_X25519:
		memcpy(param.X25519.u, u, len);
		memcpy(param.X25519.scalar, scalar, len);

		param.X25519.u[31] &= 0x7f;
		param.X25519.scalar[0] &= 248;
	        param.X25519.scalar[31] &= 127;
		param.X25519.scalar[31] |= 64;

		/* to big-endian */
		s390_flip_endian_32(param.X25519.u, param.X25519.u);
		s390_flip_endian_32(param.X25519.scalar, param.X25519.scalar);

		fc = s390_pcc_functions[SCALAR_MULTIPLY_X25519].hw_fc;
		rc = s390_pcc(fc, &param) ? EIO : 0;

		/* to little-endian */
		s390_flip_endian_32(param.X25519.res_u, param.X25519.res_u);

		if (res_u != NULL)
			memcpy(res_u, param.X25519.res_u, len);
		break;

	case NID_X448:
		memcpy(param.X448.u, u, len);
		memcpy(param.X448.scalar, scalar, len);

		param.X448.scalar[0] &= 252;
		param.X448.scalar[55] |= 128;

		/* to big-endian */
		s390_flip_endian_64(param.X448.u, param.X448.u);
		s390_flip_endian_64(param.X448.scalar, param.X448.scalar);

		fc = s390_pcc_functions[SCALAR_MULTIPLY_X448].hw_fc;
		rc = s390_pcc(fc, &param) ? EIO : 0;

		/* to little-endian */
		s390_flip_endian_64(param.X448.res_u, param.X448.res_u);

		if (res_u != NULL)
			memcpy(res_u, param.X448.res_u, len);
		break;

	default:
		rc = EINVAL;
	}

	OPENSSL_cleanse(param.buff, sizeof(param.buff));
	return rc;
}

/**
 * Perform an ECDH shared secret calculation with given EC private key A (D)
 * and EC public key B (X,Y) via CPACF Crypto Express CCA coprocessor.
 *
 * Returns 0 if successful
 *         EIO if an internal error occurred
 */
unsigned int ecdh_hw(ica_adapter_handle_t adapter_handle,
		const ICA_EC_KEY *privkey_A, const ICA_EC_KEY *pubkey_B,
		unsigned char *z)
{
	uint8_t *buf;
	size_t len;
	int rc;
	struct ica_xcRB xcrb;
	ECDH_REPLY* reply_p;
	int privlen = privlen_from_nid(privkey_A->nid);

	if (msa9_switch && !ica_offload_enabled) {
		rc = scalar_mul_cpacf(z, NULL, privkey_A->D, pubkey_B->X,
				      pubkey_B->Y, privkey_A->nid);
		if (rc != EINVAL) /* EINVAL: curve not supported by cpacf */
			return rc;
	}

	if (!ecc_via_online_card)
		return ENODEV;

	if (adapter_handle == DRIVER_NOT_LOADED)
		return EIO;

	reply_p = make_ecdh_request(privkey_A, pubkey_B, &xcrb, &buf, &len);
	if (!reply_p)
		return EIO;

	rc = ioctl(adapter_handle, ZSECSENDCPRB, xcrb);
	if (rc != 0) {
		rc = EIO;
		goto ret;
	}

	if (reply_p->key_len - 4 != privlen) {
		rc = EIO;
		goto ret;
	}

	memcpy(z, reply_p->raw_z_value, privlen);
	rc = 0;
ret:
	OPENSSL_cleanse(buf, len);
	free(buf);
	return rc;
}

/**
 * Perform an ECDH shared secret calculation with given EC private key A (D)
 * and EC public key B (X,Y) in software.
 *
 * Returns 0 if successful
 *         EIO if an internal error occurred
 */
unsigned int ecdh_sw(const ICA_EC_KEY *privkey_A, const ICA_EC_KEY *pubkey_B,
		unsigned char *z)
{
	int rc, ret = EIO;
	EC_KEY *a = NULL;
	EC_KEY *b = NULL;
	BIGNUM *xb, *yb;
	unsigned int ztmplen;
	unsigned int privlen = privlen_from_nid(privkey_A->nid);

#ifdef ICA_FIPS
	if ((fips & ICA_FIPS_MODE) && (!FIPS_mode()))
		return EACCES;
#endif /* ICA_FIPS */

	if (!is_supported_openssl_curve(privkey_A->nid))
		return EINVAL;

	a = make_eckey(privkey_A->nid, privkey_A->D, privlen);
	xb = BN_bin2bn(pubkey_B->X, privlen, NULL);
	yb = BN_bin2bn(pubkey_B->Y, privlen, NULL);
	b = make_public_eckey(privkey_A->nid, xb, yb, privlen);
	if (!a || !b)
		goto err;

	ztmplen = (EC_GROUP_get_degree(EC_KEY_get0_group(a)) + 7) / 8;
	if (ztmplen != privlen)
		goto err;

	/**
	 * Make sure to use original openssl compute_key method to avoid endless loop
	 * when being called from IBMCA engine in software fallback.
	 */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
	ECDH_set_method(a, ECDH_OpenSSL());
#else
	EC_KEY_set_method(a, EC_KEY_OpenSSL());
#endif
	rc = ECDH_compute_key(z, privlen, EC_KEY_get0_public_key(b), a, NULL);
	if (rc == 0)
		goto err;

	ret = 0;

err:
	BN_clear_free(xb);
	BN_clear_free(yb);
	EC_KEY_free(a);
	EC_KEY_free(b);

	return ret;
}

/**
 * makes an ECDSA sign parmblock at given struct and returns its length.
 */
static unsigned int make_ecdsa_sign_parmblock(ECDSA_PARMBLOCK_PART1 *pb,
		const unsigned char *hash, unsigned int hash_length)
{
	pb->subfunc_code = 0x5347; /* 'SG' */
	pb->rule_array.rule_array_len = 0x000A;
	memcpy(&(pb->rule_array.rule_array_cmd), "ECDSA   ", 8);
	pb->vud_data.vud_len = hash_length+4;
	pb->vud_data.vud1_len = hash_length+2;
	memcpy(&(pb->vud_data.vud1), hash, hash_length);

	return sizeof(ECDSA_PARMBLOCK_PART1) + hash_length;
}

/**
 * makes an ECDSA verify parmblock at given struct and returns its length.
 */
static unsigned int make_ecdsa_verify_parmblock(char *pb,
		const unsigned char *hash, unsigned int hash_length,
		const unsigned char *signature, unsigned int signature_len)
{
	ECDSA_PARMBLOCK_PART1* pb1;
	ECDSA_PARMBLOCK_PART2* pb2;

	pb1 = (ECDSA_PARMBLOCK_PART1*)pb;
	pb2 = (ECDSA_PARMBLOCK_PART2*)(pb + sizeof(ECDSA_PARMBLOCK_PART1) + hash_length);

	pb1->subfunc_code = 0x5356; /* 'SV' */
	pb1->rule_array.rule_array_len = 0x000A;
	memcpy(&(pb1->rule_array.rule_array_cmd), "ECDSA   ", 8);
	pb1->vud_data.vud_len = 2 + (2+hash_length) + (2+signature_len);
	pb1->vud_data.vud1_len = 2+hash_length;
	memcpy(&(pb1->vud_data.vud1), hash, hash_length);

	pb2->vud_data.vud2_len = 2+signature_len;
	memcpy(&(pb2->vud_data.vud2_data), signature, signature_len);

	return sizeof(ECDSA_PARMBLOCK_PART1)
			+ hash_length
			+ sizeof(ECDSA_PARMBLOCK_PART2)
			+ signature_len;
}

/**
 * makes an ECDSA key structure at given struct and returns its length.
 */
static unsigned int make_ecdsa_private_key_token(unsigned char *kb,
		const ICA_EC_KEY *privkey, const unsigned char *X, const unsigned char *Y,
		uint8_t curve_type)
{
	ECC_PRIVATE_KEY_TOKEN* kp1;
	ECC_PUBLIC_KEY_TOKEN* kp2;
	int privlen = privlen_from_nid(privkey->nid);

	unsigned int ecdsakey_length = 2 + 2 + sizeof(CCA_TOKEN_HDR)
			+ sizeof(ECC_PRIVATE_KEY_SECTION)
			+ sizeof(ECC_ASSOCIATED_DATA) + privlen
			+ sizeof(ECC_PUBLIC_KEY_TOKEN) + 2*privlen;

	unsigned int priv_bitlen = privlen*8;
	if (privkey->nid == NID_secp521r1) {
		priv_bitlen = 521;
	}

	kp1 = (ECC_PRIVATE_KEY_TOKEN*)kb;
	kp2 = (ECC_PUBLIC_KEY_TOKEN*)(kb + sizeof(ECC_PRIVATE_KEY_TOKEN) + privlen);

	kp1->key_len = ecdsakey_length;
	kp1->reserved = 0x0020;
	kp1->tknhdr.tkn_hdr_id = 0x1E;
	kp1->tknhdr.tkn_length = ecdsakey_length - 2 - 2; /* 2x len field */

	kp1->privsec.section_id = 0x20;
	kp1->privsec.version = 0x00;
	kp1->privsec.section_len =  sizeof(ECC_PRIVATE_KEY_SECTION) + sizeof(ECC_ASSOCIATED_DATA) + privlen;
	kp1->privsec.key_usage = 0x80;
	kp1->privsec.curve_type = curve_type;
	kp1->privsec.key_format = 0x40; /* unencrypted key */
	kp1->privsec.priv_p_bitlen = priv_bitlen;
	kp1->privsec.associated_data_len = sizeof(ECC_ASSOCIATED_DATA);
	kp1->privsec.ibm_associated_data_len = sizeof(ECC_ASSOCIATED_DATA);
	kp1->privsec.formatted_data_len = privlen;

	kp1->adata.ibm_data_len = sizeof(ECC_ASSOCIATED_DATA);
	kp1->adata.curve_type = curve_type;
	kp1->adata.p_bitlen = priv_bitlen;
	kp1->adata.usage_flag = 0x80;
	kp1->adata.format_and_sec_flag = 0x40;

	memcpy(&kp1->privkey[0], privkey->D, privlen);

	kp2->pubsec.section_id = 0x21;
	kp2->pubsec.section_len = sizeof(ECC_PUBLIC_KEY_TOKEN) + 2*privlen;
	kp2->pubsec.curve_type = curve_type;
	kp2->pubsec.pub_p_bitlen = priv_bitlen;
	kp2->pubsec.pub_q_bytelen = 2*privlen + 1; /* bytelen + compress flag */

	kp2->compress_flag = 0x04; /* uncompressed key */
	memcpy(&kp2->pubkey[0], X, privlen);
	memcpy(&kp2->pubkey[privlen+0], Y, privlen);

	return sizeof(ECC_PRIVATE_KEY_TOKEN)
			+ privlen
			+ sizeof(ECC_PUBLIC_KEY_TOKEN)
			+ 2*privlen;
}

/**
 * makes an ECDSA verify key structure at given struct and returns its length.
 */
static unsigned int make_ecdsa_public_key_token(ECDSA_PUBLIC_KEY_BLOCK *kb,
		const ICA_EC_KEY *pubkey, uint8_t curve_type)
{
	int privlen = privlen_from_nid(pubkey->nid);
	unsigned int this_length = sizeof(ECDSA_PUBLIC_KEY_BLOCK) + 2*privlen;

	unsigned int priv_bitlen = privlen*8;
	if (pubkey->nid == NID_secp521r1) {
		priv_bitlen = 521;
	}

	kb->key_len = this_length;
	kb->tknhdr.tkn_hdr_id = 0x1E;
	kb->tknhdr.tkn_length = this_length - 2 - 2; /* 2x len field */

	kb->pubsec.section_id = 0x21;
	kb->pubsec.section_len = sizeof(ECC_PUBLIC_KEY_TOKEN) + 2*privlen;
	kb->pubsec.curve_type = curve_type;
	kb->pubsec.pub_p_bitlen = priv_bitlen;
	kb->pubsec.pub_q_bytelen = 2*privlen + 1; /* bytelen + compress flag */

	kb->compress_flag = 0x04; /* uncompressed key */
	memcpy(&kb->pubkey[0], pubkey->X, privlen);
	memcpy(&kb->pubkey[privlen+0], pubkey->Y, privlen);

	return this_length;
}

/**
 * creates an ECDSA sign request message for zcrypt. The given private key does usually
 * not contain a public key (X,Y), but the card requires (X,Y) to be present. The
 * calling function makes sure that (X,Y) are correctly set.
 *
 * returns a pointer to the control block where the card
 * provides its reply.
 *
 * The function allocates len bytes at cbrbmem. The caller
 * is responsible to erase sensible data and free the
 * memory.
 */
static ECDSA_SIGN_REPLY* make_ecdsa_sign_request(const ICA_EC_KEY *privkey,
		const unsigned char *X, const unsigned char *Y,
		const unsigned char *hash, unsigned int hash_length,
		struct ica_xcRB* xcrb, uint8_t **cbrbmem, size_t *len)
{
	struct CPRBX *preqcblk, *prepcblk;
	int privlen = privlen_from_nid(privkey->nid);

	unsigned int ecdsa_key_token_len = 2 + 2 + sizeof(CCA_TOKEN_HDR)
		+ sizeof(ECC_PRIVATE_KEY_SECTION)
		+ sizeof(ECC_ASSOCIATED_DATA) + privlen
		+ sizeof(ECC_PUBLIC_KEY_TOKEN) + 2*privlen;

	unsigned int keyblock_len = 2 + ecdsa_key_token_len;
	unsigned int parmblock_len = sizeof(ECDSA_PARMBLOCK_PART1)
				   + hash_length + keyblock_len;

	int curve_type = curve_type_from_nid(privkey->nid);
	if (curve_type < 0)
		return NULL;

	/* allocate buffer space for req cprb, req parm, rep cprb, rep parm */
	*len = 2 * (CPRBXSIZE + PARMBSIZE);
	*cbrbmem = malloc(*len);
	if (!*cbrbmem)
		return NULL;

	memset(*cbrbmem, 0, *len);
	preqcblk = (struct CPRBX *) *cbrbmem;
	prepcblk = (struct CPRBX *) (*cbrbmem + CPRBXSIZE + PARMBSIZE);

	/* make ECDSA sign request */
	unsigned int offset = 0;
	offset = make_cprbx((struct CPRBX *)*cbrbmem, parmblock_len, preqcblk, prepcblk);
	offset += make_ecdsa_sign_parmblock((ECDSA_PARMBLOCK_PART1*)
					    (*cbrbmem+offset), hash, hash_length);
	offset += make_keyblock_length((ECC_KEYBLOCK_LENGTH*)(*cbrbmem+offset), keyblock_len);
	offset += make_ecdsa_private_key_token(*cbrbmem+offset, privkey, X, Y, curve_type);
	finalize_xcrb(xcrb, preqcblk, prepcblk);

	return (ECDSA_SIGN_REPLY*)prepcblk;
}

/**
 * calculate the public (X,Y) values for the given private key, if necessary.
 */
static unsigned int provide_pubkey(const ICA_EC_KEY *privkey, unsigned char *X, unsigned char *Y)
{
	EC_KEY *eckey = NULL;
	EC_POINT *pub_key = NULL;
	const EC_GROUP *group = NULL;
	BIGNUM *bn_d = NULL, *bn_x = NULL, *bn_y = NULL;
	int privlen = -1;
	unsigned int i, n, rc;

	if (privkey == NULL || X == NULL || Y == NULL) {
		return EFAULT;
	}

	privlen = privlen_from_nid(privkey->nid);
	if (privlen < 0) {
		return EFAULT;
	}

	/* Check if (X,Y) already available */
	if (privkey->X != NULL && privkey->Y != NULL) {
		memcpy(X, privkey->X, privlen);
		memcpy(Y, privkey->Y, privlen);
		return 0;
	}

	/* Get (D) as BIGNUM */
	if ((bn_d = BN_bin2bn(privkey->D, privlen, NULL)) == NULL) {
		return EFAULT;
	}

	/* Calculate public (X,Y) values */
	eckey = EC_KEY_new_by_curve_name(privkey->nid);
	EC_KEY_set_private_key(eckey, bn_d);
	group = EC_KEY_get0_group(eckey);
	pub_key = EC_POINT_new(group);
	if (!EC_POINT_mul(group, pub_key, bn_d, NULL, NULL, NULL)) {
		rc = EFAULT;
		goto end;
	}

	/* Get (X,Y) as BIGNUMs */
	bn_x = BN_new();
	bn_y = BN_new();
	if (!EC_POINT_get_affine_coordinates_GFp(group, pub_key, bn_x, bn_y, NULL)) {
		rc = EFAULT;
		goto end;
	}

	/* Format (X) as char array, with leading zeros if necessary */
	n = privlen - BN_num_bytes(bn_x);
	for (i = 0; i < n; i++)
		X[i] = 0x00;
	BN_bn2bin(bn_x, &(X[n]));

	/* Format (Y) as char array, with leading zeros if necessary */
	n = privlen - BN_num_bytes(bn_y);
	for (i = 0; i < n; i++)
		Y[i] = 0x00;
	BN_bn2bin(bn_y, &(Y[n]));

	rc = 0;

end:

	if (pub_key)
		EC_POINT_free(pub_key);
	if (eckey)
		EC_KEY_free(eckey);
	BN_clear_free(bn_x);
	BN_clear_free(bn_y);
	BN_clear_free(bn_d);

	return rc;
}

/**
 * creates an ECDSA signature via CPACF or Crypto Express CCA coprocessor.
 *
 * Returns 0 if successful
 *         EIO if an internal error occurred
 */
unsigned int ecdsa_sign_hw(ica_adapter_handle_t adapter_handle,
		const ICA_EC_KEY *privkey, const unsigned char *hash, unsigned int hash_length,
		unsigned char *signature)
{
	uint8_t *buf;
	size_t len;
	int rc;
	struct ica_xcRB xcrb;
	ECDSA_SIGN_REPLY* reply_p;
	int privlen = privlen_from_nid(privkey->nid);
	unsigned char X[MAX_ECC_PRIV_SIZE];
	unsigned char Y[MAX_ECC_PRIV_SIZE];

	if (msa9_switch && !ica_offload_enabled) {
		rc = ecdsa_sign_cpacf(privkey, hash, hash_length, signature,
				      NULL);
		if (rc != EINVAL) /* EINVAL: curve not supported by cpacf */
			return rc;
	}

	if (!ecc_via_online_card)
		return ENODEV;

	if (adapter_handle == DRIVER_NOT_LOADED)
		return EIO;

	rc = provide_pubkey(privkey, X, Y);
	if (rc != 0)
		return EIO;

	reply_p = make_ecdsa_sign_request((const ICA_EC_KEY*)privkey,
			X, Y, hash, hash_length, &xcrb, &buf, &len);
	if (!reply_p)
		return EIO;

	rc = ioctl(adapter_handle, ZSECSENDCPRB, xcrb);
	if (rc != 0) {
		rc = EIO;
		goto ret;
	}

	if (reply_p->vud_len - 8 != 2 * privlen) {
		rc = EIO;
		goto ret;
	}

	memcpy(signature, reply_p->signature, reply_p->vud_len-8);
	rc = 0;
ret:
	OPENSSL_cleanse(buf, len);
	free(buf);
	return rc;
}

/**
 * creates an ECDSA signature in software using OpenSSL.
 * Returns 0 if successful
 *         EIO if an internal error occurred.
 */
unsigned int ecdsa_sign_sw(const ICA_EC_KEY *privkey,
		const unsigned char *hash, unsigned int hash_length,
		unsigned char *signature)
{
	int rc = EIO;
	EC_KEY *a = NULL;
	BIGNUM* r=NULL; BIGNUM* s=NULL;
	ECDSA_SIG* sig = NULL;
	unsigned int privlen = privlen_from_nid(privkey->nid);
	unsigned int i,n;

#ifdef ICA_FIPS
	if ((fips & ICA_FIPS_MODE) && (!FIPS_mode()))
		return EACCES;
#endif /* ICA_FIPS */

	if (!is_supported_openssl_curve(privkey->nid))
		return EINVAL;

	a = make_eckey(privkey->nid, privkey->D, privlen);
	if (!a)
		goto err;

	if (!EC_KEY_check_key(a))
		goto err;

	/**
	 * Make sure to use original openssl sign method to avoid endless loop when being
	 * called from IBMCA engine in software fallback.
	 */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
	ECDSA_set_method(a, ECDSA_OpenSSL());
#else
	EC_KEY_set_method(a, EC_KEY_OpenSSL());
#endif
	sig = ECDSA_do_sign(hash, hash_length, a);
	if (!sig)
		goto err;

	ECDSA_SIG_get0(sig, (const BIGNUM**)&r, (const BIGNUM **)&s);

	/* Insert leading 0x00's if r or s shorter than privlen */
	n = privlen - BN_num_bytes(r);
	for (i = 0; i < n; i++)
		signature[i] = 0x00;
	BN_bn2bin(r, &(signature[n]));

	n = privlen - BN_num_bytes(s);
	for (i = 0; i < n; i++)
		signature[privlen+i] = 0x00;
	BN_bn2bin(s, &(signature[privlen+n]));

	rc = 0;

err:
	ECDSA_SIG_free(sig); /* also frees r and s */
	EC_KEY_free(a);

	return (rc);
}

/**
 * creates an ECDSA xcrb request message for zcrypt.
 *
 * returns a pointer to the control block where the card
 * provides its reply.
 *
 * The function allocates len bytes at cbrbmem. The caller
 * is responsible to erase sensible data and free the
 * memory.
 */
static ECDSA_VERIFY_REPLY* make_ecdsa_verify_request(const ICA_EC_KEY *pubkey,
		const unsigned char *hash, unsigned int hash_length,
		const unsigned char *signature, struct ica_xcRB* xcrb,
		uint8_t **cbrbmem, size_t *len)
{
	struct CPRBX *preqcblk, *prepcblk;
	unsigned int privlen = privlen_from_nid(pubkey->nid);

	unsigned int ecdsa_key_token_len = 2 + 2 + sizeof(CCA_TOKEN_HDR)
		+ sizeof(ECC_PUBLIC_KEY_TOKEN) + 2*privlen;

	unsigned int keyblock_len = 2 + ecdsa_key_token_len;
	unsigned int parmblock_len = sizeof(ECDSA_PARMBLOCK_PART1) + hash_length
		+ sizeof(ECDSA_PARMBLOCK_PART2) + 2*privlen + keyblock_len;

	/* allocate buffer space for req cprb, req parm, rep cprb, rep parm */
	*len = 2 * (CPRBXSIZE + PARMBSIZE);
	*cbrbmem = malloc(*len);
	if (!*cbrbmem)
		return NULL;

	int curve_type = curve_type_from_nid(pubkey->nid);
	if (curve_type < 0)
		return NULL;

	memset(*cbrbmem, 0, *len);
	preqcblk = (struct CPRBX *) *cbrbmem;
	prepcblk = (struct CPRBX *) (*cbrbmem + CPRBXSIZE + PARMBSIZE);

	/* make ECDSA verify request */
	unsigned int offset = 0;
	offset = make_cprbx((struct CPRBX *)*cbrbmem, parmblock_len, preqcblk, prepcblk);
	offset += make_ecdsa_verify_parmblock((char*)(*cbrbmem+offset), hash,
					      hash_length, signature, 2*privlen);
	offset += make_keyblock_length((ECC_KEYBLOCK_LENGTH*)(*cbrbmem+offset), keyblock_len);
	offset += make_ecdsa_public_key_token((ECDSA_PUBLIC_KEY_BLOCK*)
					      (*cbrbmem+offset), pubkey, curve_type);
	finalize_xcrb(xcrb, preqcblk, prepcblk);

	return (ECDSA_VERIFY_REPLY*)prepcblk;
}

/*
 * Verify an ecdsa signature of a hashed message under a public key.
 * Returns 0 if successful. If cpacf doesnt support the curve,
 * EINVAL is returned.
 */
static int ecdsa_verify_cpacf(const ICA_EC_KEY *pub, const unsigned char *hash,
			      size_t hashlen, const unsigned char *sig)
{
#define DEF_PARAM(curve, size)		\
struct {				\
	unsigned char sig_r[size];	\
	unsigned char sig_s[size];	\
	unsigned char hash[size];	\
	unsigned char pub_x[size];	\
	unsigned char pub_y[size];	\
} curve

	union {
		long long buff[512];	/* 4k buffer: params + reserved area */
		DEF_PARAM(P256, 32);
		DEF_PARAM(P384, 48);
		DEF_PARAM(P521, 80);
	} param;

#undef DEF_PARAM

	unsigned long fc;
	size_t off;
	int rc;

	memset(&param, 0, sizeof(param));
	rc = 0;

	switch (pub->nid) {
	case NID_X9_62_prime256v1:
		off = sizeof(param.P256.hash)
		      - (hashlen > sizeof(param.P256.hash) ?
		      sizeof(param.P256.hash) : hashlen);

		memcpy(param.P256.hash + off, hash,
		       sizeof(param.P256.hash) - off);

		off = sizeof(param.P256.pub_x)
				 - privlen_from_nid(pub->nid);

		memcpy(param.P256.sig_r + off, sig,
		       sizeof(param.P256.sig_r) - off);
		memcpy(param.P256.sig_s + off, sig +
		       sizeof(param.P256.sig_r) - off,
		       sizeof(param.P256.sig_s) - off);
		memcpy(param.P256.pub_x + off, pub->X,
		       sizeof(param.P256.pub_x) - off);
		memcpy(param.P256.pub_y + off, pub->Y,
		       sizeof(param.P256.pub_y) - off);

		fc = s390_kdsa_functions[ECDSA_VERIFY_P256].hw_fc;
		break;

	case NID_secp384r1:
		off = sizeof(param.P384.hash)
		      - (hashlen > sizeof(param.P384.hash) ?
		      sizeof(param.P384.hash) : hashlen);

		memcpy(param.P384.hash + off, hash,
		       sizeof(param.P384.hash) - off);

		off = sizeof(param.P384.pub_x)
				 - privlen_from_nid(pub->nid);

		memcpy(param.P384.sig_r + off, sig,
		       sizeof(param.P384.sig_r) - off);
		memcpy(param.P384.sig_s + off, sig +
		       sizeof(param.P384.sig_r) - off,
		       sizeof(param.P384.sig_s) - off);
		memcpy(param.P384.pub_x + off, pub->X,
		       sizeof(param.P384.pub_x) - off);
		memcpy(param.P384.pub_y + off, pub->Y,
		       sizeof(param.P384.pub_y) - off);

		fc = s390_kdsa_functions[ECDSA_VERIFY_P384].hw_fc;
		break;

	case NID_secp521r1:
		off = sizeof(param.P521.hash)
		      - (hashlen > sizeof(param.P521.hash) ?
		      sizeof(param.P521.hash) : hashlen);

		memcpy(param.P521.hash + off, hash,
		       sizeof(param.P521.hash) - off);

		off = sizeof(param.P521.pub_x)
				 - privlen_from_nid(pub->nid);

		memcpy(param.P521.sig_r + off, sig,
		       sizeof(param.P521.sig_r) - off);
		memcpy(param.P521.sig_s + off, sig +
		       sizeof(param.P521.sig_r) - off,
		       sizeof(param.P521.sig_s) - off);
		memcpy(param.P521.pub_x + off, pub->X,
		       sizeof(param.P521.pub_x) - off);
		memcpy(param.P521.pub_y + off, pub->Y,
		       sizeof(param.P521.pub_y) - off);

		fc = s390_kdsa_functions[ECDSA_VERIFY_P521].hw_fc;
		break;

	default:
		rc = EINVAL;
		break;
	}

	if (!rc)
		rc = s390_kdsa(fc, param.buff, NULL, 0) ? EFAULT : 0;

	return rc;
}

/*
 * Sign a hashed message using under a private key.
 * Returns 0 if successful. If cpacf doesnt support the curve,
 * EINVAL is returned.
 */
static int ecdsa_sign_cpacf(const ICA_EC_KEY *priv, const unsigned char *hash,
			    size_t hashlen, unsigned char *sig,
			    void (*rng_cb)(unsigned char *, size_t))
{
#define DEF_PARAM(curve, size)		\
struct {				\
	unsigned char sig_r[size];	\
	unsigned char sig_s[size];	\
	unsigned char hash[size];	\
	unsigned char priv[size];	\
	unsigned char rand[size];	\
} curve

	union {
		long long buff[512];	/* 4k buffer: params + reserved area */
		DEF_PARAM(P256, 32);
		DEF_PARAM(P384, 48);
		DEF_PARAM(P521, 80);
	} param;

#undef DEF_PARAM

	unsigned long fc;
	size_t off;
	int rc;

	memset(&param, 0, sizeof(param));
	rc = 0;

	switch (priv->nid) {
	case NID_X9_62_prime256v1:
		off = sizeof(param.P256.hash)
		      - (hashlen > sizeof(param.P256.hash) ?
		      sizeof(param.P256.hash) : hashlen);

		memcpy(param.P256.hash + off, hash,
		       sizeof(param.P256.hash) - off);

		off = sizeof(param.P256.priv)
				 - privlen_from_nid(priv->nid);

		memcpy(param.P256.priv + off, priv->D,
		       sizeof(param.P256.priv) - off);


		fc = s390_kdsa_functions[ECDSA_SIGN_P256].hw_fc;

		if (rng_cb == NULL) {
			rc = s390_kdsa(fc, param.buff, NULL, 0);
		} else {
			fc |= 0x80; /* deterministic signature */
			do {
				rng_cb(param.P256.rand + off,
				       sizeof(param.P256.rand) - off);
				rc = s390_kdsa(fc, param.buff, NULL, 0);
			} while (rc);
		}

		memcpy(sig, param.P256.sig_r + off,
		       sizeof(param.P256.sig_r) - off);
		memcpy(sig + sizeof(param.P256.sig_r) - off,
		       param.P256.sig_s + off,
		       sizeof(param.P256.sig_s) - off);

		OPENSSL_cleanse(param.P256.priv,
				sizeof(param.P256.priv));
		OPENSSL_cleanse(param.P256.rand,
				sizeof(param.P256.rand));
		break;

	case NID_secp384r1:
		off = sizeof(param.P384.hash)
		      - (hashlen > sizeof(param.P384.hash) ?
		      sizeof(param.P384.hash) : hashlen);

		memcpy(param.P384.hash + off, hash,
		       sizeof(param.P384.hash) - off);

		off = sizeof(param.P384.priv)
				 - privlen_from_nid(priv->nid);

		memcpy(param.P384.priv + off, priv->D,
		       sizeof(param.P384.priv) - off);

		fc = s390_kdsa_functions[ECDSA_SIGN_P384].hw_fc;

		if (rng_cb == NULL) {
			rc = s390_kdsa(fc, param.buff, NULL, 0);
		} else {
			fc |= 0x80; /* deterministic signature */
			do {
				rng_cb(param.P384.rand + off,
				       sizeof(param.P384.rand) - off);
				rc = s390_kdsa(fc, param.buff, NULL, 0);
			} while (rc);
		}

		memcpy(sig, param.P384.sig_r + off,
		       sizeof(param.P384.sig_r) - off);
		memcpy(sig + sizeof(param.P384.sig_r) - off,
		       param.P384.sig_s + off,
		       sizeof(param.P384.sig_s) - off);

		OPENSSL_cleanse(param.P384.priv,
				sizeof(param.P384.priv));
		OPENSSL_cleanse(param.P384.rand,
				sizeof(param.P384.rand));
		break;

	case NID_secp521r1:
		off = sizeof(param.P521.hash)
		      - (hashlen > sizeof(param.P521.hash) ?
		      sizeof(param.P521.hash) : hashlen);

		memcpy(param.P521.hash + off, hash,
		       sizeof(param.P521.hash) - off);

		off = sizeof(param.P521.priv)
				 - privlen_from_nid(priv->nid);

		memcpy(param.P521.priv + off, priv->D,
		       sizeof(param.P521.priv) - off);

		fc = s390_kdsa_functions[ECDSA_SIGN_P521].hw_fc;

		if (rng_cb == NULL) {
			rc = s390_kdsa(fc, param.buff, NULL, 0);
		} else {
			fc |= 0x80; /* deterministic signature */
			do {
				rng_cb(param.P521.rand + off,
				       sizeof(param.P521.rand) - off);
				rc = s390_kdsa(fc, param.buff, NULL, 0);
			} while (rc);
		}

		memcpy(sig, param.P521.sig_r + off,
		       sizeof(param.P521.sig_r) - off);
		memcpy(sig + sizeof(param.P521.sig_r) - off,
		       param.P521.sig_s + off,
		       sizeof(param.P521.sig_s) - off);

		OPENSSL_cleanse(param.P521.priv,
				sizeof(param.P521.priv));
		OPENSSL_cleanse(param.P521.rand,
				sizeof(param.P521.rand));
		break;

	default:
		rc = EINVAL;
		break;
	}

	return rc;
}

/**
 * verifies an ECDSA signature via CPACF or Crypto Express CCA coprocessor.
 *
 * Returns 0 if successful
 *         EIO if an internal error occurred
 *         EFAULT if signature invalid
 */
unsigned int ecdsa_verify_hw(ica_adapter_handle_t adapter_handle,
		const ICA_EC_KEY *pubkey, const unsigned char *hash, unsigned int hash_length,
		const unsigned char *signature)
{
	uint8_t *buf;
	size_t len;
	int rc;
	struct ica_xcRB xcrb;
	ECDSA_VERIFY_REPLY* reply_p;

	if (msa9_switch && !ica_offload_enabled) {
		rc = ecdsa_verify_cpacf(pubkey, hash, hash_length, signature);
		if (rc != EINVAL) /* EINVAL: curve not supported by cpacf */
			return rc;
	}

	if (!ecc_via_online_card)
		return ENODEV;

	if (adapter_handle == DRIVER_NOT_LOADED)
		return EIO;

	reply_p = make_ecdsa_verify_request(pubkey, hash, hash_length,
					    signature, &xcrb, &buf, &len);
	if (!reply_p)
		return EIO;

	rc = ioctl(adapter_handle, ZSECSENDCPRB, xcrb);
	if (rc != 0) {
		rc = EIO;
		goto ret;
	}

	if (((struct CPRBX*)reply_p)->ccp_rtcode == 4 &&
		((struct CPRBX*)reply_p)->ccp_rscode == RS_SIGNATURE_INVALID) {
		rc = EFAULT;
		goto ret;
	}

	if (((struct CPRBX*)reply_p)->ccp_rtcode != 0 ||
		((struct CPRBX*)reply_p)->ccp_rscode != 0) {
		rc = EIO;
		goto ret;
	}

	rc = 0;
ret:
	OPENSSL_cleanse(buf, len);
	free(buf);
	return rc;
}

/**
 * verifies an ECDSA signature in software using OpenSSL.
 *
 * Returns 0 if successful
 *         EIO if an internal error occurred
 *         EFAULT if signature invalid.
 */
unsigned int ecdsa_verify_sw(const ICA_EC_KEY *pubkey,
		const unsigned char *hash, unsigned int hash_length,
		const unsigned char *signature) {
	int rc = EIO;
	EC_KEY *a = NULL;
	BIGNUM *r, *s, *xa, *ya;
	ECDSA_SIG* sig = NULL;
	unsigned int privlen = privlen_from_nid(pubkey->nid);

#ifdef ICA_FIPS
	if ((fips & ICA_FIPS_MODE) && (!FIPS_mode()))
	return EACCES;
#endif /* ICA_FIPS */

	if (!is_supported_openssl_curve(pubkey->nid))
		return EINVAL;

	/* create public key with given (x,y) */
	xa = BN_bin2bn(pubkey->X, privlen, NULL);
	ya = BN_bin2bn(pubkey->Y, privlen, NULL);
	a = make_public_eckey(pubkey->nid, xa, ya, privlen);
	if (!a) {
		goto err;
	}

	/* create ECDSA_SIG instance */
	sig = ECDSA_SIG_new();
	r = BN_bin2bn(signature, privlen, NULL);
	s = BN_bin2bn(signature + privlen, privlen, NULL);
	ECDSA_SIG_set0(sig, r, s);

	/**
	 * Make sure to use original openssl verify method to avoid endless loop
	 * when being called from IBMCA engine in software fallback.
	 */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
	ECDSA_set_method(a, ECDSA_OpenSSL());
#else
	EC_KEY_set_method(a, EC_KEY_OpenSSL());
#endif
	rc = ECDSA_do_verify(hash, hash_length, sig, a);
	switch (rc) {
	case 0: /* signature invalid */
		rc = EFAULT;
		break;
	case 1: /* signature valid */
		rc = 0;
		break;
	default: /* internal error */
		rc = EIO;
		break;
	}

err:
	BN_clear_free(xa);
	BN_clear_free(ya);
	ECDSA_SIG_free(sig);
	EC_KEY_free(a);

	return rc;
}

/**
 * makes an ECKeyGen parmblock at given struct and returns its length.
 */
static unsigned int make_eckeygen_parmblock(ECKEYGEN_PARMBLOCK *pb)
{
	pb->subfunc_code = 0x5047; /* 'PG' */
	pb->rule_array.rule_array_len = 0x000A;
	memcpy(&(pb->rule_array.rule_array_cmd), "CLEAR   ", 8);
	pb->vud_len = 0x0002;

	return sizeof(ECKEYGEN_PARMBLOCK);
}

/**
 * makes an ECKeyGen private key structure at given struct and returns its length.
 */
static unsigned int make_eckeygen_private_key_token(ECKEYGEN_KEY_TOKEN* kb,
		unsigned int nid, uint8_t curve_type)
{
	unsigned int privlen = privlen_from_nid(nid);

	unsigned int priv_bitlen = privlen*8;
	if (nid == NID_secp521r1) {
		priv_bitlen = 521;
	}

	kb->key_len = sizeof(ECKEYGEN_KEY_TOKEN);
	kb->reserved1 = 0x0020;
	kb->tknhdr.tkn_hdr_id = 0x1E;
	kb->tknhdr.tkn_length = sizeof(ECKEYGEN_KEY_TOKEN) - 2 - 2; /* 2x len field */

	kb->privsec.section_id = 0x20;
	kb->privsec.version = 0x00;
	kb->privsec.section_len =  sizeof(ECC_PRIVATE_KEY_SECTION) + sizeof(ECC_ASSOCIATED_DATA);
	kb->privsec.key_usage = 0x80;
	kb->privsec.curve_type = curve_type;
	kb->privsec.key_format = 0x40; /* unencrypted key */
	kb->privsec.priv_p_bitlen = priv_bitlen;
	kb->privsec.associated_data_len = sizeof(ECC_ASSOCIATED_DATA);
	kb->privsec.ibm_associated_data_len = sizeof(ECC_ASSOCIATED_DATA);
	kb->privsec.formatted_data_len = 0; /* no key */

	kb->adata.ibm_data_len = sizeof(ECC_ASSOCIATED_DATA);
	kb->adata.curve_type = curve_type;
	kb->adata.p_bitlen = priv_bitlen;
	kb->adata.usage_flag = 0x80;
	kb->adata.format_and_sec_flag = 0x40;

	kb->pubsec.section_id = 0x21;
	kb->pubsec.section_len = sizeof(ECC_PUBLIC_KEY_SECTION);
	kb->pubsec.curve_type = curve_type;
	kb->pubsec.pub_p_bitlen = priv_bitlen;
	kb->pubsec.pub_q_bytelen = 0; /* no keys */

	return sizeof(ECKEYGEN_KEY_TOKEN);
}

/**
 * creates an ECKeyGen xcrb request message for zcrypt.
 *
 * returns a pointer to the control block where the card
 * provides its reply.
 *
 * The function allocates len bytes at cbrbmem. The caller
 * is responsible to erase sensible data and free the
 * memory.
 */
static ECKEYGEN_REPLY* make_eckeygen_request(ICA_EC_KEY *key,
					     struct ica_xcRB* xcrb,
					     uint8_t **cbrbmem, size_t *len)
{
	struct CPRBX *preqcblk, *prepcblk;

	unsigned int keyblock_len = 2 + sizeof(ECKEYGEN_KEY_TOKEN)
			+ sizeof(ECC_NULL_TOKEN);
	unsigned int parmblock_len = sizeof(ECKEYGEN_PARMBLOCK) + keyblock_len;

	int curve_type = curve_type_from_nid(key->nid);
	if (curve_type < 0)
		return NULL;

	/* allocate buffer space for req cprb, req parm, rep cprb, rep parm */
	*len = 2 * (CPRBXSIZE + PARMBSIZE);
	*cbrbmem = malloc(*len);
	if (!*cbrbmem)
		return NULL;

	memset(*cbrbmem, 0, *len);
	preqcblk = (struct CPRBX *) *cbrbmem;
	prepcblk = (struct CPRBX *) (*cbrbmem + CPRBXSIZE + PARMBSIZE);

	/* make ECKeyGen request */
	unsigned int offset = 0;
	offset = make_cprbx((struct CPRBX *)*cbrbmem, parmblock_len, preqcblk, prepcblk);
	offset += make_eckeygen_parmblock((ECKEYGEN_PARMBLOCK*)(*cbrbmem+offset));
	offset += make_keyblock_length((ECC_KEYBLOCK_LENGTH*)(*cbrbmem+offset), keyblock_len);
	offset += make_eckeygen_private_key_token((ECKEYGEN_KEY_TOKEN*)(*cbrbmem+offset), key->nid, curve_type);
	offset += make_ecc_null_token((ECC_NULL_TOKEN*)(*cbrbmem+offset));
	finalize_xcrb(xcrb, preqcblk, prepcblk);

	return (ECKEYGEN_REPLY*)prepcblk;
}

static int eckeygen_cpacf(ICA_EC_KEY *key)
{
	static const unsigned char p256_base_x[] = {
	0x6B, 0x17, 0xD1, 0xF2, 0xE1, 0x2C, 0x42, 0x47, 0xF8, 0xBC, 0xE6, 0xE5,
	0x63, 0xA4, 0x40, 0xF2, 0x77, 0x03, 0x7D, 0x81, 0x2D, 0xEB, 0x33, 0xA0,
	0xF4, 0xA1, 0x39, 0x45, 0xD8, 0x98, 0xC2, 0x96
	};
	static const unsigned char p256_base_y[] = {
	0x4F, 0xE3, 0x42, 0xE2, 0xFE, 0x1A, 0x7F, 0x9B, 0x8E, 0xE7, 0xEB, 0x4A,
	0x7C, 0x0F, 0x9E, 0x16, 0x2B, 0xCE, 0x33, 0x57, 0x6B, 0x31, 0x5E, 0xCE,
	0xCB, 0xB6, 0x40, 0x68, 0x37, 0xBF, 0x51, 0xF5
	};
	static const unsigned char p256_ord[] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84,
	0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51
	};

	static const unsigned char p384_base_x[] = {
	0xAA, 0x87, 0xCA, 0x22, 0xBE, 0x8B, 0x05, 0x37, 0x8E, 0xB1, 0xC7, 0x1E,
	0xF3, 0x20, 0xAD, 0x74, 0x6E, 0x1D, 0x3B, 0x62, 0x8B, 0xA7, 0x9B, 0x98,
	0x59, 0xF7, 0x41, 0xE0, 0x82, 0x54, 0x2A, 0x38, 0x55, 0x02, 0xF2, 0x5D,
	0xBF, 0x55, 0x29, 0x6C, 0x3A, 0x54, 0x5E, 0x38, 0x72, 0x76, 0x0A, 0xB7
	};
	static const unsigned char p384_base_y[] = {
	0x36, 0x17, 0xDE, 0x4A, 0x96, 0x26, 0x2C, 0x6F, 0x5D, 0x9E, 0x98, 0xBF,
	0x92, 0x92, 0xDC, 0x29, 0xF8, 0xF4, 0x1D, 0xBD, 0x28, 0x9A, 0x14, 0x7C,
	0xE9, 0xDA, 0x31, 0x13, 0xB5, 0xF0, 0xB8, 0xC0, 0x0A, 0x60, 0xB1, 0xCE,
	0x1D, 0x7E, 0x81, 0x9D, 0x7A, 0x43, 0x1D, 0x7C, 0x90, 0xEA, 0x0E, 0x5F
	};
	static const unsigned char p384_ord[] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xC7, 0x63, 0x4D, 0x81, 0xF4, 0x37, 0x2D, 0xDF, 0x58, 0x1A, 0x0D, 0xB2,
	0x48, 0xB0, 0xA7, 0x7A, 0xEC, 0xEC, 0x19, 0x6A, 0xCC, 0xC5, 0x29, 0x73
	};

	static const unsigned char p521_base_x[] = {
	0x00, 0xC6, 0x85, 0x8E, 0x06, 0xB7, 0x04, 0x04, 0xE9, 0xCD, 0x9E, 0x3E,
	0xCB, 0x66, 0x23, 0x95, 0xB4, 0x42, 0x9C, 0x64, 0x81, 0x39, 0x05, 0x3F,
	0xB5, 0x21, 0xF8, 0x28, 0xAF, 0x60, 0x6B, 0x4D, 0x3D, 0xBA, 0xA1, 0x4B,
	0x5E, 0x77, 0xEF, 0xE7, 0x59, 0x28, 0xFE, 0x1D, 0xC1, 0x27, 0xA2, 0xFF,
	0xA8, 0xDE, 0x33, 0x48, 0xB3, 0xC1, 0x85, 0x6A, 0x42, 0x9B, 0xF9, 0x7E,
	0x7E, 0x31, 0xC2, 0xE5, 0xBD, 0x66
	};
	static const unsigned char p521_base_y[] = {
	0x01, 0x18, 0x39, 0x29, 0x6A, 0x78, 0x9A, 0x3B, 0xC0, 0x04, 0x5C, 0x8A,
	0x5F, 0xB4, 0x2C, 0x7D, 0x1B, 0xD9, 0x98, 0xF5, 0x44, 0x49, 0x57, 0x9B,
	0x44, 0x68, 0x17, 0xAF, 0xBD, 0x17, 0x27, 0x3E, 0x66, 0x2C, 0x97, 0xEE,
	0x72, 0x99, 0x5E, 0xF4, 0x26, 0x40, 0xC5, 0x50, 0xB9, 0x01, 0x3F, 0xAD,
	0x07, 0x61, 0x35, 0x3C, 0x70, 0x86, 0xA2, 0x72, 0xC2, 0x40, 0x88, 0xBE,
	0x94, 0x76, 0x9F, 0xD1, 0x66, 0x50
	};
	static const unsigned char p521_ord[] = {
	0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFA, 0x51, 0x86,
	0x87, 0x83, 0xBF, 0x2F, 0x96, 0x6B, 0x7F, 0xCC, 0x01, 0x48, 0xF7, 0x09,
	0xA5, 0xD0, 0x3B, 0xB5, 0xC9, 0xB8, 0x89, 0x9C, 0x47, 0xAE, 0xBB, 0x6F,
	0xB7, 0x1E, 0x91, 0x38, 0x64, 0x09
	};

	const unsigned int privlen = privlen_from_nid(key->nid);
	const unsigned char *base_x, *base_y;

	BIGNUM *priv, *ord;
	BN_CTX *ctx;
	int rv, numbytes;

	ctx = BN_CTX_new();
	priv = BN_new();
	ord = BN_new();

	if (ctx == NULL || priv == NULL || ord == NULL) {
		rv = ENOMEM;
		goto out;
	}

	switch (key->nid) {
	case NID_X9_62_prime256v1:
		base_x = p256_base_x;
		base_y = p256_base_y;
		BN_bin2bn(p256_ord, sizeof(p256_ord), ord);
		break;
	case NID_secp384r1:
		base_x = p384_base_x;
		base_y = p384_base_y;
		BN_bin2bn(p384_ord, sizeof(p384_ord), ord);
		break;
	case NID_secp521r1:
		base_x = p521_base_x;
		base_y = p521_base_y;
		BN_bin2bn(p521_ord, sizeof(p521_ord), ord);
		break;
	default:
		rv = EINVAL;
		goto out;
	}

	do {
		if (!BN_rand_range(priv, ord)) {
			rv = EIO;
			goto out;
		}
	} while (BN_is_zero(priv));

	memset(key->D, 0, privlen);
	numbytes = BN_num_bytes(priv);

	rv = BN_bn2bin(priv, key->D + privlen - numbytes);
	BN_clear(priv);
	if (rv != numbytes) {
		rv = EIO;
		goto out;
	}

	rv = scalar_mul_cpacf(key->X, key->Y, key->D, base_x, base_y,
			      key->nid);

	out:
		if (ctx != NULL)
			BN_CTX_free(ctx);
		if (priv != NULL)
			BN_free(priv);
		if (ord != NULL)
			BN_free(ord);

	return rv;
}

/**
 * generates an EC key via Crypto Express CCA coprocessor.
 *
 * Returns 0 if successful
 *         EIO if an internal error occurred.
 */
unsigned int eckeygen_hw(ica_adapter_handle_t adapter_handle, ICA_EC_KEY *key)
{
	uint8_t *buf;
	size_t len;
	int rc;
	struct ica_xcRB xcrb;
	ECKEYGEN_REPLY *reply_p;
	unsigned int privlen = privlen_from_nid(key->nid);
	ECC_PUBLIC_KEY_TOKEN* pub_p;
	unsigned char* p;

	if (msa9_switch) {
		rc = eckeygen_cpacf(key);
		if (rc != EINVAL)	/* curve not supported by cpacf */
			return rc;
	}

	if (!ecc_via_online_card)
		return ENODEV;

	reply_p = make_eckeygen_request(key, &xcrb, &buf, &len);
	if (!reply_p)
		return EIO;

	rc = ioctl(adapter_handle, ZSECSENDCPRB, xcrb);
	if (rc != 0) {
		rc = EIO;
		goto ret;
	}

	if (reply_p->eckey.privsec.formatted_data_len != privlen) {
		rc = EIO;
		goto ret;
	}

	memcpy(key->D, reply_p->eckey.privkey, privlen);

	p = (unsigned char*)&(reply_p->eckey.privsec) + reply_p->eckey.privsec.section_len;
	pub_p = (ECC_PUBLIC_KEY_TOKEN*)p;
	if (pub_p->compress_flag != 0x04) {
		rc = EIO;
		goto ret;
	}

	memcpy(key->X, (char*)pub_p->pubkey, 2*privlen);
	rc = 0;
ret:
	OPENSSL_cleanse(buf, len);
	free(buf);
	return rc;
}

/**
 * generates an EC key in software using OpenSSL.
 *
 * Returns 0 if successful
 *         EIO if an internal error occurred.
 */
unsigned int eckeygen_sw(ICA_EC_KEY *key)
{
	int rc = EIO;
	EC_KEY *a = NULL;
	BIGNUM* d=NULL; BIGNUM *x=NULL; BIGNUM *y=NULL;
	const EC_POINT* q=NULL;
	const EC_GROUP *group=NULL;
	unsigned int privlen = privlen_from_nid(key->nid);
	unsigned int i, n;

#ifdef ICA_FIPS
	if ((fips & ICA_FIPS_MODE) && (!FIPS_mode()))
		return EACCES;
#endif /* ICA_FIPS */

	if (!is_supported_openssl_curve(key->nid))
		return EINVAL;

	if ((a = EC_KEY_new_by_curve_name(key->nid)) == NULL)
		goto err;

	if ((group = EC_KEY_get0_group(a)) == NULL)
		goto err;

#if OPENSSL_VERSION_NUMBER < 0x10100000L
	/* Openssl 1.0.2 does not provide a default ec_key_gen method, so IBMCA does not
	 * override such a function. Therefore nothing to do here.
	 */
#else
	/**
	 * IBMCA overrides the default ec_key_gen method for openssl 1.1.0 and later.
	 * Make sure to use original openssl method to avoid endless loop when being
	 * called from IBMCA engine in software fallback.
	 */
	EC_KEY_set_method(a, EC_KEY_OpenSSL());
#endif

	if (!EC_KEY_generate_key(a))
		goto err;

	/* provide private key */
	d = (BIGNUM*)EC_KEY_get0_private_key(a);
	n = privlen - BN_num_bytes(d);
	for (i=0;i<n;i++)
		key->D[i] = 0x00;
	BN_bn2bin(d, &(key->D[n]));

	/* provide public key */
	q = EC_KEY_get0_public_key(a);
	x = BN_new();
	y = BN_new();
	if (!EC_POINT_get_affine_coordinates_GFp(group, q, x, y, NULL))
		goto err;

	/* pub(X) */
	n = privlen - BN_num_bytes(x);
	for (i=0; i<n; i++)
		key->X[i] = 0x00;
	BN_bn2bin(x, &(key->X[n]));

	/* pub(Y) */
	n = privlen - BN_num_bytes(y);
	for (i=0; i<n; i++)
		key->Y[i] = 0x00;
	BN_bn2bin(y, &(key->Y[n]));

	rc = 0;

err:
	/* cleanup */
	EC_KEY_free(a); /* also frees d */
	BN_clear_free(x);
	BN_clear_free(y);

	return rc;
}


/*
 * Derive public key.
 * Returns 0 if successful. Caller has to check for MSA 9.
 */
int x25519_derive_pub(unsigned char pub[32],
		      const unsigned char priv[32])
{
	static const unsigned char x25519_base_u[] = {
	0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	int rc;

	rc = scalar_mulx_cpacf(pub, priv, x25519_base_u, NID_X25519);

	stats_increment(ICA_STATS_X25519_KEYGEN, ALGO_HW, ENCRYPT);
	return rc;
}

int x448_derive_pub(unsigned char pub[56],
		    const unsigned char priv[56])
{
	static const unsigned char x448_base_u[] = {
	0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	int rc;

	rc = scalar_mulx_cpacf(pub, priv, x448_base_u, NID_X448);

	stats_increment(ICA_STATS_X448_KEYGEN, ALGO_HW, ENCRYPT);
	return rc;
}

int ed25519_derive_pub(unsigned char pub[32],
		       const unsigned char priv[32])
{
	/* base point coordinates (big-endian) */
	static const unsigned char base_x[] = {
            0x21, 0x69, 0x36, 0xd3, 0xcd, 0x6e, 0x53, 0xfe,
            0xc0, 0xa4, 0xe2, 0x31, 0xfd, 0xd6, 0xdc, 0x5c,
            0x69, 0x2c, 0xc7, 0x60, 0x95, 0x25, 0xa7, 0xb2,
            0xc9, 0x56, 0x2d, 0x60, 0x8f, 0x25, 0xd5, 0x1a,
	};
	static const unsigned char base_y[] = {
	    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
	    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
	    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
	    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x58,
	};

	uint64_t lo, hi;
	unsigned char buf[64];
	unsigned char res_x[32];
	int rc;

	lo = 0;
	hi = 0;
	rc = s390_sha512(NULL, (unsigned char *)priv, 32, buf,
			 SHA_MSG_PART_ONLY, &lo, &hi);
	if (rc)
		goto out;

	buf[0] &= -8;	/* ensure multiple of cofactor */
	buf[31] &= 0x3f;
	buf[31] |= 0x40;

	/* to big endian */
	s390_flip_endian_32(buf, buf);

	rc = scalar_mul_cpacf(res_x, pub, buf, base_x, base_y, NID_ED25519);
	if (rc)
		goto out;

	/* to little endian */
	s390_flip_endian_32(res_x, res_x);
	s390_flip_endian_32(pub, pub);

	pub[31] |= ((res_x[0] & 0x01) << 7);

	/* to big endian */
	s390_flip_endian_32(pub, pub);

	stats_increment(ICA_STATS_ED25519_KEYGEN, ALGO_HW, ENCRYPT);
	rc = 0;
out:
	return rc;
}

/*
 * Derive public key.
 * Returns 0 if successful. Caller has to check for MSA 9.
 */
int ed448_derive_pub(unsigned char pub[57],
		     const unsigned char priv[57])
{
	/* base point coordinates (big-endian) */
	static const unsigned char base_x[] = {
	    0x00,
	    0x4f, 0x19, 0x70, 0xc6, 0x6b, 0xed, 0x0d, 0xed,
	    0x22, 0x1d, 0x15, 0xa6, 0x22, 0xbf, 0x36, 0xda,
	    0x9e, 0x14, 0x65, 0x70, 0x47, 0x0f, 0x17, 0x67,
	    0xea, 0x6d, 0xe3, 0x24, 0xa3, 0xd3, 0xa4, 0x64,
	    0x12, 0xae, 0x1a, 0xf7, 0x2a, 0xb6, 0x65, 0x11,
	    0x43, 0x3b, 0x80, 0xe1, 0x8b, 0x00, 0x93, 0x8e,
	    0x26, 0x26, 0xa8, 0x2b, 0xc7, 0x0c, 0xc0, 0x5e,
	};
	static const unsigned char base_y[] = {
	    0x00,
	    0x69, 0x3f, 0x46, 0x71, 0x6e, 0xb6, 0xbc, 0x24,
	    0x88, 0x76, 0x20, 0x37, 0x56, 0xc9, 0xc7, 0x62,
	    0x4b, 0xea, 0x73, 0x73, 0x6c, 0xa3, 0x98, 0x40,
	    0x87, 0x78, 0x9c, 0x1e, 0x05, 0xa0, 0xc2, 0xd7,
	    0x3a, 0xd3, 0xff, 0x1c, 0xe6, 0x7c, 0x39, 0xc4,
	    0xfd, 0xbd, 0x13, 0x2c, 0x4e, 0xd7, 0xc8, 0xad,
	    0x98, 0x08, 0x79, 0x5b, 0xf2, 0x30, 0xfa, 0x14,
	};

	uint64_t lo, hi;
	unsigned char buf[114], pub64[64];
	unsigned char res_x[64];
	int rc;

	memset(res_x, 0, sizeof(res_x));
	memset(pub64, 0, sizeof(pub64));

	lo = 0;
	hi = 0;
	rc = s390_shake_256(NULL, (unsigned char *)priv, 57, buf, sizeof(buf),
			    SHA_MSG_PART_ONLY, &lo, &hi);
	if (rc)
		goto out;

	memset(buf + 57, 0, 57);
	buf[0] &= -4;	/* ensure multiple of cofactor */
	buf[55] |= 0x80;
	buf[56] = 0;

	/* to big endian */
	s390_flip_endian_64(buf, buf);

	rc = scalar_mul_cpacf(res_x + 64 - 57, pub64 + 64 - 57, buf + 64 - 57,
			      base_x, base_y, NID_ED448);
	if (rc)
		goto out;

	/* to little endian */
	s390_flip_endian_64(res_x, res_x);
	s390_flip_endian_64(pub64, pub64);

	pub64[56] |= ((res_x[0] & 0x01) << 7);

	/* to big endian */
	s390_flip_endian_64(pub64, pub64);

	memcpy(pub, pub64 + 64 - 57, 57);
	stats_increment(ICA_STATS_ED448_KEYGEN, ALGO_HW, ENCRYPT);
	rc = 0;
out:
	return rc;
}

#ifdef ICA_INTERNAL_TEST_EC

#include "../test/testcase.h"
#include "test_vec.h"

#define TEST_ERROR(msg, alg, tv)					    \
do {									    \
	fprintf(stderr, "ERROR: %s. (%s test vector %lu)\n", msg, alg, tv); \
	exit(TEST_FAIL);						    \
} while(0)

static void ecdsa_test(void)
{
	unsigned long long rnd[2];
	sha_context_t sha_ctx;
	sha256_context_t sha256_ctx;
	sha512_context_t sha512_ctx;
	unsigned char hash[1024];
	unsigned char sig[4096];
	size_t hashlen;
	const struct ecdsa_tv *t;
	size_t i;
	int rc;

	verbosity_ = 2;
	t = &ECDSA_TV[0];

	for (i = 0; i < ECDSA_TV_LEN; i++) {
		switch (t->hash) {
		case SHA1:
			rc = ica_sha1(SHA_MSG_PART_ONLY, t->msglen, t->msg,
				      &sha_ctx, hash);
			hashlen = SHA1_HASH_LENGTH;
			break;
		case SHA224:
			rc = ica_sha224(SHA_MSG_PART_ONLY, t->msglen, t->msg,
				        &sha256_ctx, hash);
			hashlen = SHA224_HASH_LENGTH;
			break;
		case SHA256:
			rc = ica_sha256(SHA_MSG_PART_ONLY, t->msglen, t->msg,
				        &sha256_ctx, hash);
			hashlen = SHA256_HASH_LENGTH;
			break;
		case SHA384:
			rc = ica_sha384(SHA_MSG_PART_ONLY, t->msglen, t->msg,
				        &sha512_ctx, hash);
			hashlen = SHA384_HASH_LENGTH;
			break;
		case SHA512:
			rc = ica_sha512(SHA_MSG_PART_ONLY, t->msglen, t->msg,
				        &sha512_ctx, hash);
			hashlen = SHA512_HASH_LENGTH;
			break;
		default:
			TEST_ERROR("Unknown hash", "ECDSA", i);
		}

		if (rc)
			TEST_ERROR("Hashing failed", "ECDSA", i);

		deterministic_rng_output = t->k;

		/* Sign hashed message */

		rc = ecdsa_sign_cpacf(t->key, hash, hashlen, sig,
				      deterministic_rng);
		if (rc)
			TEST_ERROR("Signing failed", "ECDSA", i);

		/* Compare signature to expected result */

		if (memcmp(sig, t->r, t->siglen)
		    || memcmp(sig + t->siglen, t->s, t->siglen)) {
			printf("Result R:\n");
			dump_array(sig, t->siglen);
			printf("Correct R:\n");
			dump_array((unsigned char *)t->r, t->siglen);
			printf("Result S:\n");
			dump_array(sig + t->siglen, t->siglen);
			printf("Correct S:\n");
			dump_array((unsigned char *)t->s, t->siglen);
			TEST_ERROR("Wrong signature", "ECDSA", i);
		}

		/* Verify signature */

		rc = ecdsa_verify_cpacf(t->key, hash, hashlen, sig);
		if (rc)
			TEST_ERROR("Verification failed", "ECDSA", i);

		/*
		 * Try to verify forged signature
		 * (flip random bit in signature)
		 */

		rng_gen((unsigned char *)rnd, sizeof(rnd));
		sig[rnd[0] % (t->siglen * 2)] ^= (1 << (rnd[1] % 8));

		rc = ecdsa_verify_cpacf(t->key, hash, hashlen, sig);
		if (!rc)
			TEST_ERROR("Verification expected to fail but"
				   " succeeded", "ECDSA", i);

		t++;
	}
}

static void scalar_mul_test(void)
{
	const unsigned char *base_x, *base_y, *base_u;
	unsigned char res_x[4096], res_y[4096], res_u[4096], res_u2[4096],
		      res_u3[4096];
	const struct scalar_mul_tv *t;
	const struct scalar_mulx_tv *t2;
	const struct scalar_mulx_it_tv *t3;
	const struct scalar_mulx_kex_tv *t4;
	size_t i, j;
	int rc;

	static const unsigned char p256_base_x[] = {
	0x6B, 0x17, 0xD1, 0xF2, 0xE1, 0x2C, 0x42, 0x47, 0xF8, 0xBC, 0xE6, 0xE5,
	0x63, 0xA4, 0x40, 0xF2, 0x77, 0x03, 0x7D, 0x81, 0x2D, 0xEB, 0x33, 0xA0,
	0xF4, 0xA1, 0x39, 0x45, 0xD8, 0x98, 0xC2, 0x96
	};
	static const unsigned char p256_base_y[] = {
	0x4F, 0xE3, 0x42, 0xE2, 0xFE, 0x1A, 0x7F, 0x9B, 0x8E, 0xE7, 0xEB, 0x4A,
	0x7C, 0x0F, 0x9E, 0x16, 0x2B, 0xCE, 0x33, 0x57, 0x6B, 0x31, 0x5E, 0xCE,
	0xCB, 0xB6, 0x40, 0x68, 0x37, 0xBF, 0x51, 0xF5
	};

	static const unsigned char p384_base_x[] = {
	0xAA, 0x87, 0xCA, 0x22, 0xBE, 0x8B, 0x05, 0x37, 0x8E, 0xB1, 0xC7, 0x1E,
	0xF3, 0x20, 0xAD, 0x74, 0x6E, 0x1D, 0x3B, 0x62, 0x8B, 0xA7, 0x9B, 0x98,
	0x59, 0xF7, 0x41, 0xE0, 0x82, 0x54, 0x2A, 0x38, 0x55, 0x02, 0xF2, 0x5D,
	0xBF, 0x55, 0x29, 0x6C, 0x3A, 0x54, 0x5E, 0x38, 0x72, 0x76, 0x0A, 0xB7
	};
	static const unsigned char p384_base_y[] = {
	0x36, 0x17, 0xDE, 0x4A, 0x96, 0x26, 0x2C, 0x6F, 0x5D, 0x9E, 0x98, 0xBF,
	0x92, 0x92, 0xDC, 0x29, 0xF8, 0xF4, 0x1D, 0xBD, 0x28, 0x9A, 0x14, 0x7C,
	0xE9, 0xDA, 0x31, 0x13, 0xB5, 0xF0, 0xB8, 0xC0, 0x0A, 0x60, 0xB1, 0xCE,
	0x1D, 0x7E, 0x81, 0x9D, 0x7A, 0x43, 0x1D, 0x7C, 0x90, 0xEA, 0x0E, 0x5F
	};

	static const unsigned char p521_base_x[] = {
	0x00, 0xC6, 0x85, 0x8E, 0x06, 0xB7, 0x04, 0x04, 0xE9, 0xCD, 0x9E, 0x3E,
	0xCB, 0x66, 0x23, 0x95, 0xB4, 0x42, 0x9C, 0x64, 0x81, 0x39, 0x05, 0x3F,
	0xB5, 0x21, 0xF8, 0x28, 0xAF, 0x60, 0x6B, 0x4D, 0x3D, 0xBA, 0xA1, 0x4B,
	0x5E, 0x77, 0xEF, 0xE7, 0x59, 0x28, 0xFE, 0x1D, 0xC1, 0x27, 0xA2, 0xFF,
	0xA8, 0xDE, 0x33, 0x48, 0xB3, 0xC1, 0x85, 0x6A, 0x42, 0x9B, 0xF9, 0x7E,
	0x7E, 0x31, 0xC2, 0xE5, 0xBD, 0x66
	};
	static const unsigned char p521_base_y[] = {
	0x01, 0x18, 0x39, 0x29, 0x6A, 0x78, 0x9A, 0x3B, 0xC0, 0x04, 0x5C, 0x8A,
	0x5F, 0xB4, 0x2C, 0x7D, 0x1B, 0xD9, 0x98, 0xF5, 0x44, 0x49, 0x57, 0x9B,
	0x44, 0x68, 0x17, 0xAF, 0xBD, 0x17, 0x27, 0x3E, 0x66, 0x2C, 0x97, 0xEE,
	0x72, 0x99, 0x5E, 0xF4, 0x26, 0x40, 0xC5, 0x50, 0xB9, 0x01, 0x3F, 0xAD,
	0x07, 0x61, 0x35, 0x3C, 0x70, 0x86, 0xA2, 0x72, 0xC2, 0x40, 0x88, 0xBE,
	0x94, 0x76, 0x9F, 0xD1, 0x66, 0x50
	};
	static const unsigned char x25519_base_u[] = {
	0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	static const unsigned char x448_base_u[] = {
	0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	verbosity_ = 2;

	t = &SCALAR_MUL_TV[0];

	for (i = 0; i < SCALAR_MUL_TV_LEN; i++) {
		memset(res_x, 0, sizeof(res_x));
		memset(res_y, 0, sizeof(res_y));

		switch (t->curve_nid) {
		case NID_X9_62_prime256v1:
			base_x = p256_base_x;
			base_y = p256_base_y;
			break;
		case NID_secp384r1:
			base_x = p384_base_x;
			base_y = p384_base_y;
			break;
		case NID_secp521r1:
			base_x = p521_base_x;
			base_y = p521_base_y;
			break;
		default:
			TEST_ERROR("Unknown curve", "SCALAR-MUL", i);
		}

		rc = scalar_mul_cpacf(res_x, res_y, t->scalar, base_x, base_y,
				      t->curve_nid);
		if (rc) {
			TEST_ERROR("Scalar multipication failed",
				   "SCALAR-MUL", i);
		}

		if (memcmp(res_x, t->x, t->len)) {
			printf("Result X:\n");
			dump_array(res_x, t->len);
			printf("Correct X:\n");
			dump_array((unsigned char *)t->x, t->len);
			TEST_ERROR("Scalar multipication calculated wrong X",
				   "SCALAR-MUL", i);
		}

		if (memcmp(res_y, t->y, t->len)) {
			printf("Result Y:\n");
			dump_array(res_y, t->len);
			printf("Correct Y:\n");
			dump_array((unsigned char *)t->y, t->len);
			TEST_ERROR("Scalar multipication calculated wrong X",
				   "SCALAR-MUL", i);
		}

		t++;
	}

	t2 = &SCALAR_MULX_TV[0];

	for (i = 0; i < SCALAR_MULX_TV_LEN; i++) {
		memset(res_u, 0, sizeof(res_u));

		rc = scalar_mulx_cpacf(res_u, t2->scalar, t2->u,
				       t2->curve_nid);
		if (rc) {
			TEST_ERROR("Scalar multipication failed",
				   "SCALAR-MULX", i);
		}

		if (memcmp(res_u, t2->res_u, t2->len)) {
			printf("Result U:\n");
			dump_array(res_u, t2->len);
			printf("Correct U:\n");
			dump_array((unsigned char *)t2->res_u, t2->len);
			TEST_ERROR("Scalar multipication calculated wrong U",
				   "SCALAR-MULX", i);
		}

		t2++;
	}

	t3 = &SCALAR_MULX_IT_TV[0];

	for (i = 0; i < SCALAR_MULX_IT_TV_LEN; i++) {
		memset(res_u, 0, sizeof(res_u));
		memset(res_u2, 0, sizeof(res_u2));
		memset(res_u3, 0, sizeof(res_u3));
		memcpy(res_u, t3->scalar_u, t3->len);
		memcpy(res_u2, t3->scalar_u, t3->len);

		for (j = 1; j <= 1000000; j++) {
			rc = scalar_mulx_cpacf(res_u3, res_u2, res_u,
					       t3->curve_nid);
			if (rc) {
				TEST_ERROR("Scalar multipication failed",
					   "SCALAR-MULX-IT-MUL", i);
			}

			if (j == 1 && memcmp(res_u3, t3->res_u_it1, t3->len)) {
				printf("Result U:\n");
				dump_array(res_u3, t3->len);
				printf("Correct U:\n");
				dump_array((unsigned char *)t3->res_u_it1,
					   t3->len);
				TEST_ERROR("Scalar multipication calculated"
					   " wrong U", "SCALAR-MULX-IT-MUL",
					   i);
			}
			if (j == 1000 && memcmp(res_u3, t3->res_u_it1000,
			    t3->len)) {
				printf("Result U:\n");
				dump_array(res_u3, t3->len);
				printf("Correct U:\n");
				dump_array((unsigned char *)t3->res_u_it1000,
					   t3->len);
				TEST_ERROR("Scalar multipication calculated"
					   " wrong U", "SCALAR-MULX-IT-MUL",
					   i);
			}
			if (j == 1000000 && memcmp(res_u3, t3->res_u_it1000000,
			    t3->len)) {
				printf("Result U:\n");
				dump_array(res_u3, t3->len);
				printf("Correct U:\n");
				dump_array((unsigned char *)
					   t3->res_u_it1000000, t3->len);
				TEST_ERROR("Scalar multipication calculated"
					   " wrong U", "SCALAR-MULX-IT-MUL",
					   i);
			}

			memcpy(res_u, res_u2, sizeof(res_u));
			memcpy(res_u2, res_u3, sizeof(res_u2));
			memset(res_u3, 0, sizeof(res_u3));
		}

		t3++;
	}

	t4 = &SCALAR_MULX_KEX_TV[0];

	for (i = 0; i < SCALAR_MULX_KEX_TV_LEN; i++) {
		switch (t4->curve_nid) {
		case NID_X25519:
			base_u = x25519_base_u;
			break;
		case NID_X448:
			base_u = x448_base_u;
			break;
		default:
			TEST_ERROR("Unknown curve", "SCALAR-MULX-KEX", i);
		}

		memset(res_u, 0, sizeof(res_u));

		rc = scalar_mulx_cpacf(res_u, t4->a_priv, base_u,
				       t4->curve_nid);
		if (rc) {
			TEST_ERROR("Scalar multipication failed",
				   "SCALAR-MULX-KEX", i);
		}

		if (memcmp(res_u, t4->a_pub, t4->len)) {
			printf("Result A's pub:\n");
			dump_array(res_u, t4->len);
			printf("Correct A's pub:\n");
			dump_array((unsigned char *)t4->a_pub, t4->len);
			TEST_ERROR("Wrong public key (A)",
				   "SCALAR-MULX-KEX", i);
		}

		memset(res_u, 0, sizeof(res_u));

		rc = scalar_mulx_cpacf(res_u, t4->b_priv, base_u,
				       t4->curve_nid);
		if (rc) {
			TEST_ERROR("Scalar multipication failed",
				   "SCALARX-KEX", i);
		}

		if (memcmp(res_u, t4->b_pub, t4->len)) {
			printf("Result B's pub:\n");
			dump_array(res_u, t4->len);
			printf("Correct B's pub:\n");
			dump_array((unsigned char *)t4->b_pub, t4->len);
			TEST_ERROR("Wrong public key (B)",
				   "SCALAR-MULX-KEX", i);
		}

		memset(res_u, 0, sizeof(res_u));

		rc = scalar_mulx_cpacf(res_u, t4->b_priv, t4->a_pub,
				       t4->curve_nid);
		if (rc) {
			TEST_ERROR("Scalar multipication failed",
				   "SCALARX-KEX", i);
		}

		if (memcmp(res_u, t4->shared_secret, t4->len)) {
			printf("Result shared secret:\n");
			dump_array(res_u, t4->len);
			printf("Correct shared secret:\n");
			dump_array((unsigned char *)t4->shared_secret,
				   t4->len);
			TEST_ERROR("Wrong shared secret (B's priv * A's pub)",
				   "SCALAR-MULX-KEX", i);
		}

		memset(res_u, 0, sizeof(res_u));

		rc = scalar_mulx_cpacf(res_u, t4->a_priv, t4->b_pub,
				       t4->curve_nid);
		if (rc) {
			TEST_ERROR("Scalar multipication failed",
				   "SCALARX-KEX", i);
		}

		if (memcmp(res_u, t4->shared_secret, t4->len)) {
			printf("Result shared secret:\n");
			dump_array(res_u, t4->len);
			printf("Correct shared secret:\n");
			dump_array((unsigned char *)t4->shared_secret,
				   t4->len);
			TEST_ERROR("Wrong shared secret (A's priv * B's pub)",
				   "SCALAR-MULX-KEX", i);
		}

		t4++;
	}
}

int main(void)
{
	if (!msa9_switch)
		exit(TEST_SKIP);

	/* test exit on first failure */
	scalar_mul_test();
	ecdsa_test();

	return TEST_SUCC;
}

#endif
