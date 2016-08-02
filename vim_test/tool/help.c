#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <strings.h>
#include <sys/time.h>

/*-----------------------------------------------------------------------------------------------------------*/
static int *nrand_data;
static int nrand_max, nrand_pos;

void init_nrand(int num, int seed)
{
	int i, j, tmp;

	nrand_data = malloc(num * sizeof(int));
	assert(NULL != nrand_data);
	nrand_pos = 0;
	nrand_max = num;

	for (i = 0; i < num; i++)
		nrand_data[i] = i;

	if (seed)
		srand(seed);

	for (i = num - 1; i > 0; i--) {
		j = rand() % i;

		tmp = nrand_data[i];
		nrand_data[i] = nrand_data[j];
		nrand_data[j] = tmp;
	}

	// for (i = 0; i < num; i++)
	//	printf("%08d\n", nrand_data[i]);
}

void exit_nrand(void)
{
	free(nrand_data);
	nrand_pos = 0;
	nrand_max = 0;
}

int nrand(void)
{
	assert(nrand_pos < nrand_max);
	return nrand_data[nrand_pos++];
}

int get_nrand(int idx)
{
	return nrand_data[idx % nrand_max];
}

void uniq_rand(int num, int seed, int uniq_buffer[])
{
	int i, j, tmp;
	struct timeval tv;

	assert(NULL != uniq_buffer);

	for (i = 0; i < num; i++)
		uniq_buffer[i] = i;

	if (seed) {
		srand(seed);
	} else {
		if (gettimeofday(&tv, NULL)) {
			printf("gettimeofday failed when generate seed\n");
			exit(EXIT_FAILURE);
		}
		srand(tv.tv_usec);
	}

	for (i = num - 1; i > 0; i--) {
		j = rand() % i;

		tmp = uniq_buffer[i];
		uniq_buffer[i] = uniq_buffer[j];
		uniq_buffer[j] = tmp;
	}

	// for (i = 0; i < num; i++)
	//	printf("%04d\n", uniq_buffer[i]);
}
/*-----------------------------------------------------------------------------------------------------------*/
