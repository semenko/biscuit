#include <string.h>
#include <stdio.h>
#include <zlib.h>
#include <assert.h>
#include "bntseq.h"
#include "bwa.h"
#include "ksw.h"
#include "utils.h"
#include "kstring.h"
#include "ksort.h"
#include "kvec.h"

#ifdef USE_MALLOC_WRAPPERS
#  include "malloc_wrap.h"
#endif

int bwa_verbose = 3;
char bwa_rg_id[256];
char *bwa_pg;

/************************
 * Batch FASTA/Q reader *
 ************************/

#include "kseq.h"
KSEQ_DECLARE(gzFile)

/**
 * remove /1, /2 in read names, those are generated by
 * some machines and some reverse conversion from bams */
static inline void trim_readno(kstring_t *s) {
  if (s->l > 2 && s->s[s->l-2] == '/' && isdigit(s->s[s->l-1]))
    s->l -= 2, s->s[s->l] = 0;
  /* if (s->l > 2 && s->s[s->l-2] == '.' && isdigit(s->s[s->l-1])) */
  /* s->l -= 2, s->s[s->l] = 0; */
}

static inline void kseq2bseq1(const kseq_t *ks, bseq1_t *s) {
 // TODO: it would be better to allocate one chunk of memory, but probably it does not matter in practice
  s->name = strdup(ks->name.s);
  s->comment = ks->comment.l? strdup(ks->comment.s) : 0;
  s->seq = (uint8_t*) strdup(ks->seq.s);
  s->qual = ks->qual.l? strdup(ks->qual.s) : 0;
  s->l_seq = strlen((char*) s->seq);
}

/**
 * read bseq
 * @param ks1_, ks2_   kseq_t*
 * @param chunk_size   number of sequences to read
 * @param n_           (out) number of reads loaded
 * @return seqs        bseq1_t* 
 */
bseq1_t *bseq_read(int chunk_size, int *n_, void *ks1_, void *ks2_) {
  kseq_t *ks = (kseq_t*)ks1_, *ks2 = (kseq_t*)ks2_;
  int size = 0, m, n;
  bseq1_t *seqs;
  m = n = 0; seqs = 0;
  while (kseq_read(ks) >= 0) {
    if (ks2 && kseq_read(ks2) < 0) { // the 2nd file has fewer reads
      fprintf(stderr, "[W::%s] the 2nd file has fewer sequences.\n", __func__);
      break;
    }
    if (n >= m) {
      m = m? m<<1 : 256;
      seqs = realloc(seqs, m * sizeof(bseq1_t));
    }
    trim_readno(&ks->name);
    kseq2bseq1(ks, &seqs[n]);
    seqs[n].id = n;
    size += seqs[n++].l_seq;
    if (ks2) {
      trim_readno(&ks2->name);
      kseq2bseq1(ks2, &seqs[n]);
      seqs[n].id = n;
      size += seqs[n++].l_seq;
    }
    if (size >= chunk_size && (n&1) == 0) break;
  }
  if (size == 0) { // test if the 2nd file is finished
    if (ks2 && kseq_read(ks2) >= 0)
      fprintf(stderr, "[W::%s] the 1st file has fewer sequences.\n", __func__);
  }
  *n_ = n;
  return seqs;
}

/**
 * interleaved to separate storage of paired-end reads
 * @param seqs bseq1_t* interleaved
 * @param sep  (out) bseq1_t[2] reads in first and second in pair */
void bseq_classify(int n, bseq1_t *seqs, int m[2], bseq1_t *sep[2])
{
  int i, has_last;
  kvec_t(bseq1_t) a[2] = {{0,0,0}, {0,0,0}};
  for (i = 1, has_last = 1; i < n; ++i) {
    if (has_last) {
      if (strcmp(seqs[i].name, seqs[i-1].name) == 0) {
        kv_push(bseq1_t, a[1], seqs[i-1]);
        kv_push(bseq1_t, a[1], seqs[i]);
        has_last = 0;
      } else kv_push(bseq1_t, a[0], seqs[i-1]);
    } else has_last = 1;
  }
  if (has_last) kv_push(bseq1_t, a[0], seqs[i-1]);
  sep[0] = a[0].a, m[0] = a[0].n;
  sep[1] = a[1].a, m[1] = a[1].n;
}

/*****************
 * CIGAR related *
 *****************/

