/*
 * Copyright (c) 2012 Mellanox Technologies, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <config.h>

#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <util/bitmap.h>

#include "mlx5.h"

/* Only ia64 requires this */
#ifdef __ia64__
#define MLX5_SHM_ADDR ((void *)0x8000000000000000UL)
#define MLX5_SHMAT_FLAGS (SHM_RND)
#else
#define MLX5_SHM_ADDR NULL
#define MLX5_SHMAT_FLAGS 0
#endif

#ifndef HPAGE_SIZE
#define HPAGE_SIZE              (2UL * 1024 * 1024)
#endif

#define MLX5_SHM_LENGTH         HPAGE_SIZE
#define MLX5_Q_CHUNK_SIZE       32768

static void free_huge_mem(struct mlx5_hugetlb_mem *hmem)
{
	if (hmem->bitmap)
		free(hmem->bitmap);

	if (shmdt(hmem->shmaddr) == -1)
		mlx5_dbg(stderr, MLX5_DBG_CONTIG, "%s\n", strerror(errno));
	shmctl(hmem->shmid, IPC_RMID, NULL);
	free(hmem);
}

static struct mlx5_hugetlb_mem *alloc_huge_mem(size_t size)
{
	struct mlx5_hugetlb_mem *hmem;
	size_t shm_len;

	hmem = mlx5_malloc(sizeof(*hmem));
	if (!hmem)
		return NULL;

	shm_len = align(size, MLX5_SHM_LENGTH);
	hmem->shmid = shmget(IPC_PRIVATE, shm_len, SHM_HUGETLB | SHM_R | SHM_W);
	if (hmem->shmid == -1) {
		mlx5_dbg(stderr, MLX5_DBG_CONTIG, "%s\n", strerror(errno));
		goto out_free;
	}

	hmem->shmaddr = shmat(hmem->shmid, MLX5_SHM_ADDR, MLX5_SHMAT_FLAGS);
	if (hmem->shmaddr == (void *)-1) {
		mlx5_dbg(stderr, MLX5_DBG_CONTIG, "%s\n", strerror(errno));
		goto out_rmid;
	}

	hmem->bitmap = bitmap_alloc0(shm_len / MLX5_Q_CHUNK_SIZE);
	if (!hmem->bitmap) {
		mlx5_dbg(stderr, MLX5_DBG_CONTIG, "%s\n", strerror(errno));
		goto out_shmdt;
	}

	hmem->bmp_size = shm_len / MLX5_Q_CHUNK_SIZE;

	/*
	 * Marked to be destroyed when process detaches from shmget segment
	 */
	shmctl(hmem->shmid, IPC_RMID, NULL);

	return hmem;

out_shmdt:
	if (shmdt(hmem->shmaddr) == -1)
		mlx5_dbg(stderr, MLX5_DBG_CONTIG, "%s\n", strerror(errno));

out_rmid:
	shmctl(hmem->shmid, IPC_RMID, NULL);

out_free:
	free(hmem);
	return NULL;
}

static int alloc_huge_buf(struct mlx5_context *mctx, struct mlx5_buf *buf,
			  size_t size, int page_size)
{
	int found = 0;
	int nchunk;
	struct mlx5_hugetlb_mem *hmem;
	int ret;

	buf->length = align(size, MLX5_Q_CHUNK_SIZE);
	nchunk = buf->length / MLX5_Q_CHUNK_SIZE;

	if (!nchunk)
		return 0;

	mlx5_spin_lock(&mctx->hugetlb_lock);
	list_for_each(&mctx->hugetlb_list, hmem, entry) {
		if (!bitmap_full(hmem->bitmap, hmem->bmp_size)) {
			buf->base = bitmap_find_free_region(hmem->bitmap,
							    hmem->bmp_size,
							    nchunk);
			if (buf->base != hmem->bmp_size) {
				bitmap_fill_region(hmem->bitmap, buf->base,
						   buf->base + nchunk);
				buf->hmem = hmem;
				found = 1;
				break;
			}
		}
	}
	mlx5_spin_unlock(&mctx->hugetlb_lock);

	if (!found) {
		hmem = alloc_huge_mem(buf->length);
		if (!hmem)
			return -1;

		buf->base = 0;
		assert(nchunk <= hmem->bmp_size);
		bitmap_fill_region(hmem->bitmap, 0, nchunk);

		buf->hmem = hmem;

		mlx5_spin_lock(&mctx->hugetlb_lock);
		if (nchunk != hmem->bmp_size)
			list_add(&mctx->hugetlb_list, &hmem->entry);
		else
			list_add_tail(&mctx->hugetlb_list, &hmem->entry);
		mlx5_spin_unlock(&mctx->hugetlb_lock);
	}

	buf->buf = hmem->shmaddr + buf->base * MLX5_Q_CHUNK_SIZE;

	ret = ibv_dontfork_range(buf->buf, buf->length);
	if (ret) {
		mlx5_dbg(stderr, MLX5_DBG_CONTIG, "\n");
		goto out_fork;
	}
	buf->type = MLX5_ALLOC_TYPE_HUGE;

	return 0;

out_fork:
	mlx5_spin_lock(&mctx->hugetlb_lock);
	bitmap_zero_region(hmem->bitmap, buf->base, buf->base + nchunk);
	if (bitmap_empty(hmem->bitmap, hmem->bmp_size)) {
		list_del(&hmem->entry);
		mlx5_spin_unlock(&mctx->hugetlb_lock);
		free_huge_mem(hmem);
	} else
		mlx5_spin_unlock(&mctx->hugetlb_lock);

	return -1;
}

