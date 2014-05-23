/*
 * opencl_gpusort.h
 *
 * Sort logic accelerated by OpenCL devices
 * --
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This software is an extension of PostgreSQL; You can use, copy,
 * modify or distribute it under the terms of 'LICENSE' included
 * within this package.
 */
#ifndef OPENCL_GPUSORT_H
#define OPENCL_GPUSORT_H

/*
 * Sort acceleration using GPU/MIC devices
 *
 * Because of device memory restriction, we have implemented two different 
 * sorting logic. One is in-chunk sort using bitonic-sort, the other is
 * inter-chunk sort using merge-sort.
 * DRAM capacity of usual discrete GPU/MIC devices is much less than host
 * system (for more correctness, it depends on maximum alloc size being
 * supported by OpenCL platform), so the algorithm needs to work even if
 * only a limited portion of the data to be sorted is visible; like a window
 * towards the whole landscape.
 * Our expectation is, our supported OpenCL device can load 4-5 chunks
 * simultaneously at leas, and each chunk has 50MB-100MB capacity.
 * 
 * Preprocess
 * ----------
 * Even though a chunk has 50MB-100MB capacity, it is much larger than
 * the size of usual data unit that PG-Strom performs on. (Also, column-
 * store contains "junk" records to be filtered on scan stage. We need
 * to remove them prior to sorting),
 * So, we takes a preprocess step that construct a larger column-store
 * (here, we call it sort-chunk), prior to main sort logic. It copies
 * the contents of usual row- and column- stores into the sort-chunk,
 * and set up index array; being used in the in-chunk sorting below.
 *
 * In-chunk sorting
 * ----------------
 * Prior to inter-chunks sorting, we sort the items within a particular
 * chunk. Here is nothing difficult to do because all the items are visible
 * for a kernel invocation, thus, all we are doing is as texebook says.
 *   Bitonic-sorter
 *   http://en.wikipedia.org/wiki/Bitonic_sorter
 * Host-side kicks an OpenCL kernel with a chunk in row- or column-
 * format. Then, kernel generate an array of sorted index.
 *
 * Inter-chunk sorting
 * -------------------
 * If data set is larger than capacity of a chunk, we needs to take another
 * logic to merge preliminary sorted chunks (by bitonic-sort).
 * Because of the DRAM size restriction, all kernel can see simultaneously
 * is at most 4-5 chunks. A regular merge-sort is designed to sort two
 * preliminary sorted smaller array; usually stored in sequential devices.
 * We deal with GPU/MIC DRAM as if a small window towards whole of data set.
 * Let's assume Please assume we try to merge 
 *
 *
 */






/*
 * kern_gpusort packs three structures but not explicitly shows because of
 * variable length fields.
 * The kern_parambuf (a structure for Param/Const values) is located on
 * the head of kern_gpusort structure.
 * Then, kern_column_store should be located on the next, and
 * kern_toastbuf should be last. We allocate toastbuf anyway.
 */
typedef struct
{
	kern_parambuf		kparam;

	/*
	 * variable length fields below
	 * -----------------------------
	 * kern_column_store	kchunk
	 * cl_int				status
	 * kern_toastbuf		ktoast
	 *
	 * On gpusort_setup_chunk_(rs|cs), whole of the kern_gpusort shall
	 * be written back.
	 * On gpusort_single, result buffer (a part of kchunk) and status
	 * shall be written back.
	 * On gpusort_multi, whole of the kern_gpusort shall be written
	 * back.
	 */
} kern_gpusort;

/* macro definitions to reference packed values */
#define KERN_GPUSORT_PARAMBUF(kgpusort)						\
	((__global kern_parambuf *)(&(kgpusort)->kparam))
#define KERN_GPUSORT_PARAMBUF_LENGTH(kgpusort)				\
	STROMALIGN(KERN_GPUSORT_PARAMBUF(kgpusort)->length)

#define KERN_GPUSORT_CHUNK(kgpusort)						\
	((__global kern_column_store *)							\
	 ((__global char *)KERN_GPUSORT_PARAMBUF(kgpusort) +	\
	  KERN_GPUSORT_PARAMBUF_LENGTH(kgpusort)))
#define KERN_GPUSORT_CHUNK_LENGTH(kgpusort)					\
	STROMALIGN(KERN_GPUSORT_CHUNK(kgpusort)->length)

