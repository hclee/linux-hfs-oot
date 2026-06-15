// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/hfsplus/unicode.c
 *
 * Copyright (C) 2001
 * Brad Boyer (flar@allandria.com)
 * (C) 2003 Ardis Technologies <roman@ardistech.com>
 *
 * Handler routines for unicode strings
 */

#include <linux/types.h>
#include <linux/nls.h>
#include <linux/string.h>
#include <linux/slab.h>

#include <kunit/visibility.h>

#include "hfsplus_fs.h"
#include "hfsplus_raw.h"

/* Fold the case of a unicode char, given the 16 bit value */
/* Returns folded char, or 0 if ignorable */
static inline u16 case_fold(u16 c)
{
	u16 tmp;

	tmp = hfsplus_case_fold_table[c >> 8];
	if (tmp)
		tmp = hfsplus_case_fold_table[tmp + (c & 0xff)];
	else
		tmp = c;
	return tmp;
}

/* Compare unicode strings, return values like normal strcmp */
int hfsplus_strcasecmp(const struct hfsplus_unistr *s1,
		       const struct hfsplus_unistr *s2)
{
	u16 len1, len2, c1, c2;
	const hfsplus_unichr *p1, *p2;

	len1 = be16_to_cpu(s1->length);
	len2 = be16_to_cpu(s2->length);
	p1 = s1->unicode;
	p2 = s2->unicode;

	if (len1 > HFSPLUS_MAX_STRLEN) {
		len1 = HFSPLUS_MAX_STRLEN;
		pr_err("invalid length %u has been corrected to %d\n",
			be16_to_cpu(s1->length), len1);
	}

	if (len2 > HFSPLUS_MAX_STRLEN) {
		len2 = HFSPLUS_MAX_STRLEN;
		pr_err("invalid length %u has been corrected to %d\n",
			be16_to_cpu(s2->length), len2);
	}

	while (1) {
		c1 = c2 = 0;

		while (len1 && !c1) {
			c1 = case_fold(be16_to_cpu(*p1));
			p1++;
			len1--;
		}
		while (len2 && !c2) {
			c2 = case_fold(be16_to_cpu(*p2));
			p2++;
			len2--;
		}

		if (c1 != c2)
			return (c1 < c2) ? -1 : 1;
		if (!c1 && !c2)
			return 0;
	}
}
EXPORT_SYMBOL_IF_KUNIT(hfsplus_strcasecmp);

/* Compare names as a sequence of 16-bit unsigned integers */
int hfsplus_strcmp(const struct hfsplus_unistr *s1,
		   const struct hfsplus_unistr *s2)
{
	u16 len1, len2, c1, c2;
	const hfsplus_unichr *p1, *p2;
	int len;

	len1 = be16_to_cpu(s1->length);
	len2 = be16_to_cpu(s2->length);
	p1 = s1->unicode;
	p2 = s2->unicode;

	if (len1 > HFSPLUS_MAX_STRLEN) {
		len1 = HFSPLUS_MAX_STRLEN;
		pr_err("invalid length %u has been corrected to %d\n",
			be16_to_cpu(s1->length), len1);
	}

	if (len2 > HFSPLUS_MAX_STRLEN) {
		len2 = HFSPLUS_MAX_STRLEN;
		pr_err("invalid length %u has been corrected to %d\n",
			be16_to_cpu(s2->length), len2);
	}

	for (len = min(len1, len2); len > 0; len--) {
		c1 = be16_to_cpu(*p1);
		c2 = be16_to_cpu(*p2);
		if (c1 != c2)
			return c1 < c2 ? -1 : 1;
		p1++;
		p2++;
	}

	return len1 < len2 ? -1 :
	       len1 > len2 ? 1 : 0;
}
EXPORT_SYMBOL_IF_KUNIT(hfsplus_strcmp);

#define Hangul_SBase	0xac00
#define Hangul_LBase	0x1100
#define Hangul_VBase	0x1161
#define Hangul_TBase	0x11a7
#define Hangul_SCount	11172
#define Hangul_LCount	19
#define Hangul_VCount	21
#define Hangul_TCount	28
#define Hangul_NCount	(Hangul_VCount * Hangul_TCount)


static u16 *hfsplus_compose_lookup(u16 *p, u16 cc)
{
	int i, s, e;

	s = 1;
	e = p[1];
	if (!e || cc < p[s * 2] || cc > p[e * 2])
		return NULL;
	do {
		i = (s + e) / 2;
		if (cc > p[i * 2])
			s = i + 1;
		else if (cc < p[i * 2])
			e = i - 1;
		else
			return hfsplus_compose_table + p[i * 2 + 1];
	} while (s <= e);
	return NULL;
}

/*
 * In HFS+, a filename can contain / because : is the separator.
 * The slash is a valid filename character on macOS.
 * But on Linux, / is the path separator and
 * it cannot appear in a filename component.
 * There's a parallel mapping for the NUL character (0 -> U+2400).
 * NUL terminates strings in C/POSIX but is valid in HFS+ filenames.
 */
