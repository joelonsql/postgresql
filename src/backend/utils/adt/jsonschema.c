/*-------------------------------------------------------------------------
 *
 * jsonschema.c
 *	  Generate JSON Schema for a function's return value.
 *
 * This module provides functions to introspect PostgreSQL functions and
 * generate JSON Schema documents describing their return types. It supports:
 * - Deep introspection for SQL-body functions (analyzing parse trees)
 * - Shallow introspection for all other functions (based on declared type)
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonschema.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "parser/parse_type.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* Function declarations */
PG_FUNCTION_INFO_V1(json_schema_generate_oid);
PG_FUNCTION_INFO_V1(json_schema_generate_regprocedure);
PG_FUNCTION_INFO_V1(json_schema_generate_regproc);

/* Static function prototypes */
static Jsonb *json_schema_generate_worker(Oid funcid);
static Jsonb *schema_for_type(Oid typid, int32 typmod, bool notnull);
static Jsonb *schema_for_composite(Oid typid);
static Jsonb *schema_for_json_any(void);
static Jsonb *wrap_array_items(Jsonb *item_schema, bool is_setof);
static Jsonb *schema_from_query(Query *query, Oid prorettype, bool proretset);
static Jsonb *schema_from_expr(Node *expr, Query *query);
static Jsonb *schema_from_funcexpr(FuncExpr *func, Query *query);
static Jsonb *schema_from_aggref(Aggref *agg, Query *query);
static void push_json_string(JsonbParseState **ps, const char *key, const char *value);
static void push_json_bool(JsonbParseState **ps, const char *key, bool value);
static void merge_jsonb_object(JsonbParseState **ps, Jsonb *obj);

/* Helper functions for emitting format */
static inline void jb_key(JsonbParseState **ps, const char *key);
static inline void jb_string(JsonbParseState **ps, const char *value);
static inline void emit_format(JsonbParseState **ps, Oid typid);

/*
 * json_schema_generate_oid
 *		Generate JSON Schema for a function identified by OID
 */
Datum
json_schema_generate_oid(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	Jsonb	   *result;

	result = json_schema_generate_worker(funcid);
	PG_RETURN_JSONB_P(result);
}

/*
 * json_schema_generate_regprocedure
 *		Generate JSON Schema for a function identified by regprocedure
 */
Datum
json_schema_generate_regprocedure(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	Jsonb	   *result;

	result = json_schema_generate_worker(funcid);
	PG_RETURN_JSONB_P(result);
}

/*
 * json_schema_generate_regproc
 *		Generate JSON Schema for a function identified by regproc
 */
Datum
json_schema_generate_regproc(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	Jsonb	   *result;

	result = json_schema_generate_worker(funcid);
	PG_RETURN_JSONB_P(result);
}

/*
 * json_schema_generate_worker
 *		Common worker function for all entry points
 */