static void free_huge_buf(struct mlx5_context *ctx, struct mlx5_buf *buf)
{
	int nchunk;

	nchunk = buf->length / MLX5_Q_CHUNK_SIZE;
	if (!nchunk)
		return;

	mlx5_spin_lock(&ctx->hugetlb_lock);
	bitmap_zero_region(buf->hmem->bitmap, buf->base, buf->base + nchunk);
	if (bitmap_empty(buf->hmem->bitmap, buf->hmem->bmp_size)) {
		list_del(&buf->hmem->entry);
		mlx5_spin_unlock(&ctx->hugetlb_lock);
		free_huge_mem(buf->hmem);
	} else
		mlx5_spin_unlock(&ctx->hugetlb_lock);
}

void mlx5_free_buf_extern(struct mlx5_context *ctx, struct mlx5_buf *buf)
{
	ibv_dofork_range(buf->buf, buf->length);
	ctx->extern_alloc.free(buf->buf, ctx->extern_alloc.data);
}

int mlx5_alloc_buf_extern(struct mlx5_context *ctx, struct mlx5_buf *buf,
		size_t size)
{
	void *addr;

	addr = ctx->extern_alloc.alloc(size, ctx->extern_alloc.data);
	if (addr || size == 0) {
		if (ibv_dontfork_range(addr, size)) {
			mlx5_dbg(stderr, MLX5_DBG_CONTIG,
				"External mode dontfork_range failed\n");
			ctx->extern_alloc.free(addr,
				ctx->extern_alloc.data);
			return -1;
		}
		buf->buf = addr;
		buf->length = size;
		buf->type = MLX5_ALLOC_TYPE_EXTERNAL;
		return 0;
	}

	mlx5_dbg(stderr, MLX5_DBG_CONTIG, "External alloc failed\n");
	return -1;
}

static void mlx5_free_buf_custom(struct mlx5_context *ctx,
			  struct mlx5_buf *buf)
{
	struct mlx5_parent_domain *mparent_domain = buf->mparent_domain;

	mparent_domain->free(&mparent_domain->mpd.ibv_pd,
			     mparent_domain->pd_context,
			     buf->buf,
			     buf->resource_type);
}

static int mlx5_alloc_buf_custom(struct mlx5_context *ctx,
			  struct mlx5_buf *buf, size_t size)
{
	struct mlx5_parent_domain *mparent_domain = buf->mparent_domain;
	void *addr;

	addr = mparent_domain->alloc(&mparent_domain->mpd.ibv_pd,
				   mparent_domain->pd_context, size,
				   buf->req_alignment,
				   buf->resource_type);
	if (addr == IBV_ALLOCATOR_USE_DEFAULT)
		return 1;

	if (addr || size == 0) {
		buf->buf = addr;
		buf->length = size;
		buf->type = MLX5_ALLOC_TYPE_CUSTOM;
		return 0;
	}

	return -1;
}

