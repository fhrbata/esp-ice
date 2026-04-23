/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/crt-bundle/crt-bundle.c
 * @brief `ice idf crt-bundle` -- native x509 certificate bundle generator.
 *
 * Drop-in replacement for ESP-IDF's @b{gen_crt_bundle.py}, which pulls in
 * the python @c cryptography package purely for DER byte extraction.
 * This tool does the same structural work in pure C with no TLS / crypto
 * dependency: PEM-decode each cert, walk the X.509 ASN.1 structure
 * shallowly, and copy the raw @c subject @c Name and
 * @c subjectPublicKeyInfo DER bytes straight into the output bundle.
 *
 * Output format (consumed by
 * @c esp-idf/components/mbedtls/esp_crt_bundle/esp_crt_bundle.c at
 * runtime):
 *
 *   +----------------------+
 *   | offsets[N]           |  N x uint32_le, byte offsets from the
 *   |                      |  bundle start to each record
 *   +----------------------+
 *   | record 0             |
 *   | record 1             |
 *   | ...                  |
 *   +----------------------+
 *
 * Each record is:
 *
 *   uint16_le subject_len
 *   uint16_le spki_len
 *   subject_len bytes   -- raw X.509 subject Name SEQUENCE DER
 *   spki_len bytes      -- raw SubjectPublicKeyInfo SEQUENCE DER
 *
 * Certificates are sorted by subject DER so the runtime can binary-search
 * the table.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ice.h"

/* clang-format off */
static const struct cmd_manual idf_crt_bundle_manual = {
	.name = "ice idf crt-bundle",
	.summary = "generate an x509 certificate bundle from PEM / DER inputs",

	.description =
	H_PARA("Drop-in replacement for @b{gen_crt_bundle.py}.  Takes one "
	       "or more @b{--input} paths (PEM/DER files or directories "
	       "containing such files) and writes @b{x509_crt_bundle} in "
	       "the current directory -- the format ESP-IDF's "
	       "@b{esp_crt_bundle} component reads at runtime.")
	H_PARA("Works by structural DER extraction (no cryptographic "
	       "validation): for each cert, copies the @b{Name} SEQUENCE "
	       "and @b{SubjectPublicKeyInfo} SEQUENCE bytes unchanged.  "
	       "Output is byte-identical to the python implementation for "
	       "the Mozilla CA bundle IDF ships.")
	H_PARA("@b{--filter} applies only to an input named "
	       "@b{cacrt_all.pem}: the CSV's second column is a set of "
	       "cert display names, and only matching certs in that file "
	       "are kept.  Other input files are taken whole."),

	.examples =
	H_EXAMPLE("ice idf crt-bundle -i cacrt_all.pem -i cacrt_local.pem -q")
	H_EXAMPLE("ice idf crt-bundle -i my_certs/ --max-certs 50")
	H_EXAMPLE("ice idf crt-bundle -i cacrt_all.pem -f filter.csv"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice build",
	       "Invokes this command automatically when the active "
	       "profile enables @b{CONFIG_MBEDTLS_CERTIFICATE_BUNDLE}."),
};
/* clang-format on */

#define DEFAULT_MAX_CERTS 200

static struct svec opt_inputs = SVEC_INIT;
static const char *opt_filter;
static int opt_max_certs = DEFAULT_MAX_CERTS;
static int opt_quiet;

static const struct option cmd_idf_crt_bundle_opts[] = {
    OPT_STRING_LIST('i', "input", &opt_inputs, "path",
		    "PEM/DER cert file or directory (repeatable)", NULL),
    OPT_STRING('f', "filter", &opt_filter, "path",
	       "CSV filter for cacrt_all.pem (2nd column = cert name)", NULL),
    OPT_INT('m', "max-certs", &opt_max_certs, "n",
	    "fail if bundle would exceed this many certs (default 200)", NULL),
    OPT_BOOL('q', "quiet", &opt_quiet, "suppress informational output"),
    OPT_END(),
};