/**
 * fill regular scoring matrix */
void bwa_fill_scmat(int a, int b, int8_t mat[25]) {
  int i, j, k;
  for (i = k = 0; i < 4; ++i) {
    for (j = 0; j < 4; ++j)
      mat[k++] = i == j? a : -b;
    mat[k++] = -1; // ambiguous base
  }
  for (j = 0; j < 5; ++j) mat[k++] = -1;
}

/**
 * fill C>T assymmetric scoring matrix */
void bwa_fill_scmat_ct(int a, int b, int8_t mat[25]) {
  int i, j, k;
  for (i = k = 0; i < 4; ++i) {
    for (j = 0; j < 4; ++j) {
      if (i==1 && j==3) mat[k++] = a;
      else mat[k++] = i == j? a : -b;
    }
    mat[k++] = -1; // ambiguous base
  }
  for (j = 0; j < 5; ++j) mat[k++] = -1;
}

/**
 * fill G>A asymmetric scoring */
void bwa_fill_scmat_ga(int a, int b, int8_t mat[25]) {
  int i, j, k;
  for (i = k = 0; i < 4; ++i) {
    for (j = 0; j < 4; ++j) {
      if (i==2 && j==0) mat[k++] = a;
      else mat[k++] = i == j? a : -b;
    }
    mat[k++] = -1; // ambiguous base
  }
  for (j = 0; j < 5; ++j) mat[k++] = -1;
}

// Generate CIGAR when the alignment end points are known
uint32_t *bwa_gen_cigar2(const int8_t mat[25], int o_del, int e_del, int o_ins, int e_ins, int w_, int64_t l_pac, const uint8_t *pac, int l_query, uint8_t *query, int64_t rb, int64_t re, int *score, int *n_cigar, int *NM)
{
  uint32_t *cigar = 0;
  uint8_t tmp, *rseq;
  int i;
  int64_t rlen;
  kstring_t str;
  const char *int2base;

  if (n_cigar) *n_cigar = 0;
  if (NM) *NM = -1;
  if (l_query <= 0 || rb >= re || (rb < l_pac && re > l_pac)) return 0; // reject if negative length or bridging the forward and reverse strand
  rseq = bns_get_seq(l_pac, pac, rb, re, &rlen);
  if (re - rb != rlen) goto ret_gen_cigar; // possible if out of range
  if (rb >= l_pac) { // then reverse both query and rseq; this is to ensure indels to be placed at the leftmost position
    for (i = 0; i < l_query>>1; ++i)
      tmp = query[i], query[i] = query[l_query - 1 - i], query[l_query - 1 - i] = tmp;
    for (i = 0; i < rlen>>1; ++i)
      tmp = rseq[i], rseq[i] = rseq[rlen - 1 - i], rseq[rlen - 1 - i] = tmp;
  }
  if (l_query == re - rb && w_ == 0) { // no gap; no need to do DP
    // UPDATE: we come to this block now... FIXME: due to an issue in mem_reg2aln(), we never come to this block. This does not affect accuracy, but it hurts performance.
    if (n_cigar) {
      cigar = malloc(4);
      cigar[0] = l_query<<4 | 0;
      *n_cigar = 1;
    }
    for (i = 0, *score = 0; i < l_query; ++i)
      *score += mat[rseq[i]*5 + query[i]];
  } else {
    int w, max_gap, max_ins, max_del, min_w;
    // set the band-width
    max_ins = (int)((double)(((l_query+1)>>1) * mat[0] - o_ins) / e_ins + 1.);
    max_del = (int)((double)(((l_query+1)>>1) * mat[0] - o_del) / e_del + 1.);
    max_gap = max_ins > max_del? max_ins : max_del;
    max_gap = max_gap > 1? max_gap : 1;
    w = (max_gap + abs(rlen - l_query) + 1) >> 1;
    w = w < w_? w : w_;
    min_w = abs(rlen - l_query) + 3;
    w = w > min_w? w : min_w;
    // NW alignment
    if (bwa_verbose >= 4) {
      printf("* Global bandwidth: %d\n", w);
      printf("* Global ref:   "); for (i = 0; i < rlen; ++i) putchar("ACGTN"[(int)rseq[i]]); putchar('\n');
      printf("* Global query: "); for (i = 0; i < l_query; ++i) putchar("ACGTN"[(int)query[i]]); putchar('\n');
    }
    *score = ksw_global2(l_query, query, rlen, rseq, 5, mat, o_del, e_del, o_ins, e_ins, w, n_cigar, &cigar);
  }
  if (NM && n_cigar) {// compute NM and MD
    int k, x, y, u, n_mm = 0, n_gap = 0;
    str.l = str.m = *n_cigar * 4; str.s = (char*)cigar; // append MD to CIGAR
    int2base = rb < l_pac? "ACGTN" : "TGCAN";
    for (k = 0, x = y = u = 0; k < *n_cigar; ++k) {
      int op, len;
      cigar = (uint32_t*)str.s;
      op  = cigar[k]&0xf, len = cigar[k]>>4;
      if (op == 0) { // match
	for (i = 0; i < len; ++i) {
	  if (query[x + i] != rseq[y + i]) {
	    kputw(u, &str);
	    kputc(int2base[rseq[y+i]], &str);
	    ++n_mm; u = 0;
	  } else ++u;
	}
	x += len; y += len;
      } else if (op == 2) { // deletion
	if (k > 0 && k < *n_cigar - 1) { // don't do the following if D is the first or the last CIGAR
	  kputw(u, &str); kputc('^', &str);
	  for (i = 0; i < len; ++i)
	    kputc(int2base[rseq[y+i]], &str);
	  u = 0; n_gap += len;
	}
	y += len;
      } else if (op == 1) x += len, n_gap += len; // insertion
    }
    kputw(u, &str); kputc(0, &str);
    *NM = n_mm + n_gap;
    cigar = (uint32_t*)str.s;
  }
  if (rb >= l_pac) // reverse back query
    for (i = 0; i < l_query>>1; ++i)
      tmp = query[i], query[i] = query[l_query - 1 - i], query[l_query - 1 - i] = tmp;

ret_gen_cigar:
  free(rseq);
  return cigar;
}