static inline
void hfsplus_mac2linux_compatibility_check(u16 symbol, u16 *conversion,
					   int name_type)
{
	*conversion = symbol;

	switch (name_type) {
	case HFS_XATTR_NAME:
		/* ignore conversion */
		return;

	default:
		/* continue logic */
		break;
	}

	switch (symbol) {
	case 0:
		*conversion = 0x2400;
		break;
	case '/':
		*conversion = ':';
		break;
	}
}

static int hfsplus_uni2asc(struct super_block *sb,
			   const struct hfsplus_unistr *ustr,
			   int max_len, char *astr, int *len_p,
			   int name_type)
{
	const hfsplus_unichr *ip;
	struct nls_table *nls = HFSPLUS_SB(sb)->nls;
	u8 *op;
	u16 cc, c0, c1;
	u16 *ce1, *ce2;
	int i, len, ustrlen, res, compose;

	op = astr;
	ip = ustr->unicode;

	ustrlen = be16_to_cpu(ustr->length);
	if (ustrlen > max_len) {
		ustrlen = max_len;
		pr_err("invalid length %u has been corrected to %d\n",
			be16_to_cpu(ustr->length), ustrlen);
	}

	len = *len_p;
	ce1 = NULL;
	compose = !test_bit(HFSPLUS_SB_NODECOMPOSE, &HFSPLUS_SB(sb)->flags);

	while (ustrlen > 0) {
		c0 = be16_to_cpu(*ip++);
		ustrlen--;
		/* search for single decomposed char */
		if (likely(compose))
			ce1 = hfsplus_compose_lookup(hfsplus_compose_table, c0);
		if (ce1)
			cc = ce1[0];
		else
			cc = 0;
		if (cc) {
			/* start of a possibly decomposed Hangul char */
			if (cc != 0xffff)
				goto done;
			if (!ustrlen)
				goto same;
			c1 = be16_to_cpu(*ip) - Hangul_VBase;
			if (c1 < Hangul_VCount) {
				/* compose the Hangul char */
				cc = (c0 - Hangul_LBase) * Hangul_VCount;
				cc = (cc + c1) * Hangul_TCount;
				cc += Hangul_SBase;
				ip++;
				ustrlen--;
				if (!ustrlen)
					goto done;
				c1 = be16_to_cpu(*ip) - Hangul_TBase;
				if (c1 > 0 && c1 < Hangul_TCount) {
					cc += c1;
					ip++;
					ustrlen--;
				}
				goto done;
			}
		}
		while (1) {
			/* main loop for common case of not composed chars */
			if (!ustrlen)
				goto same;
			c1 = be16_to_cpu(*ip);
			if (likely(compose))
				ce1 = hfsplus_compose_lookup(
					hfsplus_compose_table, c1);
			if (ce1)
				break;
			hfsplus_mac2linux_compatibility_check(c0, &c0,
							      name_type);
			res = nls->uni2char(c0, op, len);
			if (res < 0) {
				if (res == -ENAMETOOLONG)
					goto out;
				*op = '?';
				res = 1;
			}
			op += res;
			len -= res;
			c0 = c1;
			ip++;
			ustrlen--;
		}
		ce2 = hfsplus_compose_lookup(ce1, c0);
		if (ce2) {
			i = 1;
			while (i < ustrlen) {
				ce1 = hfsplus_compose_lookup(ce2,
					be16_to_cpu(ip[i]));
				if (!ce1)
					break;
				i++;
				ce2 = ce1;
			}
			cc = ce2[0];
			if (cc) {
				ip += i;
				ustrlen -= i;
				goto done;
			}
		}
same:
		hfsplus_mac2linux_compatibility_check(c0, &cc,
						      name_type);
done:
		res = nls->uni2char(cc, op, len);
		if (res < 0) {
			if (res == -ENAMETOOLONG)
				goto out;
			*op = '?';
			res = 1;
		}
		op += res;
		len -= res;
	}
	res = 0;
out:
	*len_p = (char *)op - astr;
	return res;
}

inline int hfsplus_uni2asc_str(struct super_block *sb,
			       const struct hfsplus_unistr *ustr, char *astr,
			       int *len_p)
{
	return hfsplus_uni2asc(sb,
				ustr, HFSPLUS_MAX_STRLEN,
				astr, len_p,
				HFS_REGULAR_NAME);
}
EXPORT_SYMBOL_IF_KUNIT(hfsplus_uni2asc_str);

inline int hfsplus_uni2asc_xattr_str(struct super_block *sb,
				     const struct hfsplus_attr_unistr *ustr,
				     char *astr, int *len_p)
{
	return hfsplus_uni2asc(sb, (const struct hfsplus_unistr *)ustr,
				HFSPLUS_ATTR_MAX_STRLEN, astr, len_p,
				HFS_XATTR_NAME);
}
EXPORT_SYMBOL_IF_KUNIT(hfsplus_uni2asc_xattr_str);