static Jsonb *
json_schema_generate_worker(Oid funcid)
{
	HeapTuple	proctup;
	Form_pg_proc proc;
	Datum		prosqlbody;
	bool		isnull;
	JsonbParseState *ps = NULL;
	JsonbValue *res;
	Jsonb	   *schema_obj = NULL;
	char	   *funcname;
	char	   *schemaname;
	bool		is_deep = false;

	/* Fetch the function from pg_proc */
	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function with OID %u does not exist", funcid)));

	proc = (Form_pg_proc) GETSTRUCT(proctup);
	
	/* Get function name and schema for metadata */
	funcname = NameStr(proc->proname);
	schemaname = get_namespace_name(proc->pronamespace);

	/* Check for SQL body */
	prosqlbody = SysCacheGetAttr(PROCOID, proctup,
								  Anum_pg_proc_prosqlbody, &isnull);

	/* Start building the JSON Schema object */
	pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
	
	/* Add $schema */
	push_json_string(&ps, "$schema", "https://json-schema.org/draft/2020-12/schema");
	
	/* Add title with function signature */
	{
		StringInfoData title;
		initStringInfo(&title);
		if (schemaname)
			appendStringInfo(&title, "%s.%s", schemaname, funcname);
		else
			appendStringInfoString(&title, funcname);
		
		/* Add parameter types for better identification */
		if (proc->pronargs > 0)
		{
			Oid		   *argtypes;
			int			i;
			
			argtypes = (Oid *) palloc(proc->pronargs * sizeof(Oid));
			memcpy(argtypes, proc->proargtypes.values, proc->pronargs * sizeof(Oid));
			
			appendStringInfoChar(&title, '(');
			for (i = 0; i < proc->pronargs; i++)
			{
				if (i > 0)
					appendStringInfoChar(&title, ',');
				appendStringInfoString(&title, format_type_be(argtypes[i]));
			}
			appendStringInfoChar(&title, ')');
			pfree(argtypes);
		}
		else
			appendStringInfoString(&title, "()");
		
		push_json_string(&ps, "title", title.data);
		/* Don't free title.data here - it's still referenced by the jsonb being built */
	}

	if (isnull)
	{
		/* Shallow introspection - no SQL body */
		is_deep = false;
		
		/* Generate schema based on return type */
		if (proc->prorettype == JSONBOID || proc->prorettype == JSONOID)
		{
			schema_obj = schema_for_json_any();
		}
		else if (get_typtype(proc->prorettype) == TYPTYPE_COMPOSITE)
		{
			schema_obj = schema_for_composite(proc->prorettype);
		}
		else
		{
			schema_obj = schema_for_type(proc->prorettype, -1, true);
		}
		
		/* Handle SETOF returns */
		if (proc->proretset && schema_obj)
		{
			schema_obj = wrap_array_items(schema_obj, true);
		}
	}
	else
	{
		/* Deep introspection - analyze SQL body */
		char	   *prosqlbodytext;
		Node	   *n;
		Query	   *lastq = NULL;
		
		is_deep = true;
		
		/* Parse the SQL body */
		prosqlbodytext = TextDatumGetCString(prosqlbody);
		n = stringToNode(prosqlbodytext);
		
		if (IsA(n, List))
		{
			/* BEGIN ATOMIC case: List of Query nodes */
			List	   *stmts = linitial_node(List, (List *) n);
			if (stmts != NIL)
				lastq = llast_node(Query, stmts);
		}
		else
		{
			/* Single statement case */
			lastq = castNode(Query, n);
		}
		
		if (lastq == NULL)
		{
			ReleaseSysCache(proctup);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("malformed SQL-body parse tree for function %u", funcid)));
		}
		
		/* Acquire locks like pg_get_function_sqlbody does */
		AcquireRewriteLocks(lastq, false, false);
		
		/* Generate schema from the query */
		schema_obj = schema_from_query(lastq, proc->prorettype, proc->proretset);
	}
	
	/* Merge the generated schema into the result */
	if (schema_obj)
		merge_jsonb_object(&ps, schema_obj);
	
	/* Add metadata */
	push_json_string(&ps, "x-pg-introspection", is_deep ? "sql-body" : "signature-only");
	push_json_string(&ps, "x-pg-depth", is_deep ? "deep" : "shallow");
	
	/* Add x-pg-version */
	{
		char version_str[32];
		snprintf(version_str, sizeof(version_str), "%d", PG_VERSION_NUM);
		push_json_string(&ps, "x-pg-version", version_str);
	}
	
	res = pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
	
	ReleaseSysCache(proctup);
	
	return JsonbValueToJsonb(res);
}

/*
 * schema_for_type
 *		Generate JSON Schema for a PostgreSQL type
 */
