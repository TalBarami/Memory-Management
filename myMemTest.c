#include "types.h"
#include "stat.h"
#include "user.h"
#include "syscall.h"

#define PGSIZE 4096
#define MAX_PHYS_PAGES 16

void
system_pause(){
	char input[5];
	printf(1, "Press enter to continue...\n");
	gets(input, 5);
}

void
allocate_max_phys_pages(char** arr){
	int i;
	arr[0] = "0";
	arr[1] = "1000";
	arr[2] = "2000";
	for (i = 3; i < MAX_PHYS_PAGES; ++i) {
		arr[i] = sbrk(PGSIZE);
		printf(1, "Allocate new page %d (address: %x).\n", i, arr[i]);
	}
	printf(1, "All %d physical pages have been allocated.\n", MAX_PHYS_PAGES);
}


void
test(){
    char *memory_data[MAX_PHYS_PAGES + 3];
 	system_pause();
   
    allocate_max_phys_pages(memory_data);
    system_pause();
 
    printf(1, "Allocate page #%d\n", MAX_PHYS_PAGES);
    memory_data[MAX_PHYS_PAGES] = sbrk(PGSIZE);
    system_pause();
 
    printf(1, "Access page #%d (address %x)\n", 3, memory_data[3]);
    memory_data[3][1] = 'a';
    system_pause();
 
    printf(1, "Allocate page #%d.\n", MAX_PHYS_PAGES+1);
    memory_data[MAX_PHYS_PAGES + 1] = sbrk(PGSIZE);
    system_pause();
 
    printf(1, "Access pages %d-%d.\n", 4, 11);
    for (int i = 4; i <= 11; i++) {
            memory_data[i][1] = 'b';
    }
    system_pause();
 
    printf(1, "Access page #%d.\n", 4);
    memory_data[4][1] = 'c';
    system_pause();
 
    printf(1, "Allocate page #%d.\n", MAX_PHYS_PAGES + 2);
    memory_data[MAX_PHYS_PAGES + 2] = sbrk(PGSIZE);
    system_pause();
 
    printf(1, "Calling fork.\n");
    if (fork() == 0) {
        system_pause();
 
        printf(1, "Access page #%d.\n", 13);
        memory_data[13][0] = 'd';
        system_pause();
 
        exit();
    }
    else {
        wait();
    }
}


void
testNONE(){
	char* arr[50];
	for (int i = 0; i < 50; i++) {
		arr[i] = sbrk(PGSIZE);
		printf(1, "Allocate new page %d (address: %x).\n", i, arr[i]);
		
	}
	system_pause();
}

int
main(int argc, char *argv[]){
	#ifdef NFUA
	printf(1, "Testing NFUA\n");
	#elif LAPA
	printf(1, "Testing LAPA\n");
	#elif SCIFIFO
	printf(1, "Testing SCIFIFO\n");
	#elif AQ
	printf(1, "Testing AQ\n");
	#elif NONE
	printf(1, "Testing NONE\n");
	#endif
	
	#ifndef NONE
	test();
	#else
	testNONE();
	#endif

	exit();
}