/*
 * In HFS+, a filename can contain / because : is the separator.
 * The slash is a valid filename character on macOS.
 * But on Linux, / is the path separator and
 * it cannot appear in a filename component.
 * There's a parallel mapping for the NUL character (0 -> U+2400).
 * NUL terminates strings in C/POSIX but is valid in HFS+ filenames.
 */
static inline
void hfsplus_linux2mac_compatibility_check(wchar_t *uc, int name_type)
{
	switch (name_type) {
	case HFS_XATTR_NAME:
		/* ignore conversion */
		return;

	default:
		/* continue logic */
		break;
	}

	switch (*uc) {
	case 0x2400:
		*uc = 0;
		break;
	case ':':
		*uc = '/';
		break;
	}
}

/*
 * Convert one or more ASCII characters into a single unicode character.
 * Returns the number of ASCII characters corresponding to the unicode char.
 */
static inline int asc2unichar(struct super_block *sb, const char *astr, int len,
			      wchar_t *uc, int name_type)
{
	int size = HFSPLUS_SB(sb)->nls->char2uni(astr, len, uc);

	if (size <= 0) {
		*uc = '?';
		size = 1;
	}

	hfsplus_linux2mac_compatibility_check(uc, name_type);
	return size;
}

/* Decomposes a non-Hangul unicode character. */
static u16 *hfsplus_decompose_nonhangul(wchar_t uc, int *size)
{
	int off;

	off = hfsplus_decompose_table[(uc >> 12) & 0xf];
	if (off == 0 || off == 0xffff)
		return NULL;

	off = hfsplus_decompose_table[off + ((uc >> 8) & 0xf)];
	if (!off)
		return NULL;

	off = hfsplus_decompose_table[off + ((uc >> 4) & 0xf)];
	if (!off)
		return NULL;

	off = hfsplus_decompose_table[off + (uc & 0xf)];
	*size = off & 3;
	if (*size == 0)
		return NULL;
	return hfsplus_decompose_table + (off / 4);
}

/*
 * Try to decompose a unicode character as Hangul. Return 0 if @uc is not
 * precomposed Hangul, otherwise return the length of the decomposition.
 *
 * This function was adapted from sample code from the Unicode Standard
 * Annex #15: Unicode Normalization Forms, version 3.2.0.
 *
 * Copyright (C) 1991-2018 Unicode, Inc.  All rights reserved.  Distributed
 * under the Terms of Use in http://www.unicode.org/copyright.html.
 */
static int hfsplus_try_decompose_hangul(wchar_t uc, u16 *result)
{
	int index;
	int l, v, t;

	index = uc - Hangul_SBase;
	if (index < 0 || index >= Hangul_SCount)
		return 0;

	l = Hangul_LBase + index / Hangul_NCount;
	v = Hangul_VBase + (index % Hangul_NCount) / Hangul_TCount;
	t = Hangul_TBase + index % Hangul_TCount;

	result[0] = l;
	result[1] = v;
	if (t != Hangul_TBase) {
		result[2] = t;
		return 3;
	}
	return 2;
}

struct hfsplus_decomp_v32_entry {
	u16 key;
	u8 size;
	u16 decomp[3];
};

