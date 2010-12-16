#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

#include "SparseData.h"

/*------------------------------------------------------------------------------
 * Create a SparseData structure
 *
 * There are ways to do this:
 * 	First way allocates empty dynamic StringInfo of unknown initial sizes
 * 	Second way creates a copy of an existing SparseData
 * 	Third way creates a SparseData with zero storage in its StringInfos
 * 	Fourth way creates a SparseData in place using pointers to the vals and
 * 		index data
 */
SparseData makeSparseData(void)
{
	/* Allocate the struct */
	SparseData sdata = (SparseData)palloc(sizeof(SparseDataStruct));
	/* Allocate the included elements */
	sdata->vals  = makeStringInfo();
	sdata->index = makeStringInfo();
	sdata->vals->len = 0;
	sdata->index->len = 0;
	sdata->vals->cursor = 0;
	sdata->index->cursor = 0;
	sdata->unique_value_count=0;
	sdata->total_value_count=0;
	sdata->type_of_data = FLOAT8OID;
	return sdata;
}
SparseData makeEmptySparseData(void)
{
	/* Allocate the struct */
	SparseData sdata = makeSparseData();
	/*
	 * Free the data area
	 */
	pfree(sdata->vals->data);
	pfree(sdata->index->data);
	sdata->vals->data  = palloc(1);
	sdata->index->data = palloc(1);
	sdata->vals->maxlen=0;
	sdata->index->maxlen=0;
	return sdata;
}
SparseData makeInplaceSparseData(char *vals, char *index,
		int datasize, int indexsize, Oid datatype,
		int unique_value_count, int total_value_count)
{
	SparseData sdata = makeEmptySparseData();
	sdata->unique_value_count = unique_value_count;
	sdata->total_value_count  = total_value_count;
	sdata->vals->data = vals;
	sdata->vals->len = datasize;
	sdata->vals->maxlen = sdata->vals->len;
	sdata->index->data = index;
	sdata->index->len = indexsize;
	sdata->index->maxlen = sdata->index->len;
	sdata->type_of_data = datatype;
	return sdata;
}
/*------------------------------------------------------------------------------
 * Copy a SparseData structure including its data and index
 */
SparseData makeSparseDataCopy(SparseData source_sdata)
{
	/* Allocate the struct */
	SparseData sdata = (SparseData)palloc(sizeof(SparseDataStruct));
	/* Allocate the included elements */
	sdata->vals = copyStringInfo(source_sdata->vals);
	sdata->index = copyStringInfo(source_sdata->index);
	sdata->type_of_data       = source_sdata->type_of_data;
	sdata->unique_value_count = source_sdata->unique_value_count;
	sdata->total_value_count  = source_sdata->total_value_count;
	return sdata;
}
/*
 * Create a SparseData of size dimension from a constant
 */
SparseData makeSparseDataFromDouble(double constant,int64 dimension)
{
	char *bytestore=(char *)palloc(sizeof(char)*9);
	SparseData sdata = float8arr_to_sdata(&constant,1);
	int8_to_compword(dimension,bytestore); /* create compressed version of
					          int8 value */
	pfree(sdata->index->data);
	sdata->index->data = bytestore;
	sdata->index->len = int8compstoragesize(bytestore);
	sdata->total_value_count=dimension;
	if (sdata->index->maxlen < int8compstoragesize(bytestore)) {
		ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			       errOmitLocation(true),
			       errmsg("Internal error")));
	}
	return sdata;
}

void freeSparseData(SparseData sdata)
{
	pfree(sdata->vals);
	pfree(sdata->index);
	pfree(sdata);
}

void freeSparseDataAndData(SparseData sdata)
{
	pfree(sdata->vals->data);
	pfree(sdata->index->data);
	freeSparseData(sdata);
}

/*------------------------------------------------------------------------------
 * Copy a StringInfo structure
 */
