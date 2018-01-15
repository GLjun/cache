/*************************************************************************
    > File Name: cachedesign.c
    > Author: cgn
    > Func: 
    > Created Time: 日  1/14 22:31:25 2018
 ************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long long ulong;

#define CACHE_SIZE (128 << 10) // 128K
#define BLOCK_SIZE 4 //4B
#define OFFSET 2 // log2(BLOCK_SIZE)
#define LINE_MAX_LENGTH 128
#define LOAD_OP 'l'
#define STORE_OP 's'
#define INVALID 0
#define VALID 1

#define TAG(data, offset) (((uint)(data)) >> offset)
#define INDEX(data, op) (((data)&(op)) >> OFFSET)



typedef struct trace_item
{
	char load_store;
	uint data;
} trace_item;

typedef struct cache_line
{
	uchar valid;
	uint tag; // short is two small for index of 4-way SA
	//uint data;
} cache_line;


typedef struct cache_sa
{
	cache_line data[4];
} cache_sa;

char hex_array[16] = {
	'0', '1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'
};

int read_next_trace_batch(FILE* file, trace_item* traces, int n)
{
	if(n <= 0 || file == NULL || traces == NULL)
		return 0;

	char buffer[LINE_MAX_LENGTH];
	memset(buffer, 0, sizeof(char) * LINE_MAX_LENGTH);
	int cnt = 0;

	while(cnt < n && fgets(buffer, LINE_MAX_LENGTH, file))
	{
		traces[cnt].load_store = buffer[0];

		traces[cnt].data = (uint)(strtol(&buffer[2], NULL, 16));				
		++ cnt;
	}

	return cnt;
}

void itohexstr(uint number, char* str)
{
	str[0] = hex_array[(number >> 28) & 0xF];
	str[1] = hex_array[(number >> 24) & 0xF];
	str[2] = hex_array[(number >> 20) & 0xF];
	str[3] = hex_array[(number >> 16) & 0xF];
	str[4] = hex_array[(number >> 12) & 0xF];
	str[5] = hex_array[(number >> 8) & 0xF];
	str[6] = hex_array[(number >> 4) & 0xF];
	str[7] = hex_array[(number) & 0xF];
}

void write_traces(FILE* file, trace_item* traces, int n)
{
	int i;
	char buffer[LINE_MAX_LENGTH];
	memset(buffer, 0, sizeof(char) * LINE_MAX_LENGTH);
	buffer[1] = ' ';
	buffer[2] = '0';
	buffer[3] = 'x';
	buffer[12] = '\n';
	buffer[13] = '\0';
	for(i = 0;i < n; ++i)
	{
		buffer[0] = traces[i].load_store;
		itohexstr(traces[i].data, &buffer[4]);
		fputs(buffer, file);
	}
}


void direct_map_cache(FILE* input_file, FILE* output_file)
{
	int index_bits = 15;
	uint index_op = 0x0001FFFC;
	int tag_offset = index_bits + OFFSET;
	int index_size = (32 << 10);
	int nlines = 100; // number of lines of reading from input file per iteration
	int cnt_miss = 0, cnt_hit = 0, n, i;
	int cnt_output_log = 0;
	uint tag, index;

	//direct map cache
	cache_line* dmcache = (cache_line*)malloc(index_size * sizeof(cache_line));
	memset((void*)dmcache, 0, index_size * sizeof(cache_line));

	trace_item* traces = (trace_item*)malloc(nlines * sizeof(trace_item));
	trace_item* log_miss_traces = (trace_item*)malloc(nlines * sizeof(trace_item));

	while(1)
	{
		n = read_next_trace_batch(input_file, traces, nlines);
		if(n <= 0)
			break;
		for(i = 0;i < n; ++ i)
		{
			tag = TAG(traces[i].data, tag_offset);
			index = INDEX(traces[i].data, index_op);
			if(dmcache[index].valid == VALID && dmcache[index].tag == tag)
				++ cnt_hit;
			else
			{
				log_miss_traces[cnt_output_log] = traces[i];
				++ cnt_miss;
				++ cnt_output_log;
				dmcache[index].tag = tag;
				dmcache[index].valid = VALID;
			}

		}
		if(cnt_miss % nlines == 0 && cnt_miss)
		{
			write_traces(output_file, log_miss_traces, cnt_output_log);
			cnt_output_log = 0;
		}
	}
	if(cnt_output_log)
		write_traces(output_file, log_miss_traces, cnt_output_log);

	free(dmcache);
	free(traces);
	free(log_miss_traces);

	printf("cnt_hit : %d, cnt_miss: %d, hit rate: %.5f\n", cnt_hit, cnt_miss, (cnt_hit*1.0)/(cnt_hit+cnt_miss));
}


void SA_map_cache(FILE* input_file, FILE* output_file, int ways)
{
	if(ways != 2 && ways != 4)
	{
		printf("ways %d is not supported!\n", ways);
		return;
	}
	int index_bits = (ways == 2) ? 14 : 13;
	int index_size = (ways == 2) ? (16 << 10) : (8 << 10);
	uint index_op = (ways == 2) ? (0x0000FFFC) : (0x00007FFC);
	int tag_offset = index_bits + OFFSET;

	int nlines = 100;
	int cnt_miss = 0, cnt_hit = 0, cnt_output_log = 0;
	int n, i, j, k;
	uint tag, index, hit;

	//SA
	cache_sa* sacache = (cache_sa*)malloc(index_size * sizeof(cache_sa));
	memset((void*)sacache, 0, index_size * sizeof(cache_sa));

	trace_item* traces = (trace_item*)malloc(nlines * sizeof(trace_item));
	trace_item* log_miss_traces = (trace_item*)malloc(nlines * sizeof(trace_item));

	while(1)
	{
		n = read_next_trace_batch(input_file, traces, nlines);
		if(n <= 0)
			break;
		for(i = 0; i < n; ++i)
		{
			tag = TAG(traces[i].data, tag_offset);
			index = INDEX(traces[i].data, index_op);
			hit = 0;
			for(j = 0;j < ways; ++ j)
			{
				if(sacache[index].data[j].valid == VALID && sacache[index].data[j].tag == tag)
				{
					hit = 1;
					++ cnt_hit;
					k = j;
					while(k) 
					{
						sacache[index].data[k] = sacache[index].data[k-1];
						-- k;
					}
					sacache[index].data[0].valid = VALID;
					sacache[index].data[0].tag = tag;
					break;
				}
			}
			if(!hit)
			{
				log_miss_traces[cnt_output_log] = traces[i];
				++ cnt_output_log;
				++ cnt_miss;
				k = ways - 1;
				while(k)
				{
					sacache[index].data[k] = sacache[index].data[k-1];
					-- k;
				}
				sacache[index].data[0].valid = VALID;
				sacache[index].data[0].tag = tag;
			}

		}
		if(cnt_output_log % 100 == 0 && cnt_output_log)
		{
			write_traces(output_file, log_miss_traces, cnt_output_log);
			++ cnt_output_log;
		}
	}

	if(cnt_output_log)
		write_traces(output_file, log_miss_traces, cnt_output_log);

	free(sacache);
	free(traces);
	free(log_miss_traces);

	printf("ways : %d, cnt_hit : %d, cnt_miss: %d, hit rate: %.5f\n", ways, cnt_hit, cnt_miss, (cnt_hit*1.0)/(cnt_hit+cnt_miss));

}

void test_direct_map(FILE* file)
{

	fseek(file, 0, SEEK_SET);

	FILE* output_file = fopen("dm.txt", "w");

	direct_map_cache(file, output_file);

	fclose(output_file);
}

void test_sa_map(FILE* file)
{
	fseek(file, 0, SEEK_SET);
	FILE* output_file = fopen("sa2.txt", "w");
	SA_map_cache(file, output_file, 2);
	fclose(output_file);

	fseek(file, 0, SEEK_SET);
	output_file = fopen("sa4.txt", "w");
	SA_map_cache(file, output_file, 4);
	fclose(output_file);
	
}

int main(int argc, char* argv[])
{

	if(argc < 2)
	{
		printf("usage : execute filename\n");
		return 0;
	}
	FILE* f = fopen(argv[1], "r");
	if(!f)
	{
		printf("file does not existes!");
		return 0;
	}

	//test_direct_map(f);
	test_sa_map(f);

	fclose(f);
	return 0;
}