#define KERN_GPUSORT_STATUS(kgpusort)						\
	((__global cl_int *)									\
	 ((__global char *)KERN_GPUSORT_CHUNK(kgpusort) +		\
	  KERN_GPUSORT_CHUNK_LENGTH(kgpusort)))
#define KERN_GPUSORT_STATUS_LENGTH(kgpusort)				\
	STROMALIGN(sizeof(cl_int))

#define KERN_GPUSORT_TOASTBUF(kgpusort)						\
	((__global kern_toastbuf *)								\
	 ((__global char *)KERN_GPUSORT_STATUS(kgpusort) +		\
	  KERN_GPUSORT_STATUS_LENGTH(kgpusort)))
#define KERN_GPUSORT_TOASTBUF_LENGTH(kgpusort)				\
	STROMALIGN(KERN_GPUSORT_TOASTBUF(kgpusort)->length)

/* last column of kchunk is index array of the chunk */
#define KERN_GPUSORT_RESULT_INDEX(kchunk)					\
	((__global cl_int *)									\
	 ((__global char *)(kchunk) +							\
	  (kchunk)->colmeta[(kchunk)->ncols - 1].cs_ofs))

#ifdef OPENCL_DEVICE_CODE
/*
 * comparison function - to be generated by PG-Strom on the fly
 */
static cl_int gpusort_comp(__private int *errcode,
						   __global kern_column_store *kcs_x,
						   __global kern_toastbuf *ktoast_x,
						   __private cl_int x_index,
						   __global kern_column_store *kcs_y,
						   __global kern_toastbuf *ktoast_y,
						   __private cl_int y_index);







/*
 * device only code below
 */


/* expected kernel prototypes */
static void
run_gpusort_single(__global kern_parambuf *kparams,
				   cl_bool reversing,					/* in */
				   cl_uint unitsz,						/* in */
				   __global kern_column_store *kchunk,	/* in */
				   __global kern_toastbuf *ktoast,		/* in */
				   __private cl_int *errcode,			/* out */
				   __local void *local_workbuf)
{
	__global cl_int	*results = KERN_GPUSORT_RESULT_INDEX(kchunk);

	/*
	 * sort the supplied kchunk according to the supplied
	 * compare function, then it put index of sorted array
	 * on the rindex buffer.
	 * (rindex array has the least 2^N capacity larger than nrows)
	 */

	cl_int	threadID		= get_global_id(0);
	cl_int	nrows			= (kchunk)->nrows;
	cl_int	halfUnitSize	= unitsz / 2;
	cl_int	unitMask		= unitsz - 1;
	cl_int	idx0;
	cl_int	idx1;

	idx0 = (threadID / halfUnitSize) * unitsz + threadID % halfUnitSize;
	idx1 = (reversing
			? ((idx0 & ~unitMask) | (~idx0 & unitMask))
			: (idx0 + halfUnitSize));
	if(nrows <= idx1)
		return;

	cl_int	pos0			= results[idx0];
	cl_int	pos1			= results[idx1];
	cl_int	rv;

	rv = gpusort_comp(errcode, kchunk, ktoast, pos0, kchunk, ktoast, pos1);
	if(0 < rv)
	{
		/* Swap */
		results[idx0] = pos1;
		results[idx1] = pos0;
	}
	return;
}
#if 0
static void
gpusort_set_record(__global kern_parambuf		*kparams,
				   cl_int			 			index,
				   cl_int						N,
				   cl_int						nrowsDst0,
				   __global kern_column_store	*chunkDst0,
				   __global kern_toastbuf		*toastDst0,
				   __global kern_column_store	*chunkDst1,
				   __global kern_toastbuf		*toastDst1,
				   __global kern_column_store	*chunkSrc0,
				   __global kern_toastbuf		*toastSrc0,
				   __global kern_column_store	*chunkSrc1,
				   __global kern_toastbuf		*toastSrc1,
				   __global cl_int				*result0,
				   __global cl_int				*result1,
				   __private cl_int				*errcode,
				   __local void				 	*local_workbuf)
{
	cl_int N2			= N / 2;
	cl_int posDst		= index;
	cl_int posSrc 		= (index < N2) ? result0[index] : result1[index - N2];
	cl_int chunkPosDst	= (posDst < nrowsDst0) : posDst : posDst - nrowsDst0;
	cl_int chunkPosSrc	= (posSrc < N2) : posSrc : posSrc - N2;


	// set nrows
	if(chunkPosDst == N2 - 1  &&  posSrc < N)
		(chunkDst)->nrows = N2;

	else if(N <= posSrc)
	{
		cl_int flagLastPlus1 = true;

		if(0 < chunkPosDst)
		{
			cl_int indexPrev = index - 1;
			cl_int posPrev	 = ((indexPrev < N2)
								? result0[indexPrev]
								: result1[indexPrev - N2]);
			if(N <= posPrev)
			  flagLastPlus1 = false;
		}

		if(flagLastPlus1 == true)
			(chunkDst)->nrows = chunkPosDst;
	}


	// set index
	__global cl_int *resultDst = KERN_GPUSORT_RESULT_INDEX(chunkDst);
	resultDst[chunkPosDst]     = (posSrc < N) ? chunkPosDst : N;


	// set row data
	if(posSrc  < N)
	{
		__global kern_column_store *chunkDst;
		__global kern_column_store *chunkSrc;
		__global kern_toastbuf     *toastDst;
		__global kern_toastbuf     *toastSrc;

		chunkDst = (posDst < nrowsDst0) ? chunkDst0 : chunkDst1;
		toastDst = (posDst < nrowsDst0) ? toastDst0 : toastDst1;
		chunkSrc = (posSrc < N2) ? chunkSrc0 : chunkSrc1;
		toastSrc = (posSrc < N2) ? toastSrc0 : toastSrc1;

		kern_column_to_column(errcode,
							  chunkDst, toastDst, chunkPosDst,
							  chunkSrc, toastSrc, chunkPosSrc, local_workbuf);
	}

	return;
}

