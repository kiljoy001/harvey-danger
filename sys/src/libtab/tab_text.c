/*
 * libtab plain-text cell wire codec.
 *
 * Native ndb values can be unquoted tokens or double-quoted strings, but
 * libndb's parser has no escape for a literal double quote and cannot carry
 * embedded newlines.  Libtab writes familiar HTML-style entities for only the
 * bytes ndb cannot represent safely, keeping the rest of the value readable.
 */

#include <tab_internal.h>

static const char LegacyPrefix[] = "__libtab_text64_v1:";
static const char LegacyMagic[] = "libtab-text-v1:";

static int
has_prefix(const char *s, const char *prefix)
{
	int n;

	if(s == nil || prefix == nil)
		return 0;
	n = strlen(prefix);
	return strncmp((char*)s, (char*)prefix, n) == 0;
}

int
tab_text_needs_encoding(const char *val)
{
	const unsigned char *p;

	if(val == nil)
		return 0;
	if(strcmp((char*)val, "nil") == 0)
		return 1;
	if(has_prefix(val, LegacyPrefix))
		return 1;
	for(p = (const unsigned char*)val; *p != 0; p++){
		if(*p == '&' || *p == '"' || *p == '\n' || *p == '\r'
		   || (*p < ' ' && *p != '\t') || *p == 0x7f)
			return 1;
	}
	return 0;
}

int
tab_text_needs_quote(const char *val)
{
	const unsigned char *p;

	if(val == nil || val[0] == 0)
		return 0;
	if(val[0] == '#')
		return 1;
	for(p = (const unsigned char*)val; *p != 0; p++){
		if(*p == ' ' || *p == '\t')
			return 1;
	}
	return 0;
}

static int
append_bytes(char **buf, int *len, int *cap, const char *p, int n)
{
	char *nb;
	int need, newcap;

	need = *len + n + 1;
	if(need > *cap){
		newcap = *cap > 0 ? *cap : 64;
		while(newcap < need)
			newcap *= 2;
		nb = realloc(*buf, newcap);
		if(nb == nil){
			tab_seterror("tab_text_wire_encode: out of memory");
			return -1;
		}
		*buf = nb;
		*cap = newcap;
	}
	memcpy(*buf + *len, p, n);
	*len += n;
	(*buf)[*len] = 0;
	return 0;
}

static int
append_str(char **buf, int *len, int *cap, const char *s)
{
	return append_bytes(buf, len, cap, s, strlen(s));
}

static int
append_c(char **buf, int *len, int *cap, char c)
{
	return append_bytes(buf, len, cap, &c, 1);
}

static int
append_entity(char **buf, int *len, int *cap, unsigned int c)
{
	char tmp[32];

	snprint(tmp, sizeof tmp, "&#%ud;", c);
	return append_str(buf, len, cap, tmp);
}

char *
tab_text_wire_encode(const char *val)
{
	const unsigned char *p;
	char *out;
	int len, cap, legacy_prefix_colon;

	if(val == nil)
		val = "";
	if(strcmp((char*)val, "nil") == 0){
		out = strdup("n&#105;l");
		if(out == nil)
			tab_seterror("tab_text_wire_encode: out of memory");
		return out;
	}

	out = nil;
	len = 0;
	cap = 0;
	legacy_prefix_colon = has_prefix(val, LegacyPrefix)
		? strlen(LegacyPrefix) - 1
		: -1;

	for(p = (const unsigned char*)val; *p != 0; p++){
		if((int)(p - (const unsigned char*)val) == legacy_prefix_colon){
			if(append_entity(&out, &len, &cap, ':') < 0)
				goto fail;
			continue;
		}
		switch(*p){
		case '&':
			if(append_str(&out, &len, &cap, "&amp;") < 0)
				goto fail;
			break;
		case '"':
			if(append_str(&out, &len, &cap, "&quot;") < 0)
				goto fail;
			break;
		case '\n':
			if(append_entity(&out, &len, &cap, 10) < 0)
				goto fail;
			break;
		case '\r':
			if(append_entity(&out, &len, &cap, 13) < 0)
				goto fail;
			break;
		default:
			if((*p < ' ' && *p != '\t') || *p == 0x7f){
				if(append_entity(&out, &len, &cap, *p) < 0)
					goto fail;
			}else if(append_c(&out, &len, &cap, *p) < 0)
				goto fail;
			break;
		}
	}
	if(out == nil)
		out = strdup("");
	if(out == nil){
		tab_seterror("tab_text_wire_encode: out of memory");
		return nil;
	}
	return out;

fail:
	free(out);
	return nil;
}

static int
decode_legacy_text64(Ndbtuple *t)
{
	unsigned char *raw;
	char *decoded;
	int prefixlen, magiclen, rawlen, textlen, i;

	prefixlen = strlen(LegacyPrefix);
	magiclen = strlen(LegacyMagic);
	raw = tab_b64_decode(t->val + prefixlen, &rawlen);
	if(raw == nil){
		tab_seterror("tab_open: malformed legacy text64 cell");
		return -1;
	}
	if(rawlen < magiclen || memcmp(raw, LegacyMagic, magiclen) != 0){
		free(raw);
		tab_seterror("tab_open: legacy text64 cell has bad magic");
		return -1;
	}
	for(i = magiclen; i < rawlen; i++){
		if(raw[i] == 0){
			free(raw);
			tab_seterror("tab_open: legacy text64 cell contains NUL");
			return -1;
		}
	}

	textlen = rawlen - magiclen;
	decoded = malloc(textlen + 1);
	if(decoded == nil){
		free(raw);
		tab_seterror("tab_open: out of memory decoding text64 cell");
		return -1;
	}
	memcpy(decoded, raw + magiclen, textlen);
	decoded[textlen] = 0;
	free(raw);

	ndbsetval(t, decoded, textlen);
	free(decoded);
	tab_tuple_mark_nil(t, 0);
	return 0;
}