static const struct hfsplus_decomp_v32_entry hfsplus_decomp_v32_table[] = {
	{ 0x01F8, 2, { 0x004E, 0x0300 } },
	{ 0x01F9, 2, { 0x006E, 0x0300 } },
	{ 0x0218, 2, { 0x0053, 0x0326 } },
	{ 0x0219, 2, { 0x0073, 0x0326 } },
	{ 0x021A, 2, { 0x0054, 0x0326 } },
	{ 0x021B, 2, { 0x0074, 0x0326 } },
	{ 0x021E, 2, { 0x0048, 0x030C } },
	{ 0x021F, 2, { 0x0068, 0x030C } },
	{ 0x0226, 2, { 0x0041, 0x0307 } },
	{ 0x0227, 2, { 0x0061, 0x0307 } },
	{ 0x0228, 2, { 0x0045, 0x0327 } },
	{ 0x0229, 2, { 0x0065, 0x0327 } },
	{ 0x022A, 3, { 0x004F, 0x0308, 0x0304 } },
	{ 0x022B, 3, { 0x006F, 0x0308, 0x0304 } },
	{ 0x022C, 3, { 0x004F, 0x0303, 0x0304 } },
	{ 0x022D, 3, { 0x006F, 0x0303, 0x0304 } },
	{ 0x022E, 2, { 0x004F, 0x0307 } },
	{ 0x022F, 2, { 0x006F, 0x0307 } },
	{ 0x0230, 3, { 0x004F, 0x0307, 0x0304 } },
	{ 0x0231, 3, { 0x006F, 0x0307, 0x0304 } },
	{ 0x0232, 2, { 0x0059, 0x0304 } },
	{ 0x0233, 2, { 0x0079, 0x0304 } },
	{ 0x0400, 2, { 0x0415, 0x0300 } },
	{ 0x040D, 2, { 0x0418, 0x0300 } },
	{ 0x0450, 2, { 0x0435, 0x0300 } },
	{ 0x045D, 2, { 0x0438, 0x0300 } },
	{ 0x04EC, 2, { 0x042D, 0x0308 } },
	{ 0x04ED, 2, { 0x044D, 0x0308 } },
	{ 0x0622, 2, { 0x0627, 0x0653 } },
	{ 0x0623, 2, { 0x0627, 0x0654 } },
	{ 0x0624, 2, { 0x0648, 0x0654 } },
	{ 0x0625, 2, { 0x0627, 0x0655 } },
	{ 0x0626, 2, { 0x064A, 0x0654 } },
	{ 0x06C0, 2, { 0x06D5, 0x0654 } },
	{ 0x06C2, 2, { 0x06C1, 0x0654 } },
	{ 0x06D3, 2, { 0x06D2, 0x0654 } },
	{ 0x0A33, 2, { 0x0A32, 0x0A3C } },
	{ 0x0A36, 2, { 0x0A38, 0x0A3C } },
	{ 0x0DDA, 2, { 0x0DD9, 0x0DCA } },
	{ 0x0DDC, 2, { 0x0DD9, 0x0DCF } },
	{ 0x0DDD, 3, { 0x0DD9, 0x0DCF, 0x0DCA } },
	{ 0x0DDE, 2, { 0x0DD9, 0x0DDF } },
	{ 0x1026, 2, { 0x1025, 0x102E } },
	{ 0xFB1D, 2, { 0x05D9, 0x05B4 } },
};

/* Decomposes a single unicode character as Unicode 3.2.0 single replacement. */
static int hfsplus_try_decompose_v32_single(wchar_t uc, u16 *decomp)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(hfsplus_decomp_v32_table); i++) {
		if (hfsplus_decomp_v32_table[i].key == uc) {
			int sz = hfsplus_decomp_v32_table[i].size;

			memcpy(decomp, hfsplus_decomp_v32_table[i].decomp,
			       sz * sizeof(u16));
			return sz;
		}
	}
	return 0;
}

static void hfsplus_remove_chars(u16 *decomp, int *size, int index, int count)
{
	memmove(&decomp[index], &decomp[index + count],
		(*size - (index + count)) * sizeof(u16));
	*size -= count;
}

/* Corrects the decomposed character to Unicode 3.2.0 standard. */
static void hfsplus_correct_decomp_v32(u16 *decomp, int *size)
{
	int i;

	for (i = 0; i < *size - 1; i++) {
		u16 c1 = decomp[i];
		u16 c2 = decomp[i + 1];

		/*
		 * Greek capital vowels/symbols + Combining Vertical Line Above
		 * -> Combining Acute Accent
		 */
		if (c1 >= 0x0391 && c1 <= 0x03D2) {
			if (c2 == 0x030D) {
				decomp[i + 1] = 0x0301;
				continue;
			}
		}
		/* Diaeresis + Combining Vertical Line Above -> Combining Acute Accent */
		if (c1 == 0x00A8 && c2 == 0x030D) {
			decomp[i + 1] = 0x0301;
		/* Combining Diaeresis + Combining Vertical Line Above -> Combining Acute Accent */
		} else if (c1 == 0x0308 && c2 == 0x030D) {
			decomp[i + 1] = 0x0301;
		/* Combining Breve + Combining Dot Above -> Combining Candrabindu */
		} else if (c1 == 0x0306 && c2 == 0x0307) {
			decomp[i] = 0x0310;
			hfsplus_remove_chars(decomp, size, i + 1, 1);
			i--;
		/* Bengali Letter Ba + Bengali Sign Nukta -> Bengali Letter Ra */
		} else if (c1 == 0x09AC && c2 == 0x09BC) {
			decomp[i] = 0x09B0;
			hfsplus_remove_chars(decomp, size, i + 1, 1);
			i--;
		/* Gurmukhi Letter Dda + Gurmukhi Sign Nukta -> Gurmukhi Letter Rra */
		} else if (c1 == 0x0A21 && c2 == 0x0A3C) {
			decomp[i] = 0x0A5C;
			hfsplus_remove_chars(decomp, size, i + 1, 1);
			i--;
		/* Odia Letter Ya + Odia Sign Nukta -> Odia Letter Yya */
		} else if (c1 == 0x0B2F && c2 == 0x0B3C) {
			decomp[i] = 0x0B5F;
			hfsplus_remove_chars(decomp, size, i + 1, 1);
			i--;
		/* Thai Character Nikhahit + Thai Character Sara Aa -> Thai Character Sara Am */
		} else if (c1 == 0x0E4D && c2 == 0x0E32) {
			decomp[i] = 0x0E33;
			hfsplus_remove_chars(decomp, size, i + 1, 1);
			i--;
		/* Lao Semivowel Sign Nikhahit + Lao Vowel Sign Aa -> Lao Vowel Sign Am */
		} else if (c1 == 0x0ECD && c2 == 0x0EB2) {
			decomp[i] = 0x0EB3;
			hfsplus_remove_chars(decomp, size, i + 1, 1);
			i--;
		}
	}

	/* Three character sequence check: 0x0FB2/0x0FB3 + 0x0F80 + 0x0F71 -> 0x0F77/0x0F79 */
	if (*size >= 3) {
		for (i = 0; i < *size - 2; i++) {
			if (decomp[i + 1] == 0x0F80 && decomp[i + 2] == 0x0F71) {
				/*
				 * Tibetan Subjoined Ra + Vowel Sign Mryel + Vowel Sign Aa
				 * -> Vowel Sign Vocalic R
				 */
				if (decomp[i] == 0x0FB2) {
					decomp[i] = 0x0F77;
					hfsplus_remove_chars(decomp, size, i + 1, 2);
					i--;
				/*
				 * Tibetan Subjoined La + Vowel Sign Mryel + Vowel Sign Aa
				 * -> Vowel Sign Vocalic L
				 */
				} else if (decomp[i] == 0x0FB3) {
					decomp[i] = 0x0F79;
					hfsplus_remove_chars(decomp, size, i + 1, 2);
					i--;
				}
			}
		}
	}
}