StringInfo copyStringInfo(StringInfo sinfo) {
	StringInfo result;
	char *data;
	if (sinfo->data == NULL) {
		data = NULL;
	} else {
		data   = (char *)palloc(sizeof(char)*(sinfo->len+1));
		memcpy(data,sinfo->data,sinfo->len);
		data[sinfo->len] = '\0';
	}
	result = makeStringInfoFromData(data,sinfo->len);
	return result;
}
/*
 * make a StringInfo from a data pointer and length
 */
StringInfo makeStringInfoFromData(char *data,int len) {
	StringInfo sinfo;
	sinfo = (StringInfo)palloc(sizeof(StringInfoData));
	sinfo->data   = data;
	sinfo->len    = len;
	sinfo->maxlen = len;
	sinfo->cursor = 0;
	return sinfo;
}
/*------------------------------------------------------------------------------
 * Convert a double[] to a SparseData compressed format, representing duplicate
 * values in the double[] by a count and a single value.
 * Note that special double values like denormalized numbers and exceptions
 * like NaN are treated like any other value - if there are duplicates, the
 * value of the special number is preserved and they are counted.
 *------------------------------------------------------------------------------
 */
/* -- KS: remove original code, now calling arr_to_sdata */
SparseData float8arr_to_sdata(double *array, int count) {
	return arr_to_sdata((char *)array, sizeof(float8), FLOAT8OID, count);
}

/* -- KS: corrected char *curr_val=array+i step  */
#include <assert.h>
SparseData arr_to_sdata(char *array, size_t width, Oid type_of_data, int count) {
	char *run_val=array;
	int64 run_len=1;
	SparseData sdata = makeSparseData();

	sdata->type_of_data=type_of_data;

	for (int i=1; i<count; i++) {
		char *curr_val=array+ (i*size_of_type(type_of_data));
			if (memcmp(curr_val,run_val,width))
			{ /*run is interrupted, initiate new run */
				/* package up the finished run */
				add_run_to_sdata(run_val,run_len,width,sdata);
				/* mark beginning of new run */
				run_val=curr_val;
				run_len=1;
			} else 
			{ /* we're still in the same run */
				run_len++;
			}
	}
	add_run_to_sdata(run_val, run_len, width, sdata); /* package up the last run */

	/* Add the final tallies */
	sdata->unique_value_count = sdata->vals->len/width;
	sdata->total_value_count = count;

	return sdata;
}

/*------------------------------------------------------------------------------
 * Convert a SparseData compressed structure to a float8[]
 *------------------------------------------------------------------------------
 */
double *sdata_to_float8arr(SparseData sdata) {
	double *array;
	int j, aptr;
	char *iptr;

	if (sdata->type_of_data != FLOAT8OID)
	{
		ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			errOmitLocation(true),
			errmsg("Data type of SparseData is not FLOAT64\n")));
	}

	if ((array = (double *)palloc(sizeof(double)*(sdata->total_value_count)))
			== NULL)
	{
		ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			       errOmitLocation(true),
			       errmsg("Error allocating memory for array\n")));
	}

	iptr = sdata->index->data;
	aptr = 0;
	for (int i=0; i<sdata->unique_value_count; i++) {
		for (j=0;j<compword_to_int8(iptr);j++,aptr++) {
			array[aptr] = ((double *)(sdata->vals->data))[i];
		}
		iptr+=int8compstoragesize(iptr);
	}

	if ((aptr) != sdata->total_value_count) 
	{
		ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		 errOmitLocation(true),
		 errmsg("Array size is incorrect, is: %d and should be %d\n",
				aptr,sdata->total_value_count)));

		pfree(array);
		return NULL;
	}

	return array;
}

int64 *sdata_index_to_int64arr(SparseData sdata)
{
	char *iptr;
	int64 *array_ix = (int64 *)palloc(
			sizeof(int64)*(sdata->unique_value_count));

	iptr = sdata->index->data;
	for (int i=0; i<sdata->unique_value_count; i++,
			iptr+=int8compstoragesize(iptr)) {
		array_ix[i] = compword_to_int8(iptr);
	}
	return(array_ix);
}

