#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "demod.h"
#include "interpolator.h"
#include "utils.h"
#include "wavfile.h"

typedef struct {
	Demod *self;
	const char *out_fname;
} ThrArgs;

static void* demod_thr_run(void* args);

Demod*
demod_init(Source *src, unsigned interp_mult, unsigned rrc_order, float rrc_alpha, float pll_bw, unsigned sym_rate)
{
	Demod *ret;

	ret = safealloc(sizeof(*ret));

	ret->src = src;

	/* Initialize the AGC */
	ret->agc = agc_init();

	/* Initialize the interpolator, associating raw_samp to it */
	ret->interp = interp_init(src, rrc_alpha, rrc_order, interp_mult, sym_rate);
	/* Discard the first null samples */
	ret->interp->read(ret->interp, rrc_order*interp_mult);

	/* Initialize Costas loop */
	pll_bw = 2*M_PI*pll_bw/sym_rate;
	ret->cst = costas_init(pll_bw);

	/* Initialize the timing recovery variables */
	ret->sym_rate = sym_rate;
	ret->sym_period = ret->interp->samplerate/(float)sym_rate;
	pthread_mutex_init(&ret->mutex, NULL);
	ret->bytes_out_count = 0;
	ret->thr_is_running = 1;

	return ret;
}

void
demod_start(Demod *self, const char *fname)
{
	ThrArgs *args;

	args = safealloc(sizeof(*args));

	args->out_fname = fname;
	args->self = self;

	pthread_create(&self->t, NULL, demod_thr_run, (void*)args);
}

int
demod_status(const Demod *self)
{
	return self->thr_is_running;
}

int
demod_is_pll_locked(const Demod *self)
{
	return self->cst->locked;
}

unsigned
demod_get_bytes_out(Demod *self)
{
	unsigned ret;

	pthread_mutex_lock(&self->mutex);
	ret = self->bytes_out_count;
	pthread_mutex_unlock(&self->mutex);

	return ret;
}

uint64_t
demod_get_done(const Demod *self)
{
	return self->src->done(self->src);
}

uint64_t
demod_get_size(const Demod *self)
{
	return self->src->size(self->src);
}

float
demod_get_freq(const Demod *self)
{
	return self->cst->nco_freq*self->sym_rate/(2*M_PI);
}

float
demod_get_gain(const Demod *self)
{
	return self->agc->gain;
}

/* XXX not thread-safe */
const int8_t*
demod_get_buf(const Demod *self)
{
	return self->out_buf;
}

void
demod_join(Demod *self)
{
	void* retval;

	self->thr_is_running = 0;
	pthread_join(self->t, &retval);
	pthread_mutex_destroy(&self->mutex);

	agc_free(self->agc);
	costas_free(self->cst);
	self->interp->close(self->interp);

	free(self);
}

/* Static functions {{{ */
void*
demod_thr_run(void* x)
{
	FILE *out_fd;
	int i, count, buf_offset;
	float complex before, mid, cur;
	float resync_offset, resync_error, resync_period;
	int8_t *out_buf;

	const ThrArgs *args = (ThrArgs*)x;
	Demod *self = args->self;
	out_buf = self->out_buf;

	resync_period = self->sym_period;

	if (args->out_fname) {
		if (!(out_fd = fopen(args->out_fname, "w"))) {
			fatal("Could not open file for writing");
			/* Not reached */
			return NULL;
		}
	} else {
		fatal("No output filename specified");
		/* Not reached */
		return NULL;
	}

	/* Main processing loop */
	buf_offset = 0;
	resync_offset = 0;
	before = 0;
	mid = 0;
	cur = 0;
	while (self->thr_is_running && (count = self->interp->read(self->interp, CHUNKSIZE))) {
		for (i=0; i<count; i++) {
			/* Symbol resampling */
			if (resync_offset >= resync_period/2 && resync_offset < resync_period/2+1) {
				mid = agc_apply(self->agc, self->interp->data[i]);
			} else if (resync_offset >= resync_period) {
				cur = agc_apply(self->agc, self->interp->data[i]);
				/* The current sample is in the correct time slot: process it */
				/* Calculate the symbol timing error (Gardner algorithm) */
				resync_offset -= resync_period;
				resync_error = (cimagf(cur) - cimagf(before)) * cimagf(mid);
				resync_offset += (resync_error*resync_period/2000000.0);
				before = cur;

				/* Fine frequency/phase tuning */
				cur = costas_resync(self->cst, cur);

				/* Append the new samples to the output buffer */
				out_buf[buf_offset++] = clamp(crealf(cur)/2);
				out_buf[buf_offset++] = clamp(cimagf(cur)/2);

				/* Write binary stream to file and/or to socket */
				if (buf_offset >= SYM_CHUNKSIZE - 1) {
					fwrite(out_buf, buf_offset, 1, out_fd);
					buf_offset = 0;
				}
				pthread_mutex_lock(&self->mutex);
				self->bytes_out_count += 2;
				pthread_mutex_unlock(&self->mutex);
			}
			resync_offset++;
		}
	}

	/* Write the remaining bytes */
	fwrite(out_buf, buf_offset, 1, out_fd);
	fclose(out_fd);

	free(x);
	self->thr_is_running = 0;
	return NULL;
}
/*}}}*/