/**
 * Generate cigar from rb and re
 *
 * if there is gap do banded DP, if no gap, do seed extension 
 * 
 * MD is appended to cigar.
 * mat is dependent on bss not parent, that is
 * BSW should use ctmat and BSC should use gamat
 * @param mat, o_del, e_del, o_ins, e_ins, w_, l_pac, pac
 * @param l_query, query, rb, re, parent
 * @param score     (out) alignment score based on the scoring matrix
 * @param n_cigar   (out) number of cigars
 * @param NM        (out) edit distance
 * @param ZC,ZR     (out) conversion and retention
 * 
 * @return cigar    uint32_t
 */
uint32_t *bis_bwa_gen_cigar2(const int8_t mat[25], int o_del, int e_del, int o_ins, int e_ins, int w_, int64_t l_pac, const uint8_t *pac, int l_query, uint8_t *query, int64_t rb, int64_t re, int *score, int *n_cigar, int *NM, uint32_t *ZC, uint32_t *ZR, uint8_t parent) {

  uint32_t *cigar = 0;
  uint8_t tmp, *rseq;
  int i;
  int64_t rlen;
  kstring_t str;
  const char *int2base;

  if (n_cigar) *n_cigar = 0;
  if (NM) *NM = -1;
  if (l_query <= 0 || rb >= re || (rb < l_pac && re > l_pac)) return 0; // reject if negative length or bridging the forward and reverse strand

  rseq = bns_get_seq(l_pac, pac, rb, re, &rlen);

  if (re - rb != rlen) goto ret_gen_cigar; // possible if out of range

  if (rb >= l_pac) { // then reverse both query and rseq; this is to ensure indels to be placed at the leftmost position
    for (i = 0; i < l_query>>1; ++i)
      tmp = query[i], query[i] = query[l_query - 1 - i], query[l_query - 1 - i] = tmp;
    for (i = 0; i < rlen>>1; ++i)
      tmp = rseq[i], rseq[i] = rseq[rlen - 1 - i], rseq[rlen - 1 - i] = tmp;
  }

  if (l_query == re - rb && w_ == 0) { // no gap; no need to do DP
    // UPDATE: we come to this block now... FIXME: due to an issue in mem_reg2aln(), we never come to this block. This does not affect accuracy, but it hurts performance.
    if (n_cigar) {
      cigar = malloc(4);
      cigar[0] = l_query<<4 | 0;
      *n_cigar = 1;
    }
    for (i = 0, *score = 0; i < l_query; ++i)
      *score += mat[rseq[i]*5 + query[i]];

  } else {

    int w, max_gap, max_ins, max_del, min_w;
    /* band-width is maximum of insertion and deletion */
    max_ins = (int)((double)(((l_query+1)>>1) * mat[0] - o_ins) / e_ins + 1.);
    max_del = (int)((double)(((l_query+1)>>1) * mat[0] - o_del) / e_del + 1.);
    max_gap = max_ins > max_del? max_ins : max_del;
    max_gap = max_gap > 1? max_gap : 1;
    w = (max_gap + abs(rlen - l_query) + 1) >> 1;
    w = w < w_? w : w_;
    min_w = abs(rlen - l_query) + 3;
    w = w > min_w? w : min_w;

    /* NW alignment */
    if (bwa_verbose >= 4) {
      printf("* Global bandwidth: %d\n", w);
      printf("* Global ref:   "); for (i = 0; i < rlen; ++i) putchar("ACGTN"[(int)rseq[i]]); putchar('\n');
      printf("* Global query: "); for (i = 0; i < l_query; ++i) putchar("ACGTN"[(int)query[i]]); putchar('\n');
    }
    *score = ksw_global2(l_query, query, rlen, rseq, 5, mat, o_del, e_del, o_ins, e_ins, w, n_cigar, &cigar);

  }

  /* loop over cigars and compute NM and MD*/
  if (NM && n_cigar) {
    /* n_mm: number of mismatches, 
     * n_conv, n_ret: number of conversion and retention */
    int k, x, y, u, n_mm = 0, n_gap = 0, n_conv = 0, n_ret = 0;
    str.l = str.m = *n_cigar * 4; str.s = (char*)cigar; // append MD to CIGAR
    int2base = rb < l_pac? "ACGTN" : "TGCAN";
    for (k = 0, x = y = u = 0; k < *n_cigar; ++k) {
      int op, len;
      cigar = (uint32_t*)str.s;
      op  = cigar[k]&0xf, len = cigar[k]>>4;
      if (op == 0) { // match
        for (i = 0; i < len; ++i) {

          /* to allow assymmetric CT and GA */
          unsigned char _q = query[x+i];
          unsigned char _r = rseq[y+i];
          if (_q == _r) {
            if (parent && _q == 1) ++n_ret;
            if (!parent && _q == 2) ++n_ret;
            ++u;
          } else if (parent && _q == 3 && _r == 1) {
            ++n_conv; ++u;
          } else if (!parent && _q == 0 && _r == 2) {
            ++n_conv; ++u;
          } else {
            kputw(u, &str);
            kputc(int2base[_r], &str);
            ++n_mm; u = 0;
          }

          /* if (query[x + i] != rseq[y + i]) { */
          /* 	kputw(u, &str); */
          /* 	kputc(int2base[rseq[y+i]], &str); */
          /* 	++n_mm; u = 0; */
          /* } else ++u; */
        }
        x += len; y += len;
      } else if (op == 2) { // deletion
        if (k > 0 && k < *n_cigar - 1) { // don't do the following if D is the first or the last CIGAR
          kputw(u, &str); kputc('^', &str);
          for (i = 0; i < len; ++i)
            kputc(int2base[rseq[y+i]], &str);
          u = 0; n_gap += len;
        }
        y += len;
      } else if (op == 1) {	// insertion does not contribute to MD
        x += len, n_gap += len;
      }
    }
    kputw(u, &str); kputc(0, &str);
    /* NM contains both gap and mismatches, and every base in a gap counts */
    *NM = n_mm + n_gap;	
    *ZC = n_conv;		/* conversion counts */
    *ZR = n_ret;		/* retention counts */
    cigar = (uint32_t*)str.s;
  } // compute NM and MD

  /* reverse back query */
  if (rb >= l_pac)
    for (i = 0; i < l_query>>1; ++i)
      tmp = query[i], query[i] = query[l_query - 1 - i], query[l_query - 1 - i] = tmp;

ret_gen_cigar:
  free(rseq);
  return cigar;
}

