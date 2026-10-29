/*
 * Shared ndb text emitter.
 *
 * tab_serialize writes into a growable memory buffer; tab_writer writes to a
 * temp file.  Both paths must use the same value quoting and text-entity
 * rules, so the emission logic lives here behind a tiny write callback.
 */

#include <tab_internal.h>

static int
emit_str(TabEmitFn emit, void *arg, const char *s)
{
	return emit(arg, s, strlen(s));
}

static int
emit_c(TabEmitFn emit, void *arg, char c)
{
	return emit(arg, &c, 1);
}

static int
emit_schema_kv(TabEmitFn emit, void *arg, const char *attr, const char *val)
{
	if(emit_str(emit, arg, attr) < 0) return -1;
	if(emit_c(emit, arg, '=') < 0) return -1;
	if(val != nil && *val != 0){
		if(emit_str(emit, arg, val) < 0) return -1;
	}
	return 0;
}

static int
emit_cell_value(TabEmitFn emit, void *arg, const char *val)
{
	char *wire;

	if(val == nil || *val == 0)
		return 0;
	if(tab_text_needs_encoding(val)){
		wire = tab_text_wire_encode(val);
		if(wire == nil)
			return -1;
		if(tab_text_needs_quote(wire)){
			if(emit_c(emit, arg, '"') < 0){
				free(wire);
				return -1;
			}
			if(emit_str(emit, arg, wire) < 0){
				free(wire);
				return -1;
			}
			if(emit_c(emit, arg, '"') < 0){
				free(wire);
				return -1;
			}
			free(wire);
			return 0;
		}
		if(emit_str(emit, arg, wire) < 0){
			free(wire);
			return -1;
		}
		free(wire);
		return 0;
	}
	if(tab_text_needs_quote(val)){
		if(emit_c(emit, arg, '"') < 0) return -1;
		if(emit_str(emit, arg, val) < 0) return -1;
		if(emit_c(emit, arg, '"') < 0) return -1;
		return 0;
	}
	return emit_str(emit, arg, val);
}

static int
emit_cell_kv(TabEmitFn emit, void *arg, Ndbtuple *tp)
{
	if(emit_str(emit, arg, tp->attr) < 0) return -1;
	if(emit_c(emit, arg, '=') < 0) return -1;
	if(tab_tuple_is_nil(tp))
		return emit_str(emit, arg, "nil");
	return emit_cell_value(emit, arg, tp->val);
}

int
tab_emit_schema(TabSchema *schema, TabEmitFn emit, void *arg)
{
	int i, j;

	if(schema == nil || emit == nil){
		tab_seterror("tab_emit_schema: nil argument");
		return -1;
	}
	if(emit_schema_kv(emit, arg, "schema", schema->name) < 0) return -1;
	if(emit_c(emit, arg, '\n') < 0) return -1;

	for(i = 0; i < schema->ncols; i++){
		TabCol *c = &schema->cols[i];
		if(emit_c(emit, arg, '\t') < 0) return -1;
		if(emit_schema_kv(emit, arg, "col", c->name) < 0) return -1;
		if(c->type != nil){
			if(emit_c(emit, arg, ' ') < 0) return -1;
			if(emit_schema_kv(emit, arg, "type", c->type) < 0) return -1;
		}
		for(j = 0; j < c->nattrs; j++){
			if(emit_c(emit, arg, ' ') < 0) return -1;
			if(emit_schema_kv(emit, arg, c->attrs[j].key,
				c->attrs[j].val) < 0) return -1;
		}
		if(emit_c(emit, arg, '\n') < 0) return -1;
	}
	return 0;
}

int
tab_emit_row(Ndbtuple *chain, TabEmitFn emit, void *arg)
{
	Ndbtuple *tp;
	int first;

	if(chain == nil || emit == nil){
		tab_seterror("tab_emit_row: nil argument");
		return -1;
	}
	first = 1;
	for(tp = chain; tp != nil; tp = tp->entry){
		if(!first){
			if(emit_c(emit, arg, '\t') < 0) return -1;
		}
		if(emit_cell_kv(emit, arg, tp) < 0) return -1;
		if(emit_c(emit, arg, '\n') < 0) return -1;
		first = 0;
	}
	return 0;
}