static void
run_gpusort_multi(__global kern_parambuf *kparams,
				  cl_bool reversing,		/* in */
				  cl_uint unitsz,			/* out */
				  __global kern_column_store *x_chunk,
				  __global kern_toastbuf     *x_toast,
				  __global kern_column_store *y_chunk,
				  __global kern_toastbuf     *y_toast,
				  __global kern_column_store *z_chunk0,
				  __global kern_toastbuf     *z_toast0,
				  __global kern_column_store *z_chunk1,
				  __global kern_toastbuf     *z_toast1,
				  __private cl_int *errcode,
				  __local void *local_workbuf)
{
	__global cl_int	*x_results = KERN_GPUSORT_RESULT_INDEX(x_chunk);
	__global cl_int	*y_results = KERN_GPUSORT_RESULT_INDEX(y_chunk);

	/*
	 * Run merge sort logic on the supplied x_chunk and y_chunk.
	 * Its results shall be stored into z_chunk0 and z_chunk1,
	 *
	 */

	cl_int	threadID		= get_global_id(0);
	cl_int  x_nrows			= (x_chunk)->nrows;
	cl_int	y_nrows			= (y_chunk)->nrows;
	cl_int	halfUnitSize	= unitsz / 2;
	cl_int	unitMask		= unitsz - 1;
	cl_int	idx0;
	cl_int	idx1;

	idx0 = (threaadID / halfUnitSize) * unitsz + threadID % halfUnitSize;
	idx1 = (reversing
			? ((idx0 & ~unitMask) | (~idx0 & unitMask))
			: (idx0 + halfUnitSize));

	cl_int	N;

	for(int i=1; i<x_nrows+y_nrows; i<<=1) {
	}

	cl_int	N2	= N / 2; /* Starting index number of y_chunk */
	if(N2 <= threadID)
		return;

	/* Re-numbering the index at first times. */
	if(reversing)
	{
		if(x_nrows <= threadID)
			x_result[threadID] = N;

		y_results[idx1 - N2] = ((idx1 - N2 < y_nrows)
								? (y_results[idx1 - N2] + N2)
								: N);
	}


	__global cl_int	*results0;
	__global cl_int	*results1;

	results0 = (idx0 < N2) ? &x_results[idx0] : &y_results[idx0 - N2];
	results1 = (idx1 < N2) ? &x_results[idx1] : &y_results[idx1 - N2];

	cl_int	pos0	= *result0;
	cl_int	pos1	= *result1;

	if(N <= pos1)
	{
		/* pos1 is empry(maximum) */
	}

	else if (N <= pos0)
	{
		/* swap, pos0 is empry(maximum) */
		*result0 = pos1;
		*result1 = pos0;
	}

	else
	{
		/* sorting by data */
		__global kern_column_store	*chunk0 = (pos0 < N2) ? x_chunk : y_chunk;
		__global kern_column_store	*chunk1 = (pos1 < N2) ? x_chunk : y_chunk;
		__global kern_toastbuf		*toast0 = (pos0 < N2) ? x_toast : y_toast;
		__global kern_toastbuf		*toast1 = (pos1 < N2) ? x_toast : y_toast;
		cl_int						chkPos0 = (pos0 < N2) ? pos0 : (pos0 - N2);
		cl_int						chkPos1 = (pos1 < N2) ? pos1 : (pos1 - N2);

		cl_int rv = gpusort_comp(errcode,
								 chunk0, toast0, chkPos0,
								 chunk1, toast1, chkPos1);
		if(0 < rv)
		{
			/* swap */
			*result0 = pos1;
			*result1 = pos0;
		}
	}

	/* Update output chunk at last kernel. */
	if(unitsz == 2)
	{
		gpusort_set_record(kparams, idx0, N, N2,
						   z_chunk0, z_toast0, z_chunk1, z_toast1,
						   x_chunk, x_toast, y_chunk, y_toast,
						   x_result, y_result,
						   errcode, local_workbuf);

		gpusort_set_record(kparams, idx1, N, N2,
						   z_chunk0, z_toast0, z_chunk1, z_toast1,
						   x_chunk, x_toast, y_chunk, y_toast,
						   x_result, y_result,
						   errcode, local_workbuf);
	}

	return;
}