static Jsonb *
schema_for_type(Oid typid, int32 typmod, bool notnull)
{
	JsonbParseState *ps = NULL;
	JsonbValue *res;
	Oid elemtype;
	
	/* Check if this is an array type */
	elemtype = get_element_type(typid);
	if (OidIsValid(elemtype))
	{
		/* This is an array type - generate array schema */
		Jsonb *elem_schema;
		JsonbIterator *it;
		JsonbValue	v;
		JsonbIteratorToken type;
		
		pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
		push_json_string(&ps, "type", "array");
		
		/* Generate schema for the element type */
		elem_schema = schema_for_type(elemtype, -1, true);
		
		/* Add items schema */
		jb_key(&ps, "items");
		
		/* Copy the element schema */
		it = JsonbIteratorInit(&elem_schema->root);
		while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
		{
			if (type == WJB_KEY || type == WJB_VALUE || type == WJB_ELEM)
				pushJsonbValue(&ps, type, &v);
			else
				pushJsonbValue(&ps, type, NULL);
		}
		
		res = pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
		return JsonbValueToJsonb(res);
	}
	
	pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
	
	/* Map PostgreSQL types to JSON Schema types */
	switch (typid)
	{
		case BOOLOID:
			if (notnull)
				push_json_string(&ps, "type", "boolean");
			else
			{
				pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString, 
					.val.string = {.len = 4, .val = "type"}});
				pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 7, .val = "boolean"}});
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "null"}});
				pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
			}
			emit_format(&ps, typid);
			break;
			
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case FLOAT4OID:
		case FLOAT8OID:
			if (notnull)
				push_json_string(&ps, "type", "number");
			else
			{
				pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "type"}});
				pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 6, .val = "number"}});
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "null"}});
				pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
			}
			
			/* Add multipleOf for integers */
			if (typid == INT2OID || typid == INT4OID || typid == INT8OID || typid == OIDOID)
			{
				pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 10, .val = "multipleOf"}});
				pushJsonbValue(&ps, WJB_VALUE, &(JsonbValue){.type = jbvNumeric,
					.val.numeric = DatumGetNumeric(DirectFunctionCall1(int4_numeric, Int32GetDatum(1)))});
			}
			emit_format(&ps, typid);
			break;
			
		case NUMERICOID:
			/* NUMERIC must be string due to special values like NaN, Infinity */
			if (notnull)
				push_json_string(&ps, "type", "string");
			else
			{
				pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "type"}});
				pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 6, .val = "string"}});
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "null"}});
				pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
			}
			emit_format(&ps, typid);
			break;
			
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		case NAMEOID:
		case UUIDOID:
			if (notnull)
				push_json_string(&ps, "type", "string");
			else
			{
				pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "type"}});
				pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 6, .val = "string"}});
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "null"}});
				pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
			}
			emit_format(&ps, typid);
			break;
			
		case DATEOID:
			if (notnull)
				push_json_string(&ps, "type", "string");
			else
			{
				pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "type"}});
				pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 6, .val = "string"}});
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "null"}});
				pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
			}
			emit_format(&ps, typid);
			break;
			
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			if (notnull)
				push_json_string(&ps, "type", "string");
			else
			{
				pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "type"}});
				pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 6, .val = "string"}});
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "null"}});
				pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
			}
			emit_format(&ps, typid);
			break;
			
		case TIMEOID:
		case TIMETZOID:
			if (notnull)
				push_json_string(&ps, "type", "string");
			else
			{
				pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "type"}});
				pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 6, .val = "string"}});
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "null"}});
				pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
			}
			emit_format(&ps, typid);
			break;
			
		case BYTEAOID:
			if (notnull)
				push_json_string(&ps, "type", "string");
			else
			{
				pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "type"}});
				pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 6, .val = "string"}});
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "null"}});
				pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
			}
			push_json_string(&ps, "contentEncoding", "base64");
			emit_format(&ps, typid);
			break;
			
		case JSONOID:
		case JSONBOID:
			/* For JSON/JSONB types, emit string with format */
			if (notnull)
				push_json_string(&ps, "type", "string");
			else
			{
				pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "type"}});
				pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 6, .val = "string"}});
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "null"}});
				pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
			}
			emit_format(&ps, typid);
			break;
			
		default:
			/* Check if it's a composite type */
			if (get_typtype(typid) == TYPTYPE_COMPOSITE)
			{
				return schema_for_composite(typid);
			}
			/* Default to string for unknown types */
			if (notnull)
				push_json_string(&ps, "type", "string");
			else
			{
				pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "type"}});
				pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 6, .val = "string"}});
				pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
					.val.string = {.len = 4, .val = "null"}});
				pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
			}
			emit_format(&ps, typid);
			break;
	}
	
	res = pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
	return JsonbValueToJsonb(res);
}

/*
 * schema_for_composite
 *		Generate JSON Schema for a composite type
 */