static int
entity_name(const char *s, int n, unsigned int *cp)
{
	if(n == 3 && strncmp((char*)s, "amp", n) == 0){
		*cp = '&';
		return 1;
	}
	if(n == 4 && strncmp((char*)s, "quot", n) == 0){
		*cp = '"';
		return 1;
	}
	if(n == 4 && strncmp((char*)s, "apos", n) == 0){
		*cp = '\'';
		return 1;
	}
	if(n == 2 && strncmp((char*)s, "lt", n) == 0){
		*cp = '<';
		return 1;
	}
	if(n == 2 && strncmp((char*)s, "gt", n) == 0){
		*cp = '>';
		return 1;
	}
	return 0;
}

static int
parse_numeric_entity(const char *s, int n, unsigned int *cp)
{
	unsigned int v;
	int i, c, hex;

	if(n < 2 || s[0] != '#')
		return 0;
	i = 1;
	hex = 0;
	if(i < n && (s[i] == 'x' || s[i] == 'X')){
		hex = 1;
		i++;
	}
	if(i >= n)
		return 0;
	v = 0;
	for(; i < n; i++){
		c = s[i];
		if(hex){
			if(c >= '0' && c <= '9')
				c -= '0';
			else if(c >= 'a' && c <= 'f')
				c = c - 'a' + 10;
			else if(c >= 'A' && c <= 'F')
				c = c - 'A' + 10;
			else
				return 0;
			v = v * 16 + c;
		}else{
			if(c < '0' || c > '9')
				return 0;
			v = v * 10 + (c - '0');
		}
		if(v > 0x10ffff)
			return 0;
	}
	*cp = v;
	return 1;
}

static int
append_utf8(char **buf, int *len, int *cap, unsigned int cp)
{
	char tmp[4];
	int n;

	if(cp == 0){
		tab_seterror("tab_open: encoded text cell contains NUL");
		return -1;
	}
	if(cp <= 0x7f){
		tmp[0] = cp;
		n = 1;
	}else if(cp <= 0x7ff){
		tmp[0] = 0xc0 | (cp >> 6);
		tmp[1] = 0x80 | (cp & 0x3f);
		n = 2;
	}else if(cp <= 0xffff){
		tmp[0] = 0xe0 | (cp >> 12);
		tmp[1] = 0x80 | ((cp >> 6) & 0x3f);
		tmp[2] = 0x80 | (cp & 0x3f);
		n = 3;
	}else if(cp <= 0x10ffff){
		tmp[0] = 0xf0 | (cp >> 18);
		tmp[1] = 0x80 | ((cp >> 12) & 0x3f);
		tmp[2] = 0x80 | ((cp >> 6) & 0x3f);
		tmp[3] = 0x80 | (cp & 0x3f);
		n = 4;
	}else
		return 0;
	return append_bytes(buf, len, cap, tmp, n);
}

static char *
decode_entities(const char *s, int *changed)
{
	char *out;
	const char *semi;
	unsigned int cp;
	int len, cap, n;

	out = nil;
	len = 0;
	cap = 0;
	*changed = 0;
	while(*s != 0){
		if(*s != '&'){
			if(append_c(&out, &len, &cap, *s++) < 0)
				goto fail;
			continue;
		}
		semi = strchr((char*)s, ';');
		if(semi == nil){
			if(append_c(&out, &len, &cap, *s++) < 0)
				goto fail;
			continue;
		}
		n = semi - (s + 1);
		if(entity_name(s + 1, n, &cp) ||
		   parse_numeric_entity(s + 1, n, &cp)){
			if(append_utf8(&out, &len, &cap, cp) < 0)
				goto fail;
			*changed = 1;
			s = semi + 1;
			continue;
		}
		if(append_bytes(&out, &len, &cap, s, semi - s + 1) < 0)
			goto fail;
		s = semi + 1;
	}
	if(out == nil)
		out = strdup("");
	if(out == nil){
		tab_seterror("tab_open: out of memory decoding entities");
		return nil;
	}
	return out;

fail:
	free(out);
	return nil;
}

int
tab_text_decode_tuple(Ndbtuple *t)
{
	char *decoded;
	int changed;

	if(t == nil || t->val == nil)
		return 0;
	if(strcmp(t->val, "nil") == 0){
		tab_tuple_mark_nil(t, 1);
		return 0;
	}
	tab_tuple_mark_nil(t, 0);

	if(has_prefix(t->val, LegacyPrefix))
		return decode_legacy_text64(t);

	if(strchr(t->val, '&') == nil)
		return 0;
	decoded = decode_entities(t->val, &changed);
	if(decoded == nil)
		return -1;
	if(changed)
		ndbsetval(t, decoded, strlen(decoded));
	free(decoded);
	tab_tuple_mark_nil(t, 0);
	return 0;
}