int cmd_idf_crt_bundle(int argc, const char **argv);

const struct cmd_desc cmd_idf_crt_bundle_desc = {
    .name = "crt-bundle",
    .fn = cmd_idf_crt_bundle,
    .opts = cmd_idf_crt_bundle_opts,
    .manual = &idf_crt_bundle_manual,
};

/* ------------------------------------------------------------------ */
/* Status messages -- mirror python's stderr prefixes so --quiet means */
/* the same thing across the two implementations.                     */
/* ------------------------------------------------------------------ */

static void status(const char *fmt, ...)
{
	va_list ap;

	if (opt_quiet)
		return;
	fputs("gen_crt_bundle.py: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ------------------------------------------------------------------ */
/* Base64 decoder                                                      */
/* ------------------------------------------------------------------ */

/*
 * Decode base64 @p src into @p out, tolerating whitespace (the PEM
 * standard inserts a newline every 64 characters).  Stops at the first
 * '=' padding byte -- the contract with the caller is that @p src is
 * already the body of a single PEM block, so padding comes only at the
 * very end.  Returns 0 on success, -1 on any non-base64, non-whitespace
 * byte.
 */
static int b64_decode(const char *src, size_t len, struct sbuf *out)
{
	static int8_t tab[256];
	static int init;

	if (!init) {
		memset(tab, -1, sizeof(tab));
		for (int i = 0; i < 26; i++) {
			tab['A' + i] = (int8_t)i;
			tab['a' + i] = (int8_t)(26 + i);
		}
		for (int i = 0; i < 10; i++)
			tab['0' + i] = (int8_t)(52 + i);
		tab[(unsigned char)'+'] = 62;
		tab[(unsigned char)'/'] = 63;
		init = 1;
	}

	uint32_t buf = 0;
	int bits = 0;

	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)src[i];
		if (c == '=')
			break;
		if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
			continue;
		int8_t v = tab[c];
		if (v < 0)
			return -1;
		buf = (buf << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			sbuf_addch(out, (char)((buf >> bits) & 0xFF));
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* ASN.1 DER shallow walker                                            */
/* ------------------------------------------------------------------ */

/*
 * Parse one TLV at @p *p.  Advances @p *p past the whole TLV (T+L+V).
 * Returns 0 on success, -1 on malformed encoding: truncated read,
 * multi-byte length whose byte count exceeds 4, or indefinite length
 * (illegal under DER; legal under BER but not produced by X.509
 * certificates in practice).
 */
static int asn1_tlv(const uint8_t **p, const uint8_t *end, uint8_t *tag_out,
		    const uint8_t **val_out, size_t *val_len_out)
{
	if (*p >= end)
		return -1;
	uint8_t tag = *(*p)++;
	if (*p >= end)
		return -1;
	uint8_t b = *(*p)++;
	size_t len;

	if (b < 0x80) {
		len = b;
	} else {
		int n = b & 0x7F;
		if (n == 0 || n > 4)
			return -1;
		if (*p + n > end)
			return -1;
		len = 0;
		for (int i = 0; i < n; i++)
			len = (len << 8) | *(*p)++;
	}
	if (*p + len > end)
		return -1;
	*tag_out = tag;
	*val_out = *p;
	*val_len_out = len;
	*p += len;
	return 0;
}

/*
 * Given DER bytes of an X.509 @c Certificate, populate @p *subject /
 * @p *spki with pointers+lengths of the subject @c Name SEQUENCE and
 * @c subjectPublicKeyInfo SEQUENCE -- the FULL TLVs (tag + length +
 * contents), which is what the runtime binary-search / key-load code
 * expects.
 *
 * Structure (RFC 5280):
 *
 *   Certificate ::= SEQUENCE {
 *     tbsCertificate       TBSCertificate,
 *     signatureAlgorithm   AlgorithmIdentifier,
 *     signatureValue       BIT STRING
 *   }
 *
 *   TBSCertificate ::= SEQUENCE {
 *     version         [0] EXPLICIT Version DEFAULT v1,
 *     serialNumber         CertificateSerialNumber,
 *     signature            AlgorithmIdentifier,
 *     issuer               Name,
 *     validity             Validity,
 *     subject              Name,                  -- we want this
 *     subjectPublicKeyInfo SubjectPublicKeyInfo,  -- ... and this
 *     ...                  -- optional: issuerUID, subjectUID, extensions
 *   }
 */
static int cert_pick(const uint8_t *der, size_t der_len,
		     const uint8_t **subject, size_t *subject_len,
		     const uint8_t **spki, size_t *spki_len)
{
	const uint8_t *p = der;
	const uint8_t *end = der + der_len;
	uint8_t tag;
	const uint8_t *val;
	size_t val_len;

	/* Outer Certificate SEQUENCE */
	if (asn1_tlv(&p, end, &tag, &val, &val_len) < 0 || tag != 0x30)
		return -1;
	const uint8_t *tbs_p = val;
	const uint8_t *tbs_end = val + val_len;

	/* TBSCertificate SEQUENCE */
	if (asn1_tlv(&tbs_p, tbs_end, &tag, &val, &val_len) < 0 || tag != 0x30)
		return -1;
	const uint8_t *fields = val;
	const uint8_t *fields_end = val + val_len;

	/* Optional [0] EXPLICIT version -- tag 0xA0 */
	if (fields < fields_end && *fields == 0xA0) {
		if (asn1_tlv(&fields, fields_end, &tag, &val, &val_len) < 0)
			return -1;
	}
	/* serialNumber, signature, issuer, validity -- skipped */
	for (int i = 0; i < 4; i++) {
		if (asn1_tlv(&fields, fields_end, &tag, &val, &val_len) < 0)
			return -1;
	}

	/* subject Name -- capture full TLV */
	{
		const uint8_t *start = fields;
		if (asn1_tlv(&fields, fields_end, &tag, &val, &val_len) < 0)
			return -1;
		*subject = start;
		*subject_len = (size_t)(fields - start);
	}

	/* subjectPublicKeyInfo -- capture full TLV */
	{
		const uint8_t *start = fields;
		if (asn1_tlv(&fields, fields_end, &tag, &val, &val_len) < 0)
			return -1;
		*spki = start;
		*spki_len = (size_t)(fields - start);
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Cert list                                                           */
/* ------------------------------------------------------------------ */

/*
 * Owned cert record.  @p der is heap-allocated (and stable across list
 * growth) so that @p subject / @p spki, which point into @p der, stay
 * valid as the list is reallocated.
 */
struct cert_rec {
	uint8_t *der;
	size_t der_len;
	const uint8_t *subject;
	size_t subject_len;
	const uint8_t *spki;
	size_t spki_len;
};

struct cert_list {
	struct cert_rec *v;
	size_t nr;
	size_t alloc;
};

#define CERT_LIST_INIT {NULL, 0, 0}

static void cert_list_release(struct cert_list *list)
{
	for (size_t i = 0; i < list->nr; i++)
		free(list->v[i].der);
	free(list->v);
	list->v = NULL;
	list->nr = list->alloc = 0;
}

/*
 * Take ownership of a DER buffer, walk it for subject/SPKI, and append
 * a record.  On malformed DER, frees @p der and warns (matching python
 * which raises InputError but the caller chooses whether to abort).
 */
static int cert_list_add_der(struct cert_list *list, uint8_t *der, size_t len,
			     const char *source_label)
{
	const uint8_t *subj, *spki;
	size_t subj_len, spki_len;

	if (cert_pick(der, len, &subj, &subj_len, &spki, &spki_len) < 0) {
		err("invalid certificate in %s", source_label);
		free(der);
		return -1;
	}
	ALLOC_GROW(list->v, list->nr + 1, list->alloc);
	list->v[list->nr].der = der;
	list->v[list->nr].der_len = len;
	list->v[list->nr].subject = subj;
	list->v[list->nr].subject_len = subj_len;
	list->v[list->nr].spki = spki;
	list->v[list->nr].spki_len = spki_len;
	list->nr++;
	return 0;
}

/* ------------------------------------------------------------------ */
/* PEM block iteration                                                 */
/* ------------------------------------------------------------------ */

static const void *mem_find(const void *hay, size_t hay_len, const void *needle,
			    size_t needle_len)
{
	if (needle_len == 0)
		return hay;
	if (hay_len < needle_len)
		return NULL;
	const unsigned char *h = hay;
	for (size_t i = 0; i <= hay_len - needle_len; i++)
		if (!memcmp(h + i, needle, needle_len))
			return h + i;
	return NULL;
}

/*
 * Scan @p *p..@p end for the next @c{-----BEGIN CERTIFICATE-----} /
 * @c{-----END CERTIFICATE-----} pair.  On success fills
 * @p body_out / @p body_end_out with the base64 body (excluding the
 * markers themselves), advances @p *p past the END marker, and returns
 * 1.  Returns 0 at EOF / no further block.
 */
static int pem_next_cert(const char **p, const char *end, const char **body_out,
			 const char **body_end_out)
{
	static const char BEG[] = "-----BEGIN CERTIFICATE-----";
	static const char END[] = "-----END CERTIFICATE-----";
	static const size_t BEG_LEN = sizeof(BEG) - 1;
	static const size_t END_LEN = sizeof(END) - 1;

	const char *beg = mem_find(*p, (size_t)(end - *p), BEG, BEG_LEN);
	if (!beg)
		return 0;
	const char *body = beg + BEG_LEN;
	const char *e = mem_find(body, (size_t)(end - body), END, END_LEN);
	if (!e)
		return 0;

	*body_out = body;
	*body_end_out = e;
	*p = e + END_LEN;
	return 1;
}

/*
 * Parse every @c{-----BEGIN CERTIFICATE-----} block in @p pem and add
 * each decoded cert to @p list.  @p source_label is used in diagnostics
 * only.
 */
static void add_from_pem(struct cert_list *list, const char *pem, size_t len,
			 const char *source_label)
{
	const char *p = pem;
	const char *end = pem + len;
	const char *body, *body_end;
	int count = 0;

	while (pem_next_cert(&p, end, &body, &body_end)) {
		struct sbuf der = SBUF_INIT;
		size_t der_len;
		uint8_t *buf;

		if (b64_decode(body, (size_t)(body_end - body), &der) < 0) {
			sbuf_release(&der);
			err("invalid base64 in certificate (%s)", source_label);
			continue;
		}
		der_len = der.len;
		buf = (uint8_t *)sbuf_detach(&der);
		if (cert_list_add_der(list, buf, der_len, source_label) == 0)
			count++;
	}
	if (count == 0)
		status("No certificate found");
	else
		status("Successfully added %d certificates", count);
}

static void add_from_der(struct cert_list *list, const void *bytes, size_t len,
			 const char *source_label)
{
	uint8_t *buf = malloc(len);

	if (!buf)
		die_errno("malloc(%zu)", len);
	memcpy(buf, bytes, len);
	if (cert_list_add_der(list, buf, len, source_label) == 0)
		status("Successfully added 1 certificate");
}

/*
 * Case-insensitive match for the trailing 4 characters of @p s against
 * lowercase @p ext ("." plus three letters).  @p ext must itself be
 * lowercase.  Returns 1 if @p s ends with the extension.
 */
static int has_ext_icase(const char *s, const char *ext)
{
	size_t n = strlen(s);
	if (n < 4)
		return 0;
	const char *p = s + n - 4;
	for (int i = 0; i < 4; i++) {
		char a = p[i];
		if (a >= 'A' && a <= 'Z')
			a = (char)(a - 'A' + 'a');
		if (a != ext[i])
			return 0;
	}
	return 1;
}

/*
 * Add every cert in a single file.  Extension drives the parser:
 * @c .pem -> PEM, @c .der -> DER (both case-insensitive).  Other
 * extensions are silently skipped (matches python's
 * enumeration-filter behaviour).
 */
static int add_from_file(struct cert_list *list, const char *path)
{
	int is_pem = has_ext_icase(path, ".pem");
	int is_der = has_ext_icase(path, ".der");

	if (!is_pem && !is_der)
		return 0;

	struct sbuf content = SBUF_INIT;
	if (sbuf_read_file(&content, path) < 0) {
		err_errno("cannot read '%s'", path);
		sbuf_release(&content);
		return -1;
	}
	status("Parsing certificates from %s", path);
	if (is_pem)
		add_from_pem(list, content.buf, content.len, path);
	else
		add_from_der(list, content.buf, content.len, path);
	sbuf_release(&content);
	return 1;
}

struct dir_ctx {
	struct cert_list *list;
	const char *dir;
	int found;
};

static int dir_cb(const char *name, void *ud)
{
	struct dir_ctx *ctx = ud;
	struct sbuf full = SBUF_INIT;

	sbuf_addf(&full, "%s/%s", ctx->dir, name);
	if (add_from_file(ctx->list, full.buf) > 0)
		ctx->found = 1;
	sbuf_release(&full);
	return 0;
}

static void add_from_path(struct cert_list *list, const char *path)
{
	if (is_directory(path)) {
		struct dir_ctx ctx = {list, path, 0};
		if (dir_foreach(path, dir_cb, &ctx) < 0)
			die_errno("cannot read directory '%s'", path);
		if (!ctx.found)
			die("no valid x509 certificates found in %s", path);
	} else {
		if (add_from_file(list, path) <= 0)
			die("cannot parse '%s'", path);
	}
}

/* ------------------------------------------------------------------ */
/* --filter: keep only named certs from cacrt_all.pem                  */
/* ------------------------------------------------------------------ */

/*
 * Parse the filter CSV: skip header, then one name per row at column
 * index 1 (second column, 0-based).  Stores names in @p out.  The
 * field storage comes from @p csv_buf which the caller owns.
 *
 * Keep parsing minimal -- this mirrors the python csv.reader invocation
 * in gen_crt_bundle.py which uses the default dialect: commas, no
 * escapes, no quoted-field awareness beyond a blind split.  Good enough
 * for the Mozilla filter files IDF ships.
 */
static void filter_load(struct svec *out, char *csv_buf, size_t len)
{
	char *p = csv_buf;
	char *end = csv_buf + len;
	int line = 0;

	while (p < end) {
		char *nl = memchr(p, '\n', (size_t)(end - p));
		if (!nl)
			nl = end;
		if (line++ == 0) { /* skip header */
			p = nl < end ? nl + 1 : end;
			continue;
		}

		/* Walk to the second column. */
		char *col2 = memchr(p, ',', (size_t)(nl - p));
		if (col2) {
			col2++; /* past the comma */
			char *col2_end = memchr(col2, ',', (size_t)(nl - col2));
			if (!col2_end)
				col2_end = nl;
			/* Trim trailing CR. */
			while (col2_end > col2 && col2_end[-1] == '\r')
				col2_end--;
			if (col2_end > col2) {
				*col2_end = '\0';
				svec_push(out, col2);
			}
		}

		p = nl < end ? nl + 1 : end;
	}
}

/*
 * Apply @p filter_set to the PEM text @p pem in-place: scan every
 * cert block with its preceding name line (the convention in
 * @c{cacrt_all.pem} is
 *
 *     Common Name\n
 *     ==============\n
 *     -----BEGIN CERTIFICATE-----\n...
 *
 * ), keep only blocks whose name is in the set.  Returns a freshly
 * allocated filtered PEM text; caller frees.
 */
static char *apply_filter(const char *pem, size_t pem_len,
			  const struct svec *filter_set, size_t *out_len)
{
	struct sbuf out = SBUF_INIT;
	const char *p = pem;
	const char *end = pem + pem_len;

	while (p < end) {
		/* Locate the next BEGIN. */
		static const char BEG[] = "-----BEGIN CERTIFICATE-----";
		static const size_t BEG_LEN = sizeof(BEG) - 1;
		const char *beg = mem_find(p, (size_t)(end - p), BEG, BEG_LEN);
		if (!beg)
			break;

		/* The preceding two lines should be "Name\n=====\n".
		 * Walk back from BEG over the equals line and then over
		 * the name line. */
		const char *name_end = beg - 1; /* before the \n above BEG */
		/* Step back past the equals line. */
		if (name_end > p && name_end[0] == '\n')
			name_end--;
		while (name_end > p && *name_end == '=')
			name_end--;
		/* Now name_end sits on the \n above the equals line. */
		if (name_end > p && *name_end == '\n')
			name_end--;
		/* Walk back to the newline above the name. */
		const char *name_start = name_end;
		while (name_start > p && *name_start != '\n')
			name_start--;
		if (name_start < name_end && *name_start == '\n')
			name_start++;

		/* Check membership. */
		size_t name_len = (size_t)(name_end - name_start + 1);
		int matched = 0;
		for (size_t i = 0; i < filter_set->nr; i++) {
			size_t flen = strlen(filter_set->v[i]);
			if (flen == name_len &&
			    !memcmp(filter_set->v[i], name_start, name_len)) {
				matched = 1;
				break;
			}
		}

		/* Find END marker to delimit the block. */
		static const char END[] = "-----END CERTIFICATE-----";
		static const size_t END_LEN = sizeof(END) - 1;
		const char *e =
		    mem_find(beg, (size_t)(end - beg), END, END_LEN);
		if (!e)
			break;
		const char *block_end = e + END_LEN;
		if (block_end < end && *block_end == '\n')
			block_end++;

		if (matched)
			sbuf_add(&out, beg, (size_t)(block_end - beg));

		p = block_end;
	}

	*out_len = out.len;
	return sbuf_detach(&out);
}

/* ------------------------------------------------------------------ */
/* Bundle pack + write                                                 */
/* ------------------------------------------------------------------ */

static int cmp_by_subject(const void *a, const void *b)
{
	const struct cert_rec *ca = a;
	const struct cert_rec *cb = b;
	size_t n = ca->subject_len < cb->subject_len ? ca->subject_len
						     : cb->subject_len;
	int r = memcmp(ca->subject, cb->subject, n);
	if (r != 0)
		return r;
	if (ca->subject_len < cb->subject_len)
		return -1;
	if (ca->subject_len > cb->subject_len)
		return 1;
	return 0;
}

static void put_u16_le(struct sbuf *sb, uint16_t v)
{
	uint8_t b[2] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF)};
	sbuf_add(sb, b, 2);
}

static void put_u32_le(struct sbuf *sb, uint32_t v)
{
	uint8_t b[4] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
			(uint8_t)((v >> 16) & 0xFF),
			(uint8_t)((v >> 24) & 0xFF)};
	sbuf_add(sb, b, 4);
}

/*
 * Serialise @p list into @p out in the format esp_crt_bundle.c expects.
 * Sorts @p list in place by subject DER (stable for our purposes: we
 * never need the original order).
 */
static void bundle_pack(struct cert_list *list, struct sbuf *out)
{
	/* qsort(NULL, 0, ...) is undefined; bail early on an empty list. */
	if (list->nr == 0)
		return;
	qsort(list->v, list->nr, sizeof(*list->v), cmp_by_subject);

	/* Offset of record k from the start of the bundle. */
	uint32_t off = (uint32_t)(list->nr * sizeof(uint32_t));
	for (size_t i = 0; i < list->nr; i++) {
		put_u32_le(out, off);
		off += 4 + (uint32_t)list->v[i].subject_len +
		       (uint32_t)list->v[i].spki_len;
	}
	for (size_t i = 0; i < list->nr; i++) {
		put_u16_le(out, (uint16_t)list->v[i].subject_len);
		put_u16_le(out, (uint16_t)list->v[i].spki_len);
		sbuf_add(out, list->v[i].subject, list->v[i].subject_len);
		sbuf_add(out, list->v[i].spki, list->v[i].spki_len);
	}
}

/* ------------------------------------------------------------------ */
/* Command entry                                                       */
/* ------------------------------------------------------------------ */

static const char BUNDLE_FILENAME[] = "x509_crt_bundle";

/*
 * ESP-IDF's @c components/mbedtls/CMakeLists.txt expands its crt_paths
 * list into a single argparse @c{nargs='+'} invocation:
 *
 *     --input A B C -q --max-certs 200
 *
 * Our options framework is one-value-per-flag, so we pre-extract the
 * @c --input flag and all its following values (up to the next token
 * that starts with '-', since IDF always hands us absolute paths)
 * directly into @p out, then compact argv so the remaining options
 * parse normally.  The @c OPT_STRING_LIST entry in the option table
 * is kept for @c --help rendering, but parse_options will never see
 * @c --input after this pre-pass.
 */
static void extract_input_nargs(int *argc_inout, const char **argv,
				struct svec *out)
{
	int argc = *argc_inout;
	int dst = 0;
	int src = 0;

	while (src < argc) {
		const char *a = argv[src];

		if (!strcmp(a, "--input") || !strcmp(a, "-i")) {
			src++;
			while (src < argc && argv[src][0] != '-') {
				svec_push(out, argv[src]);
				src++;
			}
		} else {
			argv[dst++] = argv[src++];
		}
	}
	*argc_inout = dst;
}

int cmd_idf_crt_bundle(int argc, const char **argv)
{
	struct cert_list list = CERT_LIST_INIT;
	struct sbuf bundle = SBUF_INIT;
	struct sbuf filter_buf = SBUF_INIT;
	struct svec filter_set = SVEC_INIT;
	int rc = 0;

	extract_input_nargs(&argc, argv, &opt_inputs);
	argc = parse_options(argc, argv, &cmd_idf_crt_bundle_desc);
	(void)argv;

	if (opt_inputs.nr == 0)
		die("at least one @b{--input} is required");

	/* Pre-load --filter so we know at per-input dispatch time whether
	 * it applies to this input's basename. */
	if (opt_filter) {
		if (sbuf_read_file(&filter_buf, opt_filter) < 0)
			die_errno("cannot read filter '%s'", opt_filter);
		filter_load(&filter_set, filter_buf.buf, filter_buf.len);
	}

	for (size_t i = 0; i < opt_inputs.nr; i++) {
		const char *path = opt_inputs.v[i];
		const char *base = strrchr(path, '/');
		base = base ? base + 1 : path;

		if (opt_filter && !strcmp(base, "cacrt_all.pem")) {
			struct sbuf pem = SBUF_INIT;
			size_t filtered_len;
			char *filtered;

			if (sbuf_read_file(&pem, path) < 0)
				die_errno("cannot read '%s'", path);
			status("Parsing certificates from %s", path);
			filtered = apply_filter(pem.buf, pem.len, &filter_set,
						&filtered_len);
			add_from_pem(&list, filtered, filtered_len, path);
			free(filtered);
			sbuf_release(&pem);
		} else {
			add_from_path(&list, path);
		}
	}

	status("Successfully added %zu certificates in total", list.nr);

	if ((int)list.nr > opt_max_certs) {
		err("certificates in the bundle (%zu) exceed --max-certs (%d)",
		    list.nr, opt_max_certs);
		rc = 2;
		goto out;
	}

	bundle_pack(&list, &bundle);

	if (write_file_atomic(BUNDLE_FILENAME, bundle.buf, bundle.len) < 0) {
		err_errno("write '%s'", BUNDLE_FILENAME);
		rc = 1;
	}

out:
	sbuf_release(&bundle);
	sbuf_release(&filter_buf);
	svec_clear(&filter_set);
	cert_list_release(&list);
	return rc;
}