static Jsonb *
schema_for_composite(Oid typid)
{
	TupleDesc	tupdesc;
	JsonbParseState *ps = NULL;
	JsonbValue *res;
	int			i;
	
	/* Get the tuple descriptor for the composite type */
	tupdesc = lookup_rowtype_tupdesc(typid, -1);
	
	pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
	push_json_string(&ps, "type", "object");
	
	/* Build properties for each attribute */
	pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
		.val.string = {.len = 10, .val = "properties"}});
	pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
	
	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Jsonb	   *prop_schema;
		
		if (att->attisdropped)
			continue;
		
		/* Add property name as key */
		pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
			.val.string = {.len = strlen(NameStr(att->attname)), 
						   .val = NameStr(att->attname)}});
		
		/* Generate schema for this attribute */
		prop_schema = schema_for_type(att->atttypid, att->atttypmod, att->attnotnull);
		
		/* Add the schema as value */
		if (prop_schema)
		{
			JsonbIterator *it;
			JsonbValue	v;
			JsonbIteratorToken type;
			
			it = JsonbIteratorInit(&prop_schema->root);
			while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
			{
				if (type == WJB_KEY || type == WJB_VALUE || type == WJB_ELEM)
					pushJsonbValue(&ps, type, &v);
				else
					pushJsonbValue(&ps, type, NULL);
			}
		}
	}
	
	pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
	
	/* Add required array for NOT NULL attributes */
	pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
		.val.string = {.len = 8, .val = "required"}});
	pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);
	
	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		
		if (att->attisdropped)
			continue;
		
		if (att->attnotnull)
		{
			pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
				.val.string = {.len = strlen(NameStr(att->attname)),
							   .val = NameStr(att->attname)}});
		}
	}
	
	pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
	
	push_json_bool(&ps, "additionalProperties", false);
	
	res = pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
	
	ReleaseTupleDesc(tupdesc);
	
	return JsonbValueToJsonb(res);
}

/*
 * schema_for_json_any
 *		Generate JSON Schema for unstructured JSON
 */
static Jsonb *
schema_for_json_any(void)
{
	JsonbParseState *ps = NULL;
	JsonbValue *res;
	
	pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
	
	/* Type can be any JSON type */
	pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
		.val.string = {.len = 4, .val = "type"}});
	pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);
	pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
		.val.string = {.len = 6, .val = "object"}});
	pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
		.val.string = {.len = 5, .val = "array"}});
	pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
		.val.string = {.len = 6, .val = "string"}});
	pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
		.val.string = {.len = 6, .val = "number"}});
	pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
		.val.string = {.len = 7, .val = "boolean"}});
	pushJsonbValue(&ps, WJB_ELEM, &(JsonbValue){.type = jbvString,
		.val.string = {.len = 4, .val = "null"}});
	pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
	
	res = pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
	return JsonbValueToJsonb(res);
}

/*
 * wrap_array_items
 *		Wrap a schema as array items for SETOF returns
 */
static Jsonb *
wrap_array_items(Jsonb *item_schema, bool is_setof)
{
	JsonbParseState *ps = NULL;
	JsonbValue *res;
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken type;
	
	pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
	push_json_string(&ps, "type", "array");
	
	/* Add items schema */
	pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
		.val.string = {.len = 5, .val = "items"}});
	
	/* Copy the item schema */
	it = JsonbIteratorInit(&item_schema->root);
	while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		if (type == WJB_KEY || type == WJB_VALUE || type == WJB_ELEM)
			pushJsonbValue(&ps, type, &v);
		else
			pushJsonbValue(&ps, type, NULL);
	}
	
	/* Add x-pg-returns metadata */
	if (is_setof)
		push_json_string(&ps, "x-pg-returns", "setof");
	
	res = pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
	return JsonbValueToJsonb(res);
}

/*
 * schema_from_query
 *		Generate JSON Schema from a Query parse tree
 */