void serializeSparseData(char *target, SparseData source)
{
	/* SparseDataStruct header */
	memcpy(target,source,SIZEOF_SPARSEDATAHDR);
	/* Two StringInfo structures describing the data and index */
	memcpy(SDATA_DATA_SINFO(target), source->vals,sizeof(StringInfoData));
	memcpy(SDATA_INDEX_SINFO(target),source->index,sizeof(StringInfoData));
	/* The unique data values */
	memcpy(SDATA_VALS_PTR(target),source->vals->data,source->vals->maxlen);
	/* The index values */
	memcpy(SDATA_INDEX_PTR(target),source->index->data,source->index->maxlen);

	/*
	 * Set pointers to the data areas of the serialized structure
	 * First the two StringInfo structures contained in the SparseData,
	 * then the data areas inside each of the two StringInfos.
	 */
	((SparseData)target)->vals = (StringInfo)SDATA_DATA_SINFO(target);
	((SparseData)target)->index = (StringInfo)SDATA_INDEX_SINFO(target);
	((StringInfo)(SDATA_DATA_SINFO(target)))->data  = SDATA_VALS_PTR(target);
	if (source->index->data != NULL)
	{
		((StringInfo)(SDATA_INDEX_SINFO(target)))->data = SDATA_INDEX_PTR(target);
	} else
	{
		((StringInfo)(SDATA_INDEX_SINFO(target)))->data = NULL;
	}
}

#ifdef NOT
//TODO
SparseData deserializeSparseData(char *source)
{
	SparseData sdata = makeEmptySparseData();
	char *target = (char *)sdata;
	int index_size,vals_size;
	/* Unpack into an empty SparseDataStructure and fill the empty 
	 * StringInfo structures with data
	 */
	/* SparseDataStruct header */
	memcpy(target,source,SIZEOF_SPARSEDATAHDR);
	target+=SIZEOF_SPARSEDATAHDR;

	index_size = SDATA_INDEX_SIZE(source);
	vals_size = SDATA_DATA_SIZE(source);

	sdata->vals->data  = (char *)palloc(vals_size);
	sdata->index->data = (char *)palloc(index_size);

	sdata->vals->len = sdata->unique_value_count;
	sdata->vals->maxlen = sdata->unique_value_count;
	memcpy(sdata->vals->data,SDATA_VALS_PTR(source),vals_size);

	sdata->index->len = index_size;
	sdata->index->maxlen = sdata->index->len;
	memcpy(sdata->index->data,SDATA_INDEX_PTR(source),index_size);

	return(sdata);
}
#endif

void printSparseData(SparseData sdata);
void printSparseData(SparseData sdata) {
	int value_count = sdata->unique_value_count;
	{
		char *indexdata = sdata->index->data;
		double *values  = (double *)(sdata->vals->data);
		for (int i=0; i<value_count; i++) {
			printf("run_length[%d] = %lld, ",i,(long long int)compword_to_int8(indexdata));
			printf("value[%d] = %f\n",i,values[i]);
			indexdata+=int8compstoragesize(indexdata);
		}
	}
}

/* -- KS */
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "parser/parse_func.h"
#include "access/htup.h"
#include "catalog/pg_proc.h"

static bool lapply_error_checking(Oid foid, List * funcname);

/* This function applies an input function on all elements of a sparse data. 
 * The function is modelled after the corresponding function in R.
 */
SparseData lapply(text * func, SparseData sdata) {
	Oid argtypes[1] = { FLOAT8OID };
	List * funcname = textToQualifiedNameList(func);
	SparseData result = makeSparseDataCopy(sdata);
	Oid foid = LookupFuncName(funcname, 1, argtypes, false);

	lapply_error_checking(foid, funcname);

	for (int i=0; i<sdata->unique_value_count; i++)
		valref(float8,result,i) = 
		    DatumGetFloat8(
		      OidFunctionCall1(foid,
				    Float8GetDatum(valref(float8,sdata,i))));
	return result;
}