/* Decomposes a single unicode character. */
static u16 *decompose_unichar(struct super_block *sb, wchar_t uc, int *size, u16 *decomp_buf)
{
	struct hfsplus_sb_info *sbi = HFSPLUS_SB(sb);
	u16 *result;

	/* Hangul is handled separately */
	result = decomp_buf;
	*size = hfsplus_try_decompose_hangul(uc, result);
	if (*size == 0) {
		if (sbi->unicode_version == HFSPLUS_UNICODE_VERSION_3_2) {
			*size = hfsplus_try_decompose_v32_single(uc, decomp_buf);
			if (*size > 0)
				return result;
		}
		u16 *raw_decomp = hfsplus_decompose_nonhangul(uc, size);

		if (raw_decomp) {
			memcpy(decomp_buf, raw_decomp, *size * sizeof(u16));
			if (sbi->unicode_version == HFSPLUS_UNICODE_VERSION_3_2)
				hfsplus_correct_decomp_v32(decomp_buf, size);
		} else {
			result = NULL;
		}
	}
	return result;
}

#define HFSPLUS_LO_FIELD_BIT_SIZE 4
#define HFSPLUS_SHIFT_UNICHAR_OFFSET 0x0500
#define HFSPLUS_SHIFT_UNICHAR_LIMIT 0x3600

#define HFSPLUS_LO_FIELD_ENTRY_COUNT (1 << HFSPLUS_LO_FIELD_BIT_SIZE)
#define HFSPLUS_HI_FIELD_ENTRY_COUNT \
	(HFSPLUS_SHIFT_UNICHAR_LIMIT >> HFSPLUS_LO_FIELD_BIT_SIZE)

static const s8 classAndReplIndex[HFSPLUS_HI_FIELD_ENTRY_COUNT] = {
	-1, 75, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0xFB00- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0xFC00- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0xFD00- */
	-1, -1, 76, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0xFE00- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0xFF00- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, 0,  -1, -1, -1, -1, -1, /* uChar 0x0000- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, 1, /* uChar 0x0100- */
	-1, 2,	3,  4,	-1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x0200- */
	5,  6,	7,  8,	9,  -1, 10, -1,
	-1, 11, 12, 13, 14, 15, -1, -1, /* uChar 0x0300- */
	16, -1, -1, -1, -1, 17, -1, -1,
	18, -1, -1, -1, -1, -1, 19, -1, /* uChar 0x0400- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, 20, 21, 22, 23, -1, -1, -1, /* uChar 0x0500- */
	-1, -1, 24, -1, 25, 26, -1, 27,
	-1, -1, -1, -1, 28, 29, 30, -1, /* uChar 0x0600- */
	-1, 31, -1, 32, 33, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x0700- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x0800- */
	-1, -1, -1, 34, 35, 36, -1, -1,
	-1, -1, 37, 38, 39, -1, -1, -1, /* uChar 0x0900- */
	-1, -1, 40, 41, 42, -1, -1, -1,
	-1, -1, -1, 43, 44, -1, -1, -1, /* uChar 0x0A00- */
	-1, -1, 45, 46, 47, -1, -1, -1,
	-1, -1, -1, -1, 48, -1, -1, -1, /* uChar 0x0B00- */
	-1, -1, -1, -1, 49, 50, -1, -1,
	-1, -1, -1, -1, 51, -1, -1, -1, /* uChar 0x0C00- */
	-1, -1, -1, -1, 52, -1, -1, -1,
	-1, -1, -1, -1, 53, 54, -1, -1, /* uChar 0x0D00- */
	-1, -1, -1, 55, 56, -1, -1, -1,
	-1, -1, -1, 57, 58, -1, -1, -1, /* uChar 0x0E00- */
	-1, 59, -1, 60, -1, -1, -1, 61,
	62, -1, -1, 63, 64, -1, -1, -1, /* uChar 0x0F00- */
	-1, -1, 65, 66, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x1000- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x1100- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x1200- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x1300- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x1400- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x1500- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x1600- */
	-1, 67, -1, 68, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, 69, -1, -1, /* uChar 0x1700- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, 70, -1, -1, -1, -1, -1, /* uChar 0x1800- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x1900- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x1A00- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x1B00- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x1C00- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x1D00- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x1E00- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x1F00- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, 71, 72, -1, /* uChar 0x2000- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x2100- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x2200- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x2300- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x2400- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x2500- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x2600- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x2700- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x2800- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x2900- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x2A00- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x2B00- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x2C00- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x2D00- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x2E00- */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, /* uChar 0x2F00- */
	-1, -1, 73, -1, -1, -1, -1, -1,
	-1, 74, -1, -1, -1, -1, -1, -1 /* uChar 0x3000- */
};