uint32_t *bwa_gen_cigar(const int8_t mat[25], int q, int r, int w_, int64_t l_pac, const uint8_t *pac, int l_query, uint8_t *query, int64_t rb, int64_t re, int *score, int *n_cigar, int *NM)
{
  return bwa_gen_cigar2(mat, q, r, q, r, w_, l_pac, pac, l_query, query, rb, re, score, n_cigar, NM);
}

/*********************
 * Full index reader *
 *********************/

/**
 * check and make sure the index is intact
 */
char *bwa_idx_infer_prefix(const char *hint) {
  char *prefix;
  int l_hint;
  FILE *fp;
  l_hint = strlen(hint);

  /* prefix = malloc(l_hint + 3 + 4 + 1); */
  /* strcpy(prefix, hint); */
  /* strcpy(prefix + l_hint, ".64.bwt"); */
  /* if ((fp = fopen(prefix, "rb")) != 0) { */
  /* 	fclose(fp); */
  /* 	prefix[l_hint + 3] = 0; */
  /* 	return prefix; */
  /* } else { */
  /* 	strcpy(prefix + l_hint, ".bwt"); */
  /* 	if ((fp = fopen(prefix, "rb")) == 0) { */
  /* 		free(prefix); */
  /* 		return 0; */
  /* 	} else { */
  /* 		fclose(fp); */
  /* 		prefix[l_hint] = 0; */
  /* 		return prefix; */
  /* 	} */
  /* } */

  /* bisufite adaptation */
  prefix = malloc(l_hint+20);
  strcpy(prefix, hint);
  strcpy(prefix + l_hint, ".par.bwt");
  if ((fp = fopen(prefix, "rb")) == 0) {
    free(prefix);
    return 0;
  }
  fclose(fp);
  strcpy(prefix + l_hint, ".dau.bwt");
  if ((fp = fopen(prefix, "rb")) == 0) {
    free(prefix);
    return 0;
  }
  fclose(fp);
  
  prefix[l_hint] = 0;
  return prefix;
}

