#include "pcm.h"
#include <time.h>
#include <stdlib.h>
#include <getopt.h>
#include <assert.h>

int PCM_NUM_BANKS = PCM_NUM_BANKS_MAX;
int PCM_ROWS_PER_BANK = PCM_ROWS_PER_BANK_MAX;
int PCM_ENABLE_OPENMP = 0;

void pcm_param(int argc, char* argv[], char* usage) {
	int option = 0;
	opterr = 0;
	optind = 1;
	while ((option = getopt(argc, argv,"b:r:mh")) != -1) {
		switch (option) {
			case 'm' : PCM_ENABLE_OPENMP = 1;
				   break;
			case 'b' : PCM_NUM_BANKS = atoi(optarg);
				   break;
			case 'r' : PCM_ROWS_PER_BANK = atoi(optarg);
				   break;
			case 'h' : printf("%s",
						   "pcm param:\n"
						   "-m\t\topenmp flag\n"
						   "-b <num_banks>\tthe number of banks\n"
						   "-r <num_rows>\tthe number of rows in banks\n"
						   "\n"
						   "*** pcm params need to be set before any other params ***\n"
						   "\n"
					 );
				   if(usage != NULL) printf("%s", usage);
				   exit(-1);
			case '?':
			case 0: goto pcm_param_return;
		}
	}

pcm_param_return:
	if( PCM_NUM_BANKS > PCM_NUM_BANKS_MAX ) {
		printf("PCM_NUM_BANKS_MAX: %d\n", PCM_NUM_BANKS_MAX);
		exit(-1);
	}

	if( PCM_ROWS_PER_BANK > PCM_ROWS_PER_BANK_MAX) {
		printf("PCM_ROWS_PER_BANK_MAX: %lu\n", PCM_ROWS_PER_BANK_MAX);
		exit(-1);
	}

	printf("PCM_NUM_ROWS: %lu;\nPCM_NUM_BANKS: %d;\nPCM_ROWS_PER_BANK: %d;\nPCM_SIZE: %luMB;\n", PCM_NUM_ROWS, PCM_NUM_BANKS, PCM_ROWS_PER_BANK, PCM_SIZE/(1024*1024));
	optind = 1;
}

void *pcm_thread_func(void *data)
{
	struct pcm_thread *pcm_thread = (struct pcm_thread *) data;

	int i;

	if(pcm_thread->sort_odd != NULL) {
		assert(pcm_thread->num_rows % 2 == 0);
		for(i = 1; i < pcm_thread->num_rows; i += 2){
			pcm_thread->sort_odd(pcm_thread->rows[i], pcm_thread->rows[i + 1]);
		}
	} else if(pcm_thread->sort_even != NULL){
		assert(pcm_thread->num_rows % 2 == 0);
		for(i = 0; i < pcm_thread->num_rows; i += 2){
			pcm_thread->sort_even(pcm_thread->rows[i], pcm_thread->rows[i + 1]);
		}
	} else {
		for(i = 0; i < pcm_thread->num_rows; i++){
			if(pcm_thread->count_fn != NULL){
				pcm_thread->count += pcm_thread->count_fn(pcm_thread->rows[i]);
			} else if(pcm_thread->fn != NULL){
				pcm_thread->fn(pcm_thread->rows[i]);
			}
		}
	}

	if(!PCM_ENABLE_OPENMP) {
		pthread_exit(0);
	}
}

void pcm_threads_run(struct pcm_thread * pcm_threads, int num_threads){
	int i;
	if(PCM_ENABLE_OPENMP) {
#pragma omp parallel for num_threads(num_threads)
		for(i = 0; i < num_threads; i++){
			pcm_thread_func(pcm_threads + i);
		}
	} else {
		for(i = 0; i < num_threads; i++){
			struct pcm_thread * pth = pcm_threads + i;
			pthread_create(&pth->pthread, NULL, pcm_thread_func, pth);
		}
		for (i = 0; i < num_threads; i++) {
			struct pcm_thread * pth = pcm_threads + i;
			pthread_join(pth->pthread, NULL);
		}
	}
}

void pcm_thread_add_row(struct pcm_thread * pth, void * base, int row) {
    pth->rows[pth->num_rows] = PCM_R2P(base, row);
    pth->num_rows++;
}