static Jsonb *
schema_from_query(Query *query, Oid prorettype, bool proretset)
{
	JsonbParseState *ps = NULL;
	JsonbValue *res;
	ListCell   *lc;
	Jsonb	   *row_schema = NULL;
	List *targetList;
	
	/* Handle different query types */
	if (query->commandType != CMD_SELECT && !query->returningList)
	{
		/* DML without RETURNING - shouldn't happen for functions */
		return schema_for_type(prorettype, -1, false);
	}
	
	/* Get the target list */
	
	targetList = query->commandType == CMD_SELECT ? 
				  query->targetList : query->returningList;
	
	if (targetList == NIL)
	{
		return schema_for_type(prorettype, -1, false);
	}
	
	/* Check if return type is composite */
	if (get_typtype(prorettype) == TYPTYPE_COMPOSITE)
	{
		/* Build object schema from target entries */
		pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
		push_json_string(&ps, "type", "object");
		
		pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
			.val.string = {.len = 10, .val = "properties"}});
		pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
		
		foreach(lc, targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			Jsonb	   *field_schema;
			char *fieldname;
			
			if (tle->resjunk)
				continue;
			
			/* Get field name */
			fieldname = tle->resname ? tle->resname : "?column?";
			
			pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
				.val.string = {.len = strlen(fieldname), .val = fieldname}});
			
			/* Analyze the expression */
			field_schema = schema_from_expr((Node *) tle->expr, query);
			
			if (field_schema)
			{
				JsonbIterator *it;
				JsonbValue	v;
				JsonbIteratorToken type;
				
				it = JsonbIteratorInit(&field_schema->root);
				while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
				{
					if (type == WJB_KEY || type == WJB_VALUE || type == WJB_ELEM)
						pushJsonbValue(&ps, type, &v);
					else
						pushJsonbValue(&ps, type, NULL);
				}
			}
			else
			{
				/* Default to string if we can't determine */
				pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
				push_json_string(&ps, "type", "string");
				pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
			}
		}
		
		pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
		push_json_bool(&ps, "additionalProperties", false);
		
		res = pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
		row_schema = JsonbValueToJsonb(res);
	}
	else
	{
		/* Scalar return - analyze first target entry */
		TargetEntry *tle = (TargetEntry *) linitial(targetList);
		row_schema = schema_from_expr((Node *) tle->expr, query);
		
		if (!row_schema)
			row_schema = schema_for_type(prorettype, -1, false);
	}
	
	/* Wrap in array if SETOF */
	if (proretset)
		return wrap_array_items(row_schema, true);
	
	return row_schema;
}

/*
 * schema_from_expr
 *		Generate JSON Schema from an expression node
 */
static Jsonb *
schema_from_expr(Node *expr, Query *query)
{
	if (expr == NULL)
		return NULL;
	
	switch (nodeTag(expr))
	{
		case T_Const:
			{
				Const *con = (Const *) expr;
				return schema_for_type(con->consttype, con->consttypmod, !con->constisnull);
			}
			
		case T_Var:
			{
				Var *var = (Var *) expr;
				bool notnull = false;
				
				/* Try to determine if this column has NOT NULL constraint */
				if (query && var->varno > 0 && var->varno <= list_length(query->rtable))
				{
					RangeTblEntry *rte = rt_fetch(var->varno, query->rtable);
					if (rte->rtekind == RTE_RELATION && var->varattno > 0)
					{
						/* Look up the column's NOT NULL constraint */
						HeapTuple tp;
						tp = SearchSysCache2(ATTNUM,
											  ObjectIdGetDatum(rte->relid),
											  Int16GetDatum(var->varattno));
						if (HeapTupleIsValid(tp))
						{
							Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(tp);
							notnull = att->attnotnull;
							ReleaseSysCache(tp);
						}
					}
				}
				return schema_for_type(var->vartype, var->vartypmod, notnull);
			}
			
		case T_FuncExpr:
			return schema_from_funcexpr((FuncExpr *) expr, query);
			
		case T_Aggref:
			return schema_from_aggref((Aggref *) expr, query);
			
		case T_CoalesceExpr:
			{
				CoalesceExpr *coalesce = (CoalesceExpr *) expr;
				/* Return schema of first arg (COALESCE returns first non-null) */
				if (coalesce->args != NIL)
					return schema_from_expr((Node *) linitial(coalesce->args), query);
				return schema_for_type(coalesce->coalescetype, -1, false);
			}
			
		case T_CaseExpr:
			{
				CaseExpr *caseexpr = (CaseExpr *) expr;
				/* For now, return schema based on result type */
				/* TODO: Could implement anyOf for different WHEN branches */
				return schema_for_type(caseexpr->casetype, -1, false);
			}
			
		case T_RelabelType:
			{
				RelabelType *relabel = (RelabelType *) expr;
				return schema_from_expr((Node *) relabel->arg, query);
			}
			
		case T_CoerceToDomain:
			{
				CoerceToDomain *coerce = (CoerceToDomain *) expr;
				return schema_for_type(coerce->resulttype, coerce->resulttypmod, false);
			}
			
		case T_SubLink:
			{
				SubLink *sublink = (SubLink *) expr;
				Oid sublink_type;
				
				/* SubLink represents a subquery - analyze its result type */
				/* For scalar subqueries, we can try to determine the type from the subselect */
				if (sublink->subLinkType == EXPR_SUBLINK && sublink->subselect)
				{
					Query *subquery = castNode(Query, sublink->subselect);
					if (subquery->targetList && list_length(subquery->targetList) == 1)
					{
						TargetEntry *tle = (TargetEntry *) linitial(subquery->targetList);
						return schema_from_expr((Node *) tle->expr, subquery);
					}
				}
				/* Try to get the type from exprType */
				sublink_type = exprType((Node *) sublink);
				if (OidIsValid(sublink_type))
					return schema_for_type(sublink_type, -1, false);
				/* Default fallback */
				return NULL;
			}
			
		default:
			/* For unknown expression types, try to get the type if possible */
			{
				Oid exprtype = exprType(expr);
				if (OidIsValid(exprtype))
					return schema_for_type(exprtype, -1, false);
			}
			break;
	}
	
	return NULL;
}