/**
 * Load bwt index and SA
 * @param hint    the hint
 * @param parent  1 for parent strand 0 for daughter strand
 * @param bwt     (out) bwt_t to be loaded
 */
void bwa_idx_load_bwt(const char *hint, uint8_t parent, bwt_t *bwt)
{
  char *tmp, *prefix;
  prefix = bwa_idx_infer_prefix(hint);
  if (prefix == 0) {
    if (bwa_verbose >= 1)
      fprintf(stderr, "[E::%s] fail to locate the index files\n", __func__);
    return;
  }

  tmp = calloc(strlen(prefix) + 20, 1);
  if (parent) {
    strcat(strcpy(tmp, prefix), ".par.bwt"); // FM-index
    bwt_restore_bwt2(tmp, bwt);
    strcat(strcpy(tmp, prefix), ".par.sa");  // partial suffix array (SA)
    bwt_restore_sa(tmp, bwt);
  } else {
    strcat(strcpy(tmp, prefix), ".dau.bwt"); // FM-index
    bwt_restore_bwt2(tmp, bwt);
    strcat(strcpy(tmp, prefix), ".dau.sa");  // partial suffix array (SA)
    bwt_restore_sa(tmp, bwt);
  }
  bwt->parent = parent;

  free(tmp); free(prefix);
}

/**
 * load BWT index and/or packed reference
 * @param which    load BWA_IDX_BWT and/or BWA_IDX_BNS and/or BWA_IDX_PAC
 * @param bwaidx_t (out) include BWT, bns and pac
 */
bwaidx_t *bwa_idx_load_from_disk(const char *hint, int which) {
  bwaidx_t *idx;
  char *prefix;
  prefix = bwa_idx_infer_prefix(hint);
  if (prefix == 0) {
    if (bwa_verbose >= 1) fprintf(stderr, "[E::%s] fail to locate the index files\n", __func__);
    return 0;
  }
  idx = calloc(1, sizeof(bwaidx_t));
  if (which & BWA_IDX_BWT) {
    bwa_idx_load_bwt(hint, 1, idx->bwt+1); /* parent strand */
    bwa_idx_load_bwt(hint, 0, idx->bwt);   /* daughter strand */
  }
  if (which & BWA_IDX_BNS) {
    int i, c;
    idx->bns = bns_restore(prefix);
    for (i = c = 0; i < idx->bns->n_seqs; ++i)
      if (idx->bns->anns[i].is_alt) ++c;
    if (bwa_verbose >= 3)
      fprintf(stderr, "[M::%s] read %d ALT contigs\n", __func__, c);
    if (which & BWA_IDX_PAC) {
      idx->pac = calloc(idx->bns->l_pac/4+1, 1);
      err_fread_noeof(idx->pac, 1, idx->bns->l_pac/4+1, idx->bns->fp_pac); // concatenated 2-bit encoded sequence
      err_fclose(idx->bns->fp_pac);
      idx->bns->fp_pac = 0;
    }
  }
  free(prefix);
  return idx;
}

