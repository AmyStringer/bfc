#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "htab.h"
#include "khash.h"

#define _cnt_eq(a, b) ((a)>>14 == (b)>>14)
#define _cnt_hash(a) ((a)>>14)
KHASH_INIT(cnt, uint64_t, char, 0, _cnt_hash, _cnt_eq)
typedef khash_t(cnt) cnthash_t;

struct bfc_ch_s {
	int k;
	cnthash_t **h;
	// private
	int l_pre;
};

bfc_ch_t *bfc_ch_init(int k, int l_pre)
{
	bfc_ch_t *ch;
	int i;
	if (k * 2 - l_pre > BFC_CH_KEYBITS)
		l_pre = k * 2 - BFC_CH_KEYBITS;
	ch = calloc(1, sizeof(bfc_ch_t));
	ch->k = k, ch->l_pre = l_pre;
	ch->h = calloc(1<<ch->l_pre, sizeof(void*));
	for (i = 0; i < 1<<ch->l_pre; ++i)
		ch->h[i] = kh_init(cnt);
	return ch;
}

void bfc_ch_destroy(bfc_ch_t *ch)
{
	int i;
	if (ch == 0) return;
	for (i = 0; i < 1<<ch->l_pre; ++i)
		kh_destroy(cnt, ch->h[i]);
	free(ch->h); free(ch);
}

static inline cnthash_t *get_subhash(const bfc_ch_t *ch, const uint64_t x[2], uint64_t *key)
{
	if (ch->k <= 32) {
		int t = ch->k * 2 - ch->l_pre;
		uint64_t z = x[0] << ch->k | x[1];
		*key = (z & ((1ULL<<t) - 1)) << 14 | 1;
		return ch->h[z>>t];
	} else {
		int t = ch->k - ch->l_pre;
		*key = ((x[0] & ((1ULL<<t) - 1)) << ch->k | x[1]) << 14 | 1;
		return ch->h[x[0]>>t];
	}
}

int bfc_ch_insert(bfc_ch_t *ch, const uint64_t x[2], int is_high, int forced)
{
	int absent;
	uint64_t key;
	cnthash_t *h;
	khint_t k;
	h = get_subhash(ch, x, &key);
	if (__sync_lock_test_and_set(&h->lock, 1)) {
		if (forced) // then wait until the hash table is unlocked by the thread using it
			while (__sync_lock_test_and_set(&h->lock, 1))
				while (h->lock); // lock
		else return -1;
	}
	k = kh_put(cnt, h, key, &absent);
	if (absent) {
		if (is_high) kh_key(h, k) |= 1<<8;
	} else {
		if ((kh_key(h, k) & 0xff) != 0xff) ++kh_key(h, k);
		if (is_high && (kh_key(h, k) >> 8 & 0x3f) != 0x3f) kh_key(h, k) += 1<<8;
	}
	__sync_lock_release(&h->lock); // unlock
	return 0;
}

int bfc_ch_get(const bfc_ch_t *ch, const uint64_t x[2])
{
	uint64_t key;
	cnthash_t *h;
	khint_t itr;
	h = get_subhash(ch, x, &key);
	itr = kh_get(cnt, h, key);
	return itr == kh_end(h)? -1 : kh_key(h, itr) & 0x3fff;
}

uint64_t bfc_ch_count(const bfc_ch_t *ch)
{
	int i;
	uint64_t cnt = 0;
	for (i = 0; i < 1<<ch->l_pre; ++i)
		cnt += kh_size(ch->h[i]);
	return cnt;
}

int bfc_ch_hist(const bfc_ch_t *ch, uint64_t cnt[256], uint64_t high[64])
{
	int i, max_i = -1;
	uint64_t max;
	memset(cnt, 0, 256 * 8);
	memset(high, 0, 64 * 8);
	for (i = 0; i < 1<<ch->l_pre; ++i) {
		khint_t k;
		cnthash_t *h = ch->h[i];
		for (k = 0; k != kh_end(h); ++k)
			if (kh_exist(h, k))
				++cnt[kh_key(h, k) & 0xff], ++high[kh_key(h, k)>>8 & 0x3f];
	}
	for (i = 0, max = 0; i < 256; ++i)
		if (cnt[i] > max)
			max = cnt[i], max_i = i;
	return max_i;
}