int mlx5_alloc_prefered_buf(struct mlx5_context *mctx,
			    struct mlx5_buf *buf,
			    size_t size, int page_size,
			    enum mlx5_alloc_type type,
			    const char *component)
{
	int ret;

	if (type == MLX5_ALLOC_TYPE_CUSTOM) {
		ret = mlx5_alloc_buf_custom(mctx, buf, size);
		if (ret <= 0)
			return ret;

		/* Fallback - default allocation is required */
	}

	/*
	 * Fallback mechanism priority:
	 *	huge pages
	 *	contig pages
	 *	default
	 */
	if (type == MLX5_ALLOC_TYPE_HUGE ||
	    type == MLX5_ALLOC_TYPE_PREFER_HUGE ||
	    type == MLX5_ALLOC_TYPE_ALL) {
		ret = alloc_huge_buf(mctx, buf, size, page_size);
		if (!ret)
			return 0;

		if (type == MLX5_ALLOC_TYPE_HUGE)
			return -1;

		mlx5_dbg(stderr, MLX5_DBG_CONTIG,
			 "Huge mode allocation failed, fallback to %s mode\n",
			 MLX5_ALLOC_TYPE_ALL ? "contig" : "default");
	}

	if (type == MLX5_ALLOC_TYPE_CONTIG ||
	    type == MLX5_ALLOC_TYPE_PREFER_CONTIG ||
	    type == MLX5_ALLOC_TYPE_ALL) {
		ret = mlx5_alloc_buf_contig(mctx, buf, size, page_size, component);
		if (!ret)
			return 0;

		if (type == MLX5_ALLOC_TYPE_CONTIG)
			return -1;
		mlx5_dbg(stderr, MLX5_DBG_CONTIG,
			 "Contig allocation failed, fallback to default mode\n");
	}

	if (type == MLX5_ALLOC_TYPE_EXTERNAL)
		return mlx5_alloc_buf_extern(mctx, buf, size);

	return mlx5_alloc_buf(buf, size, page_size);

}

int mlx5_free_actual_buf(struct mlx5_context *ctx, struct mlx5_buf *buf)
{
	int err = 0;

	switch (buf->type) {
	case MLX5_ALLOC_TYPE_ANON:
		mlx5_free_buf(buf);
		break;

	case MLX5_ALLOC_TYPE_HUGE:
		free_huge_buf(ctx, buf);
		break;

	case MLX5_ALLOC_TYPE_CONTIG:
		mlx5_free_buf_contig(ctx, buf);
		break;

	case MLX5_ALLOC_TYPE_EXTERNAL:
		mlx5_free_buf_extern(ctx, buf);
		break;

	case MLX5_ALLOC_TYPE_CUSTOM:
		mlx5_free_buf_custom(ctx, buf);
		break;

	default:
		mlx5_err(ctx->dbg_fp, "Bad allocation type\n");
	}

	return err;
}

/* This function computes log2(v) rounded up.
   We don't want to have a dependency to libm which exposes ceil & log2 APIs.
   Code was written based on public domain code:
	URL: http://graphics.stanford.edu/~seander/bithacks.html#IntegerLog.
*/
static uint32_t mlx5_get_block_order(uint32_t v)
{
	static const uint32_t bits_arr[] = {0x2, 0xC, 0xF0, 0xFF00, 0xFFFF0000};
	static const uint32_t shift_arr[] = {1, 2, 4, 8, 16};
	int i;
	uint32_t input_val = v;

	register uint32_t r = 0;/* result of log2(v) will go here */
	for (i = 4; i >= 0; i--) {
		if (v & bits_arr[i]) {
			v >>= shift_arr[i];
			r |= shift_arr[i];
		}
	}
	/* Rounding up if required */
	r += !!(input_val & ((1 << r) - 1));

	return r;
}

bool mlx5_is_custom_alloc(struct ibv_pd *pd)
{
	struct mlx5_parent_domain *mparent_domain = to_mparent_domain(pd);

	return (mparent_domain && mparent_domain->alloc && mparent_domain->free);
}

bool mlx5_is_extern_alloc(struct mlx5_context *context)
{
	return context->extern_alloc.alloc && context->extern_alloc.free;
}

void mlx5_get_alloc_type(struct mlx5_context *context,
			 struct ibv_pd *pd,
			 const char *component,
			 enum mlx5_alloc_type *alloc_type,
			 enum mlx5_alloc_type default_type)

{
	char *env_value;
	char name[128];

	if (mlx5_is_custom_alloc(pd)) {
		*alloc_type = MLX5_ALLOC_TYPE_CUSTOM;
		return;
	}

	if (mlx5_is_extern_alloc(context)) {
		*alloc_type = MLX5_ALLOC_TYPE_EXTERNAL;
		return;
	}

	snprintf(name, sizeof(name), "%s_ALLOC_TYPE", component);

	*alloc_type = default_type;