bwaidx_t *bwa_idx_load(const char *hint, int which) {
  return bwa_idx_load_from_disk(hint, which);
}

void bwa_idx_destroy(bwaidx_t *idx) {
  if (idx == 0) return;
  if (idx->mem == 0) {
    free(idx->bwt[0].sa); free(idx->bwt[0].bwt);
    free(idx->bwt[1].sa); free(idx->bwt[1].bwt);
    /* if (idx->bwt_par) bwt_destroy(idx->bwt_par); */
    /* if (idx->bwt_dau) bwt_destroy(idx->bwt_dau); */
    if (idx->bns) bns_destroy(idx->bns);
    if (idx->pac) free(idx->pac);
  } else {
    /* free(idx->bwt_par); free(idx->bwt_dau); */
    free(idx->bns->anns); free(idx->bns);
    if (!idx->is_shm) free(idx->mem);
  }
  free(idx);
}

int bwa_mem2idx(int64_t l_mem, uint8_t *mem, bwaidx_t *idx) {

  int64_t k = 0, x;
  int i;

  // generate idx->bwt
  /* biscuit shared memory might not work */
  x = sizeof(bwt_t); memcpy(idx->bwt, mem + k, x); k += x;
  x = idx->bwt[0].bwt_size * 4; idx->bwt[0].bwt = (uint32_t*)(mem + k); k += x;
  x = idx->bwt[0].n_sa * sizeof(bwtint_t); idx->bwt[0].sa = (bwtint_t*)(mem + k); k += x;
  x = sizeof(bwt_t); memcpy(idx->bwt, mem + k, x); k += x;
  x = idx->bwt[1].bwt_size * 4; idx->bwt[1].bwt = (uint32_t*)(mem + k); k += x;
  x = idx->bwt[1].n_sa * sizeof(bwtint_t); idx->bwt[1].sa = (bwtint_t*)(mem + k); k += x;

  // generate idx->bns and idx->pac
  x = sizeof(bntseq_t); idx->bns = malloc(x); memcpy(idx->bns, mem + k, x); k += x;
  x = idx->bns->n_holes * sizeof(bntamb1_t); idx->bns->ambs = (bntamb1_t*)(mem + k); k += x;
  x = idx->bns->n_seqs  * sizeof(bntann1_t); idx->bns->anns = malloc(x); memcpy(idx->bns->anns, mem + k, x); k += x;
  for (i = 0; i < idx->bns->n_seqs; ++i) {
    idx->bns->anns[i].name = (char*)(mem + k); k += strlen(idx->bns->anns[i].name) + 1;
    idx->bns->anns[i].anno = (char*)(mem + k); k += strlen(idx->bns->anns[i].anno) + 1;
  }
  idx->pac = (uint8_t*)(mem + k); k += idx->bns->l_pac/4+1;
  assert(k == l_mem);
  
  idx->l_mem = k; idx->mem = mem;
  return 0;
}