int bfc_ch_dump(const bfc_ch_t *ch, const char *fn)
{
	FILE *fp;
	uint32_t t[2];
	int i;
	if ((fp = strcmp(fn, "-")? fopen(fn, "wb") : stdout) == 0) return -1;
	t[0] = ch->k, t[1] = ch->l_pre;
	fwrite(t, 4, 2, fp);
	for (i = 0; i < 1<<ch->l_pre; ++i) {
		cnthash_t *h = ch->h[i];
		khint_t k;
		t[0] = kh_n_buckets(h), t[1] = kh_size(h);
		fwrite(t, 4, 2, fp);
		for (k = 0; k < kh_end(h); ++k)
			if (kh_exist(h, k))
				fwrite(&kh_key(h, k), 8, 1, fp);
	}
	fprintf(stderr, "[M::%s] dumpped the hash table to file '%s'.\n", __func__, fn);
	fclose(fp);
	return 0;
}

bfc_ch_t *bfc_ch_restore(const char *fn)
{
	FILE *fp;
	uint32_t t[2];
	int i, j, absent;
	bfc_ch_t *ch;

	if ((fp = fopen(fn, "rb")) == 0) return 0;
	fread(t, 4, 2, fp);
	ch = bfc_ch_init(t[0], t[1]);
	assert((int)t[1] == ch->l_pre);
	for (i = 0; i < 1<<ch->l_pre; ++i) {
		cnthash_t *h = ch->h[i];
		fread(t, 4, 2, fp);
		kh_resize(cnt, h, t[0]);
		for (j = 0; j < t[1]; ++j) {
			uint64_t key;
			fread(&key, 8, 1, fp);
//			if ((key&0xff) > 2. * (key>>8&0x3f) && (key>>8&0x3f) <= 5)
//				key &= ~(0x3fULL<<8);
//			if ((key&0xff) > 15 && (key>>8&0x3f) == 0)
//				key &= ~0x3fffULL;
			kh_put(cnt, h, key, &absent);
			assert(absent);
		}
	}
	fclose(fp);
	fprintf(stderr, "[M::%s] restored the hash table from file '%s'.\n", __func__, fn);
	return ch;
}

int bfc_ch_get_k(const bfc_ch_t *ch)
{
	return ch->k;
}

typedef struct {
	uint64_t h0:56, ch:8;
	uint64_t h1:56, cl:6, absent:2;
} bfc_kcelem_t;

#define kc_hash(a) ((a).h0 + (a).h1)
#define kc_equal(a, b) ((a).h0 == (b).h0 && (a).h1 == (b).h1)
KHASH_INIT(kc, bfc_kcelem_t, char, 0, kc_hash, kc_equal)

bfc_kc_t *bfc_kc_init(void) { return kh_init(kc); }
void bfc_kc_destroy(bfc_kc_t *kc) { kh_destroy(kc, kc); }
void bfc_kc_clear(bfc_kc_t *kc) { kh_clear(kc, kc); }

int bfc_kc_get(const bfc_ch_t *ch, bfc_kc_t *kc, const bfc_kmer_t *z)
{
	int r;
	uint64_t x[2];
	bfc_kmer_hash(ch->k, z->x, x);
	if (kc) {
		khint_t k;
		int absent;
		bfc_kcelem_t key, *p;
		key.h0 = x[0], key.h1 = x[1];
		key.ch = key.cl = key.absent = 0; // no effect; only to suppress gcc warnings
		k = kh_put(kc, kc, key, &absent);
		p = &kh_key(kc, k);
		if (absent) {
			r = bfc_ch_get(ch, x);
			if (r >= 0) p->ch = r&0xff, p->cl = r>>8&0x3f, p->absent = 0;
			else p->ch = p->cl = 0, p->absent = 1;
		} else r = p->absent? -1 : p->cl<<8 | p->ch;
	} else r = bfc_ch_get(ch, x);
	return r;
}