/*
 * schema_from_funcexpr
 *		Generate JSON Schema from a FuncExpr, handling JSON constructors specially
 */
static Jsonb *
schema_from_funcexpr(FuncExpr *func, Query *query)
{
	Oid			funcid = func->funcid;
	char	   *funcname;
	JsonbParseState *ps = NULL;
	JsonbValue *res;
	
	/* Get function name for identification */
	funcname = get_func_name(funcid);
	if (!funcname)
		return schema_for_type(func->funcresulttype, -1, false);
	
	/* Handle JSON constructor functions */
	if (strcmp(funcname, "jsonb_build_object") == 0 ||
		strcmp(funcname, "json_build_object") == 0)
	{
		/* Analyze jsonb_build_object arguments */
		ListCell   *lc;
		bool		all_keys_const = true;
		List	   *keys = NIL;
		List	   *values = NIL;
		int			i = 0;
		
		/* Extract keys and values (alternating args) */
		foreach(lc, func->args)
		{
			Node *arg = (Node *) lfirst(lc);
			if (i % 2 == 0)  /* Key */
			{
				if (!IsA(arg, Const))
					all_keys_const = false;
				keys = lappend(keys, arg);
			}
			else  /* Value */
			{
				values = lappend(values, arg);
			}
			i++;
		}
		
		pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
		push_json_string(&ps, "type", "object");
		
		if (all_keys_const && keys != NIL)
		{
			/* Build precise properties */
			ListCell   *kc, *vc;
			
			pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
				.val.string = {.len = 10, .val = "properties"}});
			pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
			
			forboth(kc, keys, vc, values)
			{
				Const *key_const = (Const *) lfirst(kc);
				Node *value_expr = (Node *) lfirst(vc);
				Jsonb *value_schema;
				
				if (!key_const->constisnull)
				{
					char *keystr;
					
					/* Extract key string based on type */
					if (key_const->consttype == TEXTOID)
					{
						text *key_text = DatumGetTextP(key_const->constvalue);
						keystr = text_to_cstring(key_text);
					}
					else
					{
						/* Convert to string if not text */
						Oid			typoutput;
						bool		typIsVarlena;
						
						getTypeOutputInfo(key_const->consttype, &typoutput, &typIsVarlena);
						keystr = OidOutputFunctionCall(typoutput, key_const->constvalue);
					}
					
					pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
						.val.string = {.len = strlen(keystr), .val = keystr}});
					
					value_schema = schema_from_expr(value_expr, query);
					if (value_schema)
					{
						JsonbIterator *it;
						JsonbValue	v;
						JsonbIteratorToken type;
						
						it = JsonbIteratorInit(&value_schema->root);
						while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
						{
							if (type == WJB_KEY || type == WJB_VALUE || type == WJB_ELEM)
								pushJsonbValue(&ps, type, &v);
							else
								pushJsonbValue(&ps, type, NULL);
						}
					}
					else
					{
						/* Default schema */
						pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
						push_json_string(&ps, "type", "string");
						pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
					}
				}
			}
			
			pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
			push_json_bool(&ps, "additionalProperties", false);
		}
		else
		{
			/* Dynamic keys - use patternProperties */
			Jsonb *any_schema_obj;
			JsonbIterator *it;
			JsonbValue	v;
			JsonbIteratorToken type;
			
			pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
				.val.string = {.len = 17, .val = "patternProperties"}});
			pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
			
			pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
				.val.string = {.len = 2, .val = ".*"}});
			
			/* Schema for values - could be union of all value schemas */
			/* For simplicity, use generic JSON schema */
			any_schema_obj = schema_for_json_any();
			it = JsonbIteratorInit(&any_schema_obj->root);
			
			while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
			{
				if (type == WJB_KEY || type == WJB_VALUE || type == WJB_ELEM)
					pushJsonbValue(&ps, type, &v);
				else
					pushJsonbValue(&ps, type, NULL);
			}
			
			pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
			push_json_bool(&ps, "additionalProperties", false);
		}
		
		res = pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
		return JsonbValueToJsonb(res);
	}
	else if (strcmp(funcname, "jsonb_build_array") == 0 ||
			 strcmp(funcname, "json_build_array") == 0)
	{
		/* Build array schema with prefixItems */
		ListCell   *lc;
		
		pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
		push_json_string(&ps, "type", "array");
		
		if (func->args != NIL)
		{
			pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
				.val.string = {.len = 11, .val = "prefixItems"}});
			pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);
			
			foreach(lc, func->args)
			{
				Node *arg = (Node *) lfirst(lc);
				Jsonb *item_schema = schema_from_expr(arg, query);
				
				if (item_schema)
				{
					JsonbIterator *it;
					JsonbValue	v;
					JsonbIteratorToken type;
					
					it = JsonbIteratorInit(&item_schema->root);
					while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
					{
						if (type == WJB_KEY || type == WJB_VALUE || type == WJB_ELEM)
							pushJsonbValue(&ps, type, &v);
						else if (type == WJB_BEGIN_OBJECT || type == WJB_END_OBJECT ||
								 type == WJB_BEGIN_ARRAY || type == WJB_END_ARRAY)
							pushJsonbValue(&ps, type, NULL);
					}
				}
			}
			
			pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
			push_json_bool(&ps, "items", false);
		}
		
		res = pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
		return JsonbValueToJsonb(res);
	}
	else if (strcmp(funcname, "to_json") == 0 ||
			 strcmp(funcname, "to_jsonb") == 0 ||
			 strcmp(funcname, "row_to_json") == 0)
	{
		/* These convert their input to JSON */
		if (func->args != NIL)
		{
			Node *arg = (Node *) linitial(func->args);
			Oid argtype = exprType(arg);
			
			if (get_typtype(argtype) == TYPTYPE_COMPOSITE)
				return schema_for_composite(argtype);
		}
		return schema_for_json_any();
	}
	
	/* Default: return schema based on function result type */
	return schema_for_type(func->funcresulttype, -1, false);
}