int bwa_idx2mem(bwaidx_t *idx) {
  int i;
  int64_t k, x, tmp;
  uint8_t *mem;

  /* TODO: not quite sure how this should adapt for biscuit */
  // copy idx->bwt
  x = idx->bwt->bwt_size * 4;
  mem = realloc(idx->bwt->bwt, sizeof(bwt_t) + x); idx->bwt->bwt = 0;
  memmove(mem + sizeof(bwt_t), mem, x);
  memcpy(mem, idx->bwt, sizeof(bwt_t)); k = sizeof(bwt_t) + x;
  x = idx->bwt->n_sa * sizeof(bwtint_t); mem = realloc(mem, k + x); memcpy(mem + k, idx->bwt->sa, x); k += x;
  free(idx->bwt->sa);
  /* free(idx->bwt); idx->bwt = 0; */

  // copy idx->bns
  tmp = idx->bns->n_seqs * sizeof(bntann1_t) + idx->bns->n_holes * sizeof(bntamb1_t);
  for (i = 0; i < idx->bns->n_seqs; ++i) // compute the size of heap-allocated memory
    tmp += strlen(idx->bns->anns[i].name) + strlen(idx->bns->anns[i].anno) + 2;
  mem = realloc(mem, k + sizeof(bntseq_t) + tmp);
  x = sizeof(bntseq_t); memcpy(mem + k, idx->bns, x); k += x;
  x = idx->bns->n_holes * sizeof(bntamb1_t); memcpy(mem + k, idx->bns->ambs, x); k += x;
  free(idx->bns->ambs);
  x = idx->bns->n_seqs * sizeof(bntann1_t); memcpy(mem + k, idx->bns->anns, x); k += x;
  for (i = 0; i < idx->bns->n_seqs; ++i) {
    x = strlen(idx->bns->anns[i].name) + 1; memcpy(mem + k, idx->bns->anns[i].name, x); k += x;
    x = strlen(idx->bns->anns[i].anno) + 1; memcpy(mem + k, idx->bns->anns[i].anno, x); k += x;
    free(idx->bns->anns[i].name); free(idx->bns->anns[i].anno);
  }
  free(idx->bns->anns);

  // copy idx->pac
  x = idx->bns->l_pac/4+1;
  mem = realloc(mem, k + x);
  memcpy(mem + k, idx->pac, x); k += x;
  free(idx->bns); idx->bns = 0;
  free(idx->pac); idx->pac = 0;

  return bwa_mem2idx(k, mem, idx);
}

/***********************
 * SAM header routines *
 ***********************/
#define bntann1_lt(a,b) (strcmp((a)->name,(b)->name)<0)
typedef bntann1_t *ksbntann1_t; /* because pointer will get confused */
KSORT_INIT(bntann1, ksbntann1_t, bntann1_lt);

void bwa_print_sam_hdr(const bntseq_t *bns, const char *hdr_line) {
  int i, n_SQ = 0;
  extern char *bwa_pg;
  /* header line may contain the sequence information */
  if (hdr_line) {
    const char *p = hdr_line;
    while ((p = strstr(p, "@SQ\t")) != 0) {
      if (p == hdr_line || *(p-1) == '\n') ++n_SQ;
      p += 4;
    }
  }
  if (n_SQ == 0) {

    /* print sequence info from index */
    bntann1_t **annps = malloc(bns->n_seqs*sizeof(bntann1_t*));
    for (i=0; i<bns->n_seqs; ++i) annps[i] = bns->anns+i;
    ks_introsort(bntann1, bns->n_seqs, annps);
    for (i=0; i<bns->n_seqs; ++i) {
      err_printf("@SQ\tSN:%s\tLN:%d\n", annps[i]->name, annps[i]->len);
    }
    free(annps);

    /* for (i = 0; i < bns->n_seqs; ++i) { */
    /*   /\* if (!bns->anns[i].bsstrand)       /\\* bisulfite adaption *\\/ *\/ */
    /*   err_printf("@SQ\tSN:%s\tLN:%d\n", bns->anns[i].name, bns->anns[i].len); */
    /* } */
  } else if (n_SQ != bns->n_seqs && bwa_verbose >= 2) /* sequences in the header line on command option does not match index */
    fprintf(stderr, "[W::%s] %d @SQ lines provided with -H; %d sequences in the index. Continue anyway.\n", __func__, n_SQ, bns->n_seqs);
  if (hdr_line) err_printf("%s\n", hdr_line);
  if (bwa_pg) err_printf("%s\n", bwa_pg);
}

static char *bwa_escape(char *s) {
  char *p, *q;
  for (p = q = s; *p; ++p) {
    if (*p == '\\') {
      ++p;
      if (*p == 't') *q++ = '\t';
      else if (*p == 'n') *q++ = '\n';
      else if (*p == 'r') *q++ = '\r';
      else if (*p == '\\') *q++ = '\\';
    } else *q++ = *p;
  }
  *q = '\0';
  return s;
}