static bool lapply_error_checking(Oid foid, List * func) {
	/* foid != InvalidOid; otherwise LookupFuncName would raise error.
	   Here we check that the return type of foid is float8. */
	HeapTuple ftup = SearchSysCache(PROCOID, 
					ObjectIdGetDatum(foid), 0, 0, 0);
	Form_pg_proc pform = (Form_pg_proc) GETSTRUCT(ftup);

	if (pform->prorettype != FLOAT8OID)
		ereport(ERROR, 
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("return type of %s is not double", 
				NameListToString(func)))); 

	// check volatility

	ReleaseSysCache(ftup);
	return true;
}

/* This function projects onto an element of a sparse data. As usual, we 
 * start counting from zero. So idx = 0 gives the first element. 
 */
double sd_proj(SparseData sdata, int idx) {
	char * ix = sdata->index->data;
	double * vals = (double *)sdata->vals->data;
	float8 ret;
	int read, i;

	// error checking
	if (0 > idx || idx >= sdata->total_value_count)
		ereport(ERROR, 
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errOmitLocation(true),
			 errmsg("Index out of bounds.")));

	// find desired block
	read = compword_to_int8(ix) - 1;
	i = 0;
	while (read < idx) {
		ix += int8compstoragesize(ix);
		read += compword_to_int8(ix);
		i++;
	}
	return vals[i];
}

/* This function extracts a sub array, indexed by start and end, of a sparse 
 * data. The indices begin at zero.
 */
SparseData subarr(SparseData sdata, int start, int end) {
	char * ix = sdata->index->data;
	double * vals = (double *)sdata->vals->data;
	SparseData ret = makeSparseData();
	size_t wf8 = sizeof(float8);
	
	if (start > end) 
		return reverse(subarr(sdata,end,start));

	// error checking
	if (0 > start || start > end || end >= sdata->total_value_count)
		ereport(ERROR, 
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errOmitLocation(true),
			 errmsg("Array index out of bounds.")));

	// find start block
	int read = compword_to_int8(ix) - 1;
	int i = 0;
	while (read < start) {
		ix += int8compstoragesize(ix);
		read += compword_to_int8(ix);
		i++;
	}
	if (end <= read) {
		// the whole subarray is in the first block, we are done
		add_run_to_sdata((char *)(&vals[i]), end-start+1, wf8, ret);
		return ret;
	}
	// else start building subarray
	add_run_to_sdata((char *)(&vals[i]), read-start+1, wf8, ret);

	for (int j=i+1; j<sdata->unique_value_count; j++) {
		ix += int8compstoragesize(ix);
		int esize = compword_to_int8(ix);
		if (read + esize > end) {
			add_run_to_sdata((char *)(&vals[j]), end-read, wf8,ret);
			break;
		} 
		add_run_to_sdata((char *)(&vals[j]), esize, wf8, ret);
		read += esize;
	}
	return ret;
}

SparseData reverse(SparseData sdata) {
	char * ix = sdata->index->data;
	double * vals = (double *)sdata->vals->data;
	SparseData ret = makeSparseData();
	size_t w = sizeof(float8);

	// move to the last count array element
	for (int j=0; j<sdata->unique_value_count-1; j++)
		ix += int8compstoragesize(ix);

	// copy from right to left
	for (int j=sdata->unique_value_count-1; j!=-1; j--) {
		add_run_to_sdata((char *)(&vals[j]),compword_to_int8(ix),w,ret);
		ix -= int8compstoragesize(ix);
	}
	return ret;
}

/* -- KS: 
  general procedure
  - write function for SparseData
  - wrap it up for svecs in operators.c or sparse_vectors.c
  - make it available in gp_svec.sql
*/