void pcm_threads_map_fn(
		struct pcm_thread pcm_threads[], int num_threads,
		void (* fn)(void *row)
		) {
	int i;
	for(i = 0; i < num_threads; i++) {
		pcm_threads[i].fn = fn;
	}
}

void pcm_threads_map_count_fn(
		struct pcm_thread pcm_threads[], int num_threads,
		unsigned long (* count_fn)(void *row)
		) {
	int i;
	for(i = 0; i < num_threads; i++) {
		pcm_threads[i].count_fn = count_fn;
		pcm_threads[i].count = 0;
	}
}

void pcm_threads_reduce_count_fn(
		struct pcm_thread pcm_threads[], int num_threads,
		void (* count_reduce)(unsigned long count)
		){
	int i;
	for(i = 0; i < num_threads; i++) {
		count_reduce(pcm_threads[i].count);
	}
}

void pcm_rows_shuffle(int rows[], int num_rows) {
	int r;
	srand(time(NULL));
	for(r = 0; r < num_rows; r++) {
		int swap = rand() % num_rows;
		int tmp = rows[r];
		rows[r] = rows[swap];
		rows[swap] = tmp;
	}
}

void pcm_rows_bank_aware_shuffle(int rows[], int num_rows) {
	int r;
	srand(time(NULL));
	for(r = 0; r < num_rows; r++) {
		int swap = rand() % PCM_ROWS_PER_BANK * PCM_NUM_BANKS + PCM_R2B(r);
		int tmp = rows[r];
		rows[r] = rows[swap];
		rows[swap] = tmp;
	}
}

void pcm_rows_bank_aware_shuffle2(int rows[], int num_rows) {
	int r, b;

	for (b = 0; b < PCM_NUM_BANKS; b++) {
		for(r = 0; r < PCM_ROWS_PER_BANK; r++) {
			rows[b * PCM_ROWS_PER_BANK + r] = r * PCM_NUM_BANKS + b;
		}
	}
}

void pcm_r2t_contention_free(struct pcm_thread pths[], int num_threads, int rows[], int num_rows, void * buf) {
	int r, t;
	for(t = 0; t < num_threads; t++) {
		pths[t].num_rows = 0;
	}
	if(PCM_NUM_BANKS >= num_threads) {
		for(r = 0; r < num_rows; r++) {
			t = PCM_R2B(r) % num_threads;
			pcm_thread_add_row(pths + t, buf, rows[r]);
		}
	} else {
		int threads_per_bank = num_threads / PCM_NUM_BANKS;
		int rows_in_bank[PCM_NUM_BANKS];
		for(r = 0; r < num_rows; r++) {
			int b = PCM_R2B(r);
			int off = rows_in_bank[b] % threads_per_bank;
			t = b * threads_per_bank + off;
			pcm_thread_add_row(pths + t, buf, rows[r]);
			rows_in_bank[b]++;
		}
	}
}

void pcm_r2t_even_split(struct pcm_thread pths[], int num_threads, int rows[], int num_rows, void * buf) {
	int rows_in_thread = num_rows / num_threads;
	int left_rows = num_rows % num_threads;

	int r, t;
	for(t = 0; t < num_threads; t++) {
		pths[t].num_rows = 0;
		for(r = 0; r < rows_in_thread; r++) {
			pcm_thread_add_row(pths + t, buf, rows[t * rows_in_thread + r]);
		}
	}
	for(r = 0; r < left_rows; r++) {
		pcm_thread_add_row(pths + r, buf, rows[num_threads * rows_in_thread + r]);
	}
}

void pcm_print_row_shuffle(int rows[])
{
	int r;

	for (r = 0; r < PCM_NUM_ROWS; r++)
		fprintf(stderr, "row[%d]: %d (%d->%d)\n", r, rows[r], r % PCM_NUM_BANKS, rows[r] % PCM_NUM_BANKS);
}

void pcm_thread_print(struct pcm_thread pcm_threads[], int num_threads, char* base) {
	int t, r;
	for (t = 0; t < num_threads; t++) {
		struct pcm_thread *pth = pcm_threads + t;
		fprintf(stderr, "thread %d: ", t);
		for (r = 0; r < pth->num_rows; r++) {
			void * row = pth->rows[r];
			fprintf(stderr, "%lu/%lu ", PCM_P2R(base, row), PCM_P2B(base, row));
		}
		fprintf(stderr, "\n");
	}
}
