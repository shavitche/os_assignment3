#include "types.h"
#include "stat.h"
#include "user.h"

int NUM_OF_PAGES = 20;

int page_size = 4096;



volatile int
test_simple_allocating_data()
{
    char* pages_matrix[NUM_OF_PAGES];

    int i;
    int j;

    for (i = 0; i < NUM_OF_PAGES ; ++i)
    {
        pages_matrix[i] = sbrk(page_size);
        printf(1, "allocated the page %d with the virtual address  %x\n", i, pages_matrix[i]);
    }

    //Use the pages
    printf(1, "Going to access the pages\n");
    for ( i = 0; i < NUM_OF_PAGES; ++i)
    {
        printf(1, "Accessing page %d\n", i);
        for ( j = 0; j < page_size; ++j)
        {
            pages_matrix[i][j] = 0;
        }
    }


    printf(1, "TEST DONE\n");
    return 0;
}



int test_data_after_fork(void){
    int array_size = 20;
    int *array[array_size];

    int i, j, k, id;
    printf(1, "Test swapping start \n");
    printf(1, "Creating a pointer to 4KB in an array of size %d\n", array_size);
    for (i = 0; i < array_size; ++i){
        array[i] = (void *) sbrk(page_size);
    }
    printf(1, "Done allocating\n");

    printf(1, "Now we will fill the array with numbers\n");
    for (j = 0; j < array_size; ++j) {
        *array[j] = j;
    }
    printf(1, "Done filling\n");
    printf(1, "Now we are going to fork and check in the child\n");

    if ((id = fork()) == 0) {
        printf(1, "Go over the array and see if the numbers are the same \n");
        for (k = 0; k < array_size; ++k) {
            if (*array[k] != k) {
                printf(1, "ERROR, got the number %d instead of %d \n", k, *array[k]);
                sleep(200);
                return 1;
            }
        }
    }
    else{
        wait();
        printf(1, "TEST DONE\n");
    }

    return 0;
}



int test_rewrite_data(void){
    int array_size = 20;
    int num_of_loops = 5;
    int *array[array_size];
    int l, k, i, j;

    printf(1, "Test swapping start \n");
    printf(1, "Creating a pointer to 4KB in an array of size %d\n", array_size);
    for (i = 0; i < array_size; ++i){
        array[i] = (void *) sbrk(page_size);
    }
    printf(1, "Done allocating\n");


    printf(1, "We are going to write data %d times and check if it stays the same\n", num_of_loops);
    for (l = 0; l < num_of_loops; ++l) {
        printf(1, "------------- Loop number: %d ---------------\n", l, num_of_loops);
        printf(1, "Starting to fill the array with the index number\n");
        for (j = 0; j < array_size; ++j) {
            *array[j] = j + l;
        }
        printf(1, "Go over the array and see if the numbers are the same \n");
        for (k = 0; k < array_size; ++k) {
            if (*array[k] != (k +l)) {
                printf(1, "ERROR, got the number %d instead of %d \n", k, *array[k]);
                sleep(200);
                return 1;
            }
        }
    }
    printf(1, "TEST DONE\n");
    return 0;
}



int test_create_and_swap(void){
    int array_size = 20;
    int *array[array_size];
    int i, j, k;
    printf(1, "start test test_create_swap_file \n");
    printf(1, "Creating a pointer to 4KB in an array of size %d\n", array_size);
    for (i = 0; i < array_size; ++i){
        array[i] = (void *) sbrk(page_size);
    }
    printf(1, "Done allocating\n");

    printf(1, "Now we will fill the array with numbers\n");
    for (j = 0; j < array_size; ++j) {
        *array[j] = j;
    }
    printf(1, "Done filling\n");

    printf(1, "Check if the numbers are the same\n");
    for (k = 0; k < array_size; ++k) {
        if (*array[k] != k){
            printf(1, "ERROR, got the number %d instead of %d \n", k, *array[k]);
            sleep(100);
            return 1;
        }
    }
    printf(1, "TEST DONE\n");
    return 0;
}


int main(void){
    int id;

    printf(1, "Test 1: Simple allocating data \n");

    if ((id = fork()) == 0) {
        if(test_simple_allocating_data() == 1){
            printf(1, "test simple allocating data failed \n");
        }
        exit();
    }
    else{
        wait();
    }
    printf(1, "Test 1: done\n\n\n\n");


    printf(1, "Test 2: Test create and write  \n");
    if ((id = fork()) == 0) {
        if(test_create_and_swap() == 1){
            printf(1, "test create and write failed \n");
        }
        exit();

    }
    else{
        wait();
    }
    printf(1, "Test 2: done \n\n\n\n");


    printf(1, "Test 3: Test rewriting data\n");

    if ((id = fork()) == 0) {
        if(test_rewrite_data() == 1){
            printf(1, "test rewriting data failed \n");
        }
        exit();

    }
    else{
        wait();
    }
    printf(1, "Test 3: done \n\n\n\n");



    printf(1, "Test 4: Data after forking\n");

    if ((id = fork()) == 0) {
        if(test_data_after_fork() == 1){
            printf(1, "test data after forking failed \n");
        }
        exit();

    }
    else{
        wait();
    }
    printf(1, "Test 4: data after forking done \n");






    printf(1, "ALL THE TEST ARE DONE! WOOHOO \n");
    exit();
}