/*
 * schema_from_aggref
 *		Generate JSON Schema from an Aggref, handling JSON aggregates specially
 */
static Jsonb *
schema_from_aggref(Aggref *agg, Query *query)
{
	char	   *aggname;
	JsonbParseState *ps = NULL;
	JsonbValue *res;
	
	/* Get aggregate name */
	aggname = get_func_name(agg->aggfnoid);
	if (!aggname)
		return schema_for_type(agg->aggtype, -1, false);
	
	/* Handle JSON aggregate functions */
	if (strcmp(aggname, "jsonb_agg") == 0 ||
		strcmp(aggname, "json_agg") == 0)
	{
		/* Array of aggregated values */
		Jsonb *item_schema = NULL;
		
		if (agg->args != NIL)
		{
			TargetEntry *arg = (TargetEntry *) linitial(agg->args);
			item_schema = schema_from_expr((Node *) arg->expr, query);
		}
		
		if (!item_schema)
			item_schema = schema_for_json_any();
		
		pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
		push_json_string(&ps, "type", "array");
		
		pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
			.val.string = {.len = 5, .val = "items"}});
		
		/* Add item schema */
		{
			JsonbIterator *it = JsonbIteratorInit(&item_schema->root);
			JsonbValue	v;
			JsonbIteratorToken type;
		
			while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
			{
				if (type == WJB_KEY || type == WJB_VALUE || type == WJB_ELEM)
					pushJsonbValue(&ps, type, &v);
				else
					pushJsonbValue(&ps, type, NULL);
			}
		}
		
		res = pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
		return JsonbValueToJsonb(res);
	}
	else if (strcmp(aggname, "jsonb_object_agg") == 0 ||
			 strcmp(aggname, "json_object_agg") == 0)
	{
		/* Object with dynamic keys */
		Jsonb *value_schema = NULL;
		
		if (list_length(agg->args) >= 2)
		{
			/* Second argument is the value */
			TargetEntry *value_arg = (TargetEntry *) lsecond(agg->args);
			value_schema = schema_from_expr((Node *) value_arg->expr, query);
		}
		
		if (!value_schema)
			value_schema = schema_for_json_any();
		
		pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
		push_json_string(&ps, "type", "object");
		
		pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
			.val.string = {.len = 17, .val = "patternProperties"}});
		pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
		
		pushJsonbValue(&ps, WJB_KEY, &(JsonbValue){.type = jbvString,
			.val.string = {.len = 2, .val = ".*"}});
		
		/* Add value schema */
		{
			JsonbIterator *it = JsonbIteratorInit(&value_schema->root);
			JsonbValue	v;
			JsonbIteratorToken type;
			
			while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
			{
				if (type == WJB_KEY || type == WJB_VALUE || type == WJB_ELEM)
					pushJsonbValue(&ps, type, &v);
				else
					pushJsonbValue(&ps, type, NULL);
			}
		}
		
		pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
		push_json_bool(&ps, "additionalProperties", false);
		
		res = pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
		return JsonbValueToJsonb(res);
	}
	
	/* Default: return schema based on aggregate result type */
	return schema_for_type(agg->aggtype, -1, false);
}