#endif

__kernel void
gpusort_single(cl_int bitonic_unitsz,
			   __global kern_gpusort *kgsort,
			   __local void *local_workbuf)
{
	__global kern_parambuf *kparams		= KERN_GPUSORT_PARAMBUF(kgsort);
	__global kern_column_store *kchunk	= KERN_GPUSORT_CHUNK(kgsort);
	__global kern_toastbuf *ktoast		= KERN_GPUSORT_TOASTBUF(kgsort);
	__global cl_int		   *results		= KERN_GPUSORT_RESULT_INDEX(kchunk);
	cl_bool		reversing = (bitonic_unitsz < 0 ? true : false);
	cl_uint		unitsz = (bitonic_unitsz < 0
						  ? 1U << -bitonic_unitsz
						  : 1U << bitonic_unitsz);
	cl_int		errcode = StromError_Success;

	run_gpusort_single(kparams, reversing, unitsz,
					   kchunk, ktoast, &errcode, local_workbuf);
}

#if 0
__kernel void
gpusort_multi(cl_int mergesort_unitsz,
			  __global kern_gpusort *kgsort_x,
			  __global kern_gpusort *kgsort_y,
			  __global kern_gpusort *kgsort_z1,
			  __global kern_gpusort *kgsort_z2,
			  __local void *local_workbuf)
{
	__global kern_parambuf *kparams		= KERN_GPUSORT_PARAMBUF(kgsort_x);
	__global kern_column_store *x_chunk = KERN_GPUSORT_CHUNK(kgsort_x);
	__global kern_column_store *y_chunk = KERN_GPUSORT_CHUNK(kgsort_y);
	__global kern_column_store *z_chunk1 = KERN_GPUSORT_CHUNK(kgsort_z1);
	__global kern_column_store *z_chunk2 = KERN_GPUSORT_CHUNK(kgsort_z2);
	__global kern_toastbuf *x_toast = KERN_GPUSORT_TOASTBUF(kgsort_x);
	__global kern_toastbuf *y_toast = KERN_GPUSORT_TOASTBUF(kgsort_y);
	__global kern_toastbuf *z_toast1 = KERN_GPUSORT_TOASTBUF(kgsort_z1);
	__global kern_toastbuf *z_toast2 = KERN_GPUSORT_TOASTBUF(kgsort_z2);
	cl_bool		reversing = (mergesort_unitsz < 0 ? true : false);
	cl_int		unitsz = (mergesort_unitsz < 0
						  ? 1U << -mergesort_unitsz
						  : 1U << mergesort_unitsz);
	cl_int		errcode = StromError_Success;

	run_gpusort_multi(kparams,
					  reversing, unitsz,
					  x_chunk, x_toast,
					  y_chunk, y_toast,
					  z_chunk1, z_toast1,
					  z_chunk2, z_toast2,
					  &errcode, local_workbuf);
}
#endif