	env_value = getenv(name);
	if (env_value) {
		if (!strcasecmp(env_value, "ANON"))
			*alloc_type = MLX5_ALLOC_TYPE_ANON;
		else if (!strcasecmp(env_value, "HUGE"))
			*alloc_type = MLX5_ALLOC_TYPE_HUGE;
		else if (!strcasecmp(env_value, "CONTIG"))
			*alloc_type = MLX5_ALLOC_TYPE_CONTIG;
		else if (!strcasecmp(env_value, "PREFER_CONTIG"))
			*alloc_type = MLX5_ALLOC_TYPE_PREFER_CONTIG;
		else if (!strcasecmp(env_value, "PREFER_HUGE"))
			*alloc_type = MLX5_ALLOC_TYPE_PREFER_HUGE;
		else if (!strcasecmp(env_value, "ALL"))
			*alloc_type = MLX5_ALLOC_TYPE_ALL;
	}
}

static void mlx5_alloc_get_env_info(struct mlx5_context *mctx,
				    int *max_block_log,
				    int *min_block_log,
				    const char *component)

{
	char *env;
	int value;
	char name[128];

	/* First set defaults */
	*max_block_log = MLX5_MAX_LOG2_CONTIG_BLOCK_SIZE;
	*min_block_log = MLX5_MIN_LOG2_CONTIG_BLOCK_SIZE;

	snprintf(name, sizeof(name), "%s_MAX_LOG2_CONTIG_BSIZE", component);
	env = getenv(name);
	if (env) {
		value = atoi(env);
		if (value <= MLX5_MAX_LOG2_CONTIG_BLOCK_SIZE &&
		    value >= MLX5_MIN_LOG2_CONTIG_BLOCK_SIZE)
			*max_block_log = value;
		else
			mlx5_err(mctx->dbg_fp, "Invalid value %d for %s\n",
				 value, name);
	}
	sprintf(name, "%s_MIN_LOG2_CONTIG_BSIZE", component);
	env = getenv(name);
	if (env) {
		value = atoi(env);
		if (value >= MLX5_MIN_LOG2_CONTIG_BLOCK_SIZE &&
		    value  <=  *max_block_log)
			*min_block_log = value;
		else
			mlx5_err(mctx->dbg_fp, "Invalid value %d for %s\n",
				 value, name);
	}
}

int mlx5_alloc_buf_contig(struct mlx5_context *mctx,
			  struct mlx5_buf *buf, size_t size,
			  int page_size,
			  const char *component)
{
	void *addr = MAP_FAILED;
	int block_size_exp;
	int max_block_log;
	int min_block_log;
	struct ibv_context *context = &mctx->ibv_ctx.context;
	off_t offset;

	mlx5_alloc_get_env_info(mctx, &max_block_log,
				&min_block_log,
				component);

	block_size_exp = mlx5_get_block_order(size);

	if (block_size_exp > max_block_log)
		block_size_exp = max_block_log;

	do {
		offset = 0;
		set_command(MLX5_IB_MMAP_GET_CONTIGUOUS_PAGES, &offset);
		set_order(block_size_exp, &offset);
		addr = mmap(NULL , size, PROT_WRITE | PROT_READ, MAP_SHARED,
			    context->cmd_fd, page_size * offset);
		if (addr != MAP_FAILED)
			break;

		/*
		 *  The kernel returns EINVAL if not supported
		 */
		if (errno == EINVAL)
			return -1;

		block_size_exp -= 1;
	} while (block_size_exp >= min_block_log);
	mlx5_dbg(mctx->dbg_fp, MLX5_DBG_CONTIG, "block order %d, addr %p\n",
		 block_size_exp, addr);

	if (addr == MAP_FAILED)
		return -1;

	if (ibv_dontfork_range(addr, size)) {
		munmap(addr, size);
		return -1;
	}

	buf->buf = addr;
	buf->length = size;
	buf->type = MLX5_ALLOC_TYPE_CONTIG;

	return 0;
}

void mlx5_free_buf_contig(struct mlx5_context *mctx, struct mlx5_buf *buf)
{
	ibv_dofork_range(buf->buf, buf->length);
	munmap(buf->buf, buf->length);
}

int mlx5_alloc_buf(struct mlx5_buf *buf, size_t size, int page_size)
{
	int ret;
	int al_size;

	al_size = align(size, page_size);
	ret = posix_memalign(&buf->buf, page_size, al_size);
	if (ret)
		return ret;

	ret = ibv_dontfork_range(buf->buf, al_size);
	if (ret)
		free(buf->buf);

	if (!ret) {
		buf->length = al_size;
		buf->type = MLX5_ALLOC_TYPE_ANON;
	}

	return ret;
}

void mlx5_free_buf(struct mlx5_buf *buf)
{
	ibv_dofork_range(buf->buf, buf->length);
	free(buf->buf);
}