/*
 * Helper functions for building JSON
 */
static void
push_json_string(JsonbParseState **ps, const char *key, const char *value)
{
	JsonbValue	jbvKey, jbvVal;
	
	jbvKey.type = jbvString;
	jbvKey.val.string.len = strlen(key);
	jbvKey.val.string.val = (char *) key;
	
	jbvVal.type = jbvString;
	jbvVal.val.string.len = strlen(value);
	jbvVal.val.string.val = (char *) value;
	
	pushJsonbValue(ps, WJB_KEY, &jbvKey);
	pushJsonbValue(ps, WJB_VALUE, &jbvVal);
}

static void
push_json_bool(JsonbParseState **ps, const char *key, bool value)
{
	JsonbValue	jbvKey, jbvVal;
	
	jbvKey.type = jbvString;
	jbvKey.val.string.len = strlen(key);
	jbvKey.val.string.val = (char *) key;
	
	jbvVal.type = jbvBool;
	jbvVal.val.boolean = value;
	
	pushJsonbValue(ps, WJB_KEY, &jbvKey);
	pushJsonbValue(ps, WJB_VALUE, &jbvVal);
}

static void
merge_jsonb_object(JsonbParseState **ps, Jsonb *obj)
{
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken type;
	int			level = 0;
	
	/* Safety check for NULL input */
	if (!obj)
		return;
	
	it = JsonbIteratorInit(&obj->root);
	
	while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		if (type == WJB_BEGIN_OBJECT)
		{
			level++;
			if (level > 1)  /* Only push nested BEGIN_OBJECT */
				pushJsonbValue(ps, type, NULL);
		}
		else if (type == WJB_END_OBJECT)
		{
			level--;
			if (level > 0)  /* Only push nested END_OBJECT */
				pushJsonbValue(ps, type, NULL);
		}
		else if (type == WJB_BEGIN_ARRAY || type == WJB_END_ARRAY)
		{
			/* Pass array markers through */
			pushJsonbValue(ps, type, NULL);
		}
		else if (type == WJB_KEY || type == WJB_VALUE || type == WJB_ELEM)
		{
			/* Pass keys, values, and array elements through */
			pushJsonbValue(ps, type, &v);
		}
	}
}

/*
 * jb_key
 *		Convenience function to add a JSON key
 */
static inline void
jb_key(JsonbParseState **ps, const char *key)
{
	JsonbValue	jbvKey;
	
	jbvKey.type = jbvString;
	jbvKey.val.string.len = strlen(key);
	jbvKey.val.string.val = (char *) key;
	
	pushJsonbValue(ps, WJB_KEY, &jbvKey);
}

/*
 * jb_string
 *		Convenience function to add a JSON string value
 */
static inline void
jb_string(JsonbParseState **ps, const char *value)
{
	JsonbValue	jbvVal;
	
	jbvVal.type = jbvString;
	jbvVal.val.string.len = strlen(value);
	jbvVal.val.string.val = (char *) value;
	
	pushJsonbValue(ps, WJB_VALUE, &jbvVal);
}

/*
 * emit_format
 *		Emit "format" field with PostgreSQL type name (as shown by pg_typeof())
 */
static inline void
emit_format(JsonbParseState **ps, Oid typid)
{
	jb_key(ps, "format");
	jb_string(ps, format_type_be(typid));
}