/*
 * gpusort_setup_chunk_rs
 *
 * This routine move records from usual row-store (smaller) into
 * sorting chunk (a larger column store).
 *
 * The first column of the sorting chunk (cl_long) is identifier
 * of individual rows on the host side. The last column of the
 * sorting chunk (cl_uint) can be used as index of array.
 * Usually, this index is initialized to a sequential number,
 * then gpusort_single modifies this index array later.
 */
__kernel void
gpusort_setup_chunk_rs(cl_uint rcs_gstore_num,
					   __global kern_gpusort *kgpusort,
					   __global kern_row_store *krs,
					   __local void *local_workmem)
{
	__global kern_parambuf	   *kparams = KERN_GPUSORT_PARAMBUF(kgpusort);
	__global kern_column_store *kcs = KERN_GPUSORT_CHUNK(kgpusort);
	__global kern_toastbuf	   *ktoast = KERN_GPUSORT_TOASTBUF(kgpusort);
	__global cl_int			   *kstatus = KERN_GPUSORT_STATUS(kgpusort);
	__global cl_char		   *attrefs;
	__local size_t	kcs_offset;
	__local size_t	kcs_nitems;
	pg_bytea_t		kparam_0;
	cl_int			errcode = StromError_DataStoreNoSpace; //StromError_Success;
	if (get_local_id(0) == 0)
	{
		if (get_global_id(0) + get_local_size(0) < krs->nrows)
			kcs_nitems = get_local_size(0);
		else if (get_global_id(0) < krs_nrows)
			kcs_nitems = krs->nrows - kcs_offset;
		else
			kcs_nitems = 0;
		kcs_offset = atomic_and(&kcs->nrows, kcs_nitems);
	}
	barrier(CLK_LOCAL_MEM_FENCE);

	/* flag of referenced columns */
	kparam_0 = pg_bytea_param(kparams, &errcode, 0);
	attrefs = (__global cl_char *)VARDATA(kparam_0.value);

	kern_row_to_column(&errcode,
					   attrefs,
					   krs,
					   kcs,
					   ktoast,
					   kcs_offset,
					   kcs_nitems,
					   local_workmem);

	if (get_local_id(0) < kcs_nitems)
	{
		cl_uint		ncols = kcs->ncols;
		cl_uint		rindex = kcs_offset + get_local_id(0);
		cl_ulong	growid = (cl_ulong)rcs_gstore_num << 32 | get_global_id(0);
		__global cl_char   *addr;

		/* second last column is global record-id */
		addr = kern_get_datum(kcs, ncols - 2, rindex);
		*((__global cl_ulong *)addr) = grecid;
		/* last column is index number within a chunk */
		addr = kern_get_datum(kcs, ncols - 1, rindex);
		*((__global cl_uint *)addr) = rindex;
	}
	kern_writeback_error_status(kstatus, errcode, local_workmem);
}

__kernel void
gpusort_setup_chunk_cs(__global kern_gpusort *kgsort,
					   __global kern_column_store *kcs,
					   __global kern_toastbuf *ktoast,
					   __local void *local_workmem)
{
	/*
	 * This routine moves records from usual column-store (smaller)
	 * into sorting chunk (a larger column store), as a preprocess
	 * of GPU sorting.
	 * Note: get_global_offset(1) shows index of row-store on host.
	 */
}

#else	/* OPENCL_DEVICE_CODE */

typedef struct
{
	dlist_node		chain;		/* to be linked to pgstrom_gpusort */
	StromObject	  **rcs_slot;	/* array of underlying row/column-store */
	cl_uint			rcs_slotsz;	/* length of the array */
	cl_uint			rcs_nums;	/* current usage of the array */
	cl_uint			rcs_global_index;	/* starting offset in GpuSortState */
	kern_gpusort	kern;
} pgstrom_gpusort_chunk;

typedef struct
{
	pgstrom_message	msg;		/* = StromTag_GpuSort */
	Datum			dprog_key;	/* key of device program object */
	dlist_node		chain;		/* be linked to free list */
	dlist_head		in_chunk1;	/* sorted chunks to be merged */
	dlist_head		in_chunk2;	/* sorted chunks to be merged */
	dlist_head		work_chunk;	/* working buffer during merge sort */
} pgstrom_gpusort;

#define GPUSORT_MULTI_PER_BLOCK				\
	((SHMEM_BLOCKSZ - SHMEM_ALLOC_COST		\
	  - sizeof(dlist_node)) / sizeof(pgstrom_gpusort_multi))

#endif	/* !OPENCL_DEVICE_CODE */
#endif	/* OPENCL_GPUSORT_H */