char *bwa_set_rg(const char *s) {
  char *p, *q, *r, *rg_line = 0;
  memset(bwa_rg_id, 0, 256);
  if (strstr(s, "@RG") != s) {
    if (bwa_verbose >= 1) fprintf(stderr, "[E::%s] the read group line is not started with @RG\n", __func__);
    goto err_set_rg;
  }
  rg_line = strdup(s);
  bwa_escape(rg_line);
  if ((p = strstr(rg_line, "\tID:")) == 0) {
    if (bwa_verbose >= 1) fprintf(stderr, "[E::%s] no ID at the read group line\n", __func__);
    goto err_set_rg;
  }
  p += 4;
  for (q = p; *q && *q != '\t' && *q != '\n'; ++q);
  if (q - p + 1 > 256) {
    if (bwa_verbose >= 1) fprintf(stderr, "[E::%s] @RG:ID is longer than 255 characters\n", __func__);
    goto err_set_rg;
  }
  for (q = p, r = bwa_rg_id; *q && *q != '\t' && *q != '\n'; ++q)
    *r++ = *q;
  return rg_line;

err_set_rg:
  free(rg_line);
  return 0;
}

char *bwa_insert_header(const char *s, char *hdr) {
  int len = 0;
  if (s == 0 || s[0] != '@') return hdr;
  if (hdr) {
    len = strlen(hdr);
    hdr = realloc(hdr, len + strlen(s) + 2);
    hdr[len++] = '\n';
    strcpy(hdr + len, s);
  } else hdr = strdup(s);
  bwa_escape(hdr + len);
  return hdr;
}

void bseq1_code_nt4(bseq1_t *s) {
  int i;
  for (i=0; i<s->l_seq; ++i)
    s->seq[i] = nst_nt4_table[(int)s->seq[i]];
}

// create one seq
bseq1_t *bis_create_bseq1(char *seq1, char *seq2, int *n) {
  bseq1_t *s2 = calloc(seq2 ? 2 : 1, sizeof(bseq1_t));
  s2[0].name = strdup("inputread");
  s2[0].seq = (unsigned char*) seq1;
  s2[0].l_seq = strlen(seq1);
  bseq1_code_nt4(&s2[0]);
  *n = 1;
  if (seq2) {
    s2[1].name = strdup("inputread");
    s2[1].seq = (unsigned char*) seq2;
    s2[1].l_seq = strlen(seq2);
    bseq1_code_nt4(&s2[1]);
    ++(*n);
  }
  return s2;
}

static void bis_kseq2bseq1(const kseq_t *ks, bseq1_t *s)
{ // TODO: it would be better to allocate one chunk of memory, but probably it does not matter in practice
  s->name = strdup(ks->name.s);
  s->comment = ks->comment.l? strdup(ks->comment.s) : 0;
  s->seq = (uint8_t*) strdup(ks->seq.s);
  s->qual = ks->qual.l? strdup(ks->qual.s) : 0;
  s->l_seq = strlen((char*)s->seq);

  /* bisulfite note: here I convert all base to nst_nt4 */
  /* s->bisseq = calloc(s->l_seq, sizeof(char)); /\* no sentinel \0 *\/ */
  s->bisseq[0] = 0;
  s->bisseq[1] = 0;

  bseq1_code_nt4(s);
}

bseq1_t *bis_bseq_read(int chunk_size, int *n_, void *ks1_, void *ks2_) {

  kseq_t *ks = (kseq_t*)ks1_, *ks2 = (kseq_t*)ks2_;
  int size = 0, m, n;
  bseq1_t *seqs;
  m = n = 0; seqs = 0;
  while (kseq_read(ks) >= 0) {
    if (ks2 && kseq_read(ks2) < 0) { // the 2nd file has fewer reads
      fprintf(stderr, "[W::%s] the 2nd file has fewer sequences.\n", __func__);
      break;
    }
    if (n >= m) {
      m = m? m<<1 : 256;
      seqs = realloc(seqs, m * sizeof(bseq1_t));
    }
    trim_readno(&ks->name);
    bis_kseq2bseq1(ks, &seqs[n]);
    size += seqs[n++].l_seq;
    if (ks2) {
      trim_readno(&ks2->name);
      bis_kseq2bseq1(ks2, &seqs[n]);
      size += seqs[n++].l_seq;
    }
    if (size >= chunk_size && (n&1) == 0) break;
  }
  if (size == 0) { // test if the 2nd file is finished
    if (ks2 && kseq_read(ks2) >= 0)
      fprintf(stderr, "[W::%s] the 1st file has fewer sequences.\n", __func__);
  }
  *n_ = n;
  return seqs;
}