static const u8 combClassRanges[][HFSPLUS_LO_FIELD_ENTRY_COUNT] = {
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x00A0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x01F0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0210- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0220- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0230- */
	{ 230, 230, 230, 230, 230, 230, 230, 230, 230, 230, 230, 230, 230, 230, 230, 230 }, /* uChar 0x0300- */
	{ 230, 230, 230, 230, 230, 232, 220, 220, 220, 220, 232, 216, 220, 220, 220, 220 }, /* uChar 0x0310- */
	{ 220, 202, 202, 220, 220, 220, 220, 202, 202, 220, 220, 220, 220, 220, 220, 220 }, /* uChar 0x0320- */
	{ 220, 220, 220, 220, 1, 1, 1, 1, 1, 220, 220, 220, 220, 230, 230, 230 }, /* uChar 0x0330- */
	{ 230, 230, 230, 230, 230, 240, 230, 220, 220, 220, 230, 230, 230, 220, 220, 0 }, /* uChar 0x0340- */
	{ 234, 234, 233, 230, 230, 230, 230, 230, 230, 230, 230, 230, 230, 230, 230, 230 }, /* uChar 0x0360- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0390- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x03A0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x03B0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x03C0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x03D0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0400- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0450- */
	{ 0, 0, 0, 230, 230, 230, 230, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0480- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x04E0- */
	{ 0, 220, 230, 230, 230, 230, 220, 230, 230, 230, 222, 220, 230, 230, 230, 230 }, /* uChar 0x0590- */
	{ 230, 230, 0, 220, 220, 220, 220, 220, 230, 230, 220, 230, 230, 222, 228, 230 }, /* uChar 0x05A0- */
	{ 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 0, 20, 21, 22, 0, 23 }, /* uChar 0x05B0- */
	{ 0, 24, 25, 0, 230, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x05C0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0620- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 27, 28, 29, 30, 31 }, /* uChar 0x0640- */
	{ 32, 33, 34, 230, 230, 220, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0650- */
	{ 35, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0670- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x06C0- */
	{ 0, 0, 0, 0, 0, 0, 230, 230, 230, 230, 230, 230, 230, 0, 0, 230 }, /* uChar 0x06D0- */
	{ 230, 230, 230, 220, 230, 0, 0, 230, 230, 0, 220, 230, 230, 220, 0, 0 }, /* uChar 0x06E0- */
	{ 0, 36, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0710- */
	{ 230, 220, 230, 230, 220, 230, 230, 220, 220, 220, 230, 220, 220, 230, 220, 230 }, /* uChar 0x0730- */
	{ 230, 230, 220, 230, 220, 230, 220, 230, 220, 230, 230, 0, 0, 0, 0, 0 }, /* uChar 0x0740- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0 }, /* uChar 0x0930- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0 }, /* uChar 0x0940- */
	{ 0, 230, 220, 230, 230, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0950- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x09A0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0 }, /* uChar 0x09B0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0 }, /* uChar 0x09C0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0A20- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0 }, /* uChar 0x0A30- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0 }, /* uChar 0x0A40- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0 }, /* uChar 0x0AB0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0 }, /* uChar 0x0AC0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0B20- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0 }, /* uChar 0x0B30- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0 }, /* uChar 0x0B40- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0 }, /* uChar 0x0BC0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0 }, /* uChar 0x0C40- */
	{ 0, 0, 0, 0, 0, 84, 91, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0C50- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0 }, /* uChar 0x0CC0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0 }, /* uChar 0x0D40- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0 }, /* uChar 0x0DC0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0DD0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 103, 103, 9, 0, 0, 0, 0, 0 }, /* uChar 0x0E30- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 107, 107, 107, 107, 0, 0, 0, 0 }, /* uChar 0x0E40- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 118, 118, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0EB0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 122, 122, 122, 122, 0, 0, 0, 0 }, /* uChar 0x0EC0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 220, 220, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0F10- */
	{ 0, 0, 0, 0, 0, 220, 0, 220, 0, 216, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0F30- */
	{ 0, 129, 130, 0, 132, 0, 0, 0, 0, 0, 130, 130, 130, 130, 0, 0 }, /* uChar 0x0F70- */
	{ 130, 0, 230, 230, 9, 0, 230, 230, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0F80- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0FB0- */
	{ 0, 0, 0, 0, 0, 0, 220, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x0FC0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x1020- */
	{ 0, 0, 0, 0, 0, 0, 0, 7, 0, 9, 0, 0, 0, 0, 0, 0 }, /* uChar 0x1030- */
	{ 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x1710- */
	{ 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x1730- */
	{ 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0x17D0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 228, 0, 0, 0, 0, 0, 0 }, /* uChar 0x18A0- */
	{ 230, 230, 1, 1, 230, 230, 230, 230, 1, 1, 1, 230, 230, 0, 0, 0 }, /* uChar 0x20D0- */
	{ 0, 230, 0, 0, 0, 1, 1, 230, 220, 230, 1, 0, 0, 0, 0, 0 }, /* uChar 0x20E0- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 218, 228, 232, 222, 224, 224 }, /* uChar 0x3020- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 0, 0, 0, 0, 0 }, /* uChar 0x3090- */
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 26, 0 }, /* uChar 0xFB10- */
	{ 230, 230, 230, 230, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* uChar 0xFE20- */
};

static u8 hfsplus_get_ccc(u16 c)
{
	u16 shiftUniChar = (u16)(c + HFSPLUS_SHIFT_UNICHAR_OFFSET);

	if (shiftUniChar < HFSPLUS_SHIFT_UNICHAR_LIMIT) {
		int rangeIndex = classAndReplIndex[shiftUniChar >> 4];

		if (rangeIndex >= 0)
			return combClassRanges[rangeIndex][shiftUniChar & 15];
	}
	return 0;
}

static void hfsplus_sort_combining(u16 *unicode, int len)
{
	int i, j;
	bool swapped;

	for (i = 0; i < len - 1; i++) {
		u16 c1 = unicode[i];
		u8 ccc1 = hfsplus_get_ccc(c1);

		if (ccc1 == 0)
			continue;

		/* Find the end of this combining sequence */
		for (j = i + 1; j < len; j++) {
			u16 c2 = unicode[j];
			u8 ccc2 = hfsplus_get_ccc(c2);

			if (ccc2 == 0)
				break;
		}

		/* Sort sequence unicode[i...j-1] by CCC (stable bubble sort) */
		int seq_len = j - i;
		if (seq_len > 1) {
			int k, m;

			for (k = 0; k < seq_len - 1; k++) {
				swapped = false;
				for (m = 0; m < seq_len - k - 1; m++) {
					u16 ch3 = unicode[i + m];
					u16 ch4 = unicode[i + m + 1];
					u8 c3 = hfsplus_get_ccc(ch3);
					u8 c4 = hfsplus_get_ccc(ch4);

					if (c3 > c4) {
						u16 tmp = unicode[i + m];

						unicode[i + m] = unicode[i + m + 1];
						unicode[i + m + 1] = tmp;
						swapped = true;
					}
				}
				if (!swapped)
					break;
			}
		}

		i = j - 1;
	}
}

static int hfsplus_normalize_dentry_name(struct super_block *sb,
					 const char *astr, int len,
					 u16 *out_buf, int max_len)
{
	int size, dsize, decompose, outlen = 0;
	u16 *dstr;
	wchar_t c;
	u16 dhangul[3];

	decompose = !test_bit(HFSPLUS_SB_NODECOMPOSE, &HFSPLUS_SB(sb)->flags);
	while (outlen < max_len && len > 0) {
		size = asc2unichar(sb, astr, len, &c, HFS_REGULAR_NAME);

		if (decompose)
			dstr = decompose_unichar(sb, c, &dsize, dhangul);
		else
			dstr = NULL;
		if (dstr) {
			if (outlen + dsize > max_len)
				break;
			do {
				out_buf[outlen++] = *dstr++;
			} while (--dsize > 0);
		} else {
			out_buf[outlen++] = c;
		}

		astr += size;
		len -= size;
	}

	if (outlen > 0 && HFSPLUS_SB(sb)->unicode_version == HFSPLUS_UNICODE_VERSION_3_2) {
		hfsplus_correct_decomp_v32(out_buf, &outlen);
		hfsplus_sort_combining(out_buf, outlen);
	}

	if (len > 0)
		return -ENAMETOOLONG;

	return outlen;
}

int hfsplus_asc2uni(struct super_block *sb,
		    struct hfsplus_unistr *ustr, int max_unistr_len,
		    const char *astr, int len, int name_type)
{
	int size, dsize, outlen = 0, decompose, i;
	u16 *dstr, *buf;
	wchar_t c;
	u16 dhangul[3];
	int limit = min(max_unistr_len, HFSPLUS_MAX_STRLEN);

	buf = kmalloc_array(HFSPLUS_MAX_STRLEN, sizeof(u16), GFP_NOWAIT);
	if (!buf)
		return -ENOMEM;

	decompose = !test_bit(HFSPLUS_SB_NODECOMPOSE, &HFSPLUS_SB(sb)->flags);
	while (outlen < limit && len > 0) {
		size = asc2unichar(sb, astr, len, &c, name_type);

		if (decompose)
			dstr = decompose_unichar(sb, c, &dsize, dhangul);
		else
			dstr = NULL;
		if (dstr) {
			if (outlen + dsize > limit)
				break;
			do {
				buf[outlen++] = *dstr++;
			} while (--dsize > 0);
		} else
			buf[outlen++] = c;

		astr += size;
		len -= size;
	}

	if (len > 0) {
		kfree(buf);
		return -ENAMETOOLONG;
	}

	if (outlen > 0 && HFSPLUS_SB(sb)->unicode_version == HFSPLUS_UNICODE_VERSION_3_2) {
		hfsplus_correct_decomp_v32(buf, &outlen);
		hfsplus_sort_combining(buf, outlen);
	}

	for (i = 0; i < outlen; i++)
		ustr->unicode[i] = cpu_to_be16(buf[i]);
	ustr->length = cpu_to_be16(outlen);

	kfree(buf);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(hfsplus_asc2uni);

int hfsplus_hash_dentry(const struct dentry *dentry, struct qstr *str)
{
	struct super_block *sb = dentry->d_sb;
	int casefold, len, i;
	unsigned long hash;
	u16 c2;
	u16 *normal_name;

	normal_name = kmalloc_array(HFSPLUS_MAX_STRLEN, sizeof(u16), GFP_NOWAIT);
	if (!normal_name)
		return -ENOMEM;

	len = hfsplus_normalize_dentry_name(sb, str->name, str->len,
					     normal_name, HFSPLUS_MAX_STRLEN);
	if (unlikely(len < 0)) {
		kfree(normal_name);
		return len;
	}

	casefold = test_bit(HFSPLUS_SB_CASEFOLD, &HFSPLUS_SB(sb)->flags);
	hash = init_name_hash(dentry);

	for (i = 0; i < len; i++) {
		c2 = normal_name[i];
		if (casefold)
			c2 = case_fold(c2);
		if (!casefold || c2)
			hash = partial_name_hash(c2, hash);
	}

	str->hash = end_name_hash(hash);

	kfree(normal_name);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(hfsplus_hash_dentry);

int hfsplus_compare_dentry(const struct dentry *dentry,
		unsigned int len, const char *str, const struct qstr *name)
{
	struct super_block *sb = dentry->d_sb;
	int casefold, i1, i2, len1, len2;
	u16 *normal_name1, *normal_name2;
	int ret = 0;

	normal_name1 = kmalloc_array(HFSPLUS_MAX_STRLEN, sizeof(u16), GFP_NOWAIT);
	if (!normal_name1)
		return -ENOMEM;

	normal_name2 = kmalloc_array(HFSPLUS_MAX_STRLEN, sizeof(u16), GFP_NOWAIT);
	if (!normal_name2) {
		kfree(normal_name1);
		return -ENOMEM;
	}

	len1 = hfsplus_normalize_dentry_name(sb, str, len,
					     normal_name1, HFSPLUS_MAX_STRLEN);
	if (unlikely(len1 < 0)) {
		ret = len1;
		goto out;
	}
	len2 = hfsplus_normalize_dentry_name(sb, name->name, name->len,
					     normal_name2, HFSPLUS_MAX_STRLEN);
	if (unlikely(len2 < 0)) {
		ret = len2;
		goto out;
	}

	casefold = test_bit(HFSPLUS_SB_CASEFOLD, &HFSPLUS_SB(sb)->flags);

	i1 = i2 = 0;
	while (1) {
		u16 c1 = 0, c2 = 0;

		while (i1 < len1 && !c1) {
			c1 = normal_name1[i1];
			if (casefold)
				c1 = case_fold(c1);
			i1++;
		}
		while (i2 < len2 && !c2) {
			c2 = normal_name2[i2];
			if (casefold)
				c2 = case_fold(c2);
			i2++;
		}

		if (c1 != c2) {
			ret = (c1 < c2) ? -1 : 1;
			goto out;
		}
		if (!c1 && !c2)
			break;
	}

out:
	kfree(normal_name1);
	kfree(normal_name2);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(hfsplus_compare_dentry);
