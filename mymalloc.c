#define _DEFAULT_SOURCE
#define _BSD_SOURCE 
#include <malloc.h>
#include <stdio.h>
#include <debug.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>

// Structure for a block header for a chunk of memory.
typedef struct block {
  size_t size;        // How many bytes beyond this block have been allocated in the heap.
  struct block *next; // The next block in the memory linked list.
  int free;           // Whether the memory is free or in use.
} block_t;

// Function declarations.
void *init_mem(size_t s);
void *nextBlock(size_t s);
void *expandHeap(size_t s, block_t *last);
void *allocMap(size_t s, block_t *last);
void *mymalloc(size_t s);
void *mycalloc(size_t nmemb, size_t s);
void myfree(void *ptr);

// Size of the header block.
#define BLOCK_SIZE sizeof(block_t)

// Size of a page in memory
#define PAGE_SIZE sysconf(_SC_PAGE_SIZE)

// Global variable to define the head of our linked list.
static block_t *head = 0;

// Mutex locks in order to preserve the program with multi-threading.
pthread_mutex_t memChange = PTHREAD_MUTEX_INITIALIZER; // Ensures only one thread can edit memory at a time.
pthread_mutex_t listChange = PTHREAD_MUTEX_INITIALIZER; // Ensures only one thread can edit our linked list at a time.

// Initializes the memory tracking by setting up our head in the heap. Runs once when mymalloc is called for the first time.
void *init_mem(size_t s) {
  pthread_mutex_lock(&memChange); // Locks the memory mutex when using sbrk and editing memory.

  // Sets the current pointer to the location of the program break.
  void *pt = sbrk(0);
  
  // Returns NULL if sbrk has failed, otherwise moves the program break.
  if (sbrk(s + BLOCK_SIZE) == (void *) -1) {
    return NULL;
  }
  pthread_mutex_unlock(&memChange);

  pthread_mutex_lock(&listChange); // Locks the list change mutex when initializing the head.
  // Sets the appropriate metadata for the initial header memory block.
  head = (block_t *) pt; // Casts as a block_t from void.
  head->size = s;
  head->free = 0;
  head->next = NULL; // No other blocks in front of this current block on creation.
  pthread_mutex_unlock(&listChange);

  return head;
}

// Returns the next available block to be used for the given size. Allocates more memory if none is found that matches.
void *nextBlock(size_t s) {
  pthread_mutex_lock(&listChange); // Locks the list change mutex when traversing the linked list (prevents any edits while traversing)
  block_t *i = head; // Iterator for the linked list.

  // Iterates through the linked list until it reaches the last block before NULL.
  while (i->next != NULL) {
    // Next block is free & has exact size needed.
    if (i->free && i->size >= s) {
      i->free = 0; // Sets to be no longer free.
      pthread_mutex_unlock(&listChange);
      return i;
    }

    i = i->next; // Increment to next block
  }

  // The following conditional will preform the same free and size check on the last block in the linked list.
  // Checks if the last block is free and available in size.
  if (i->free && i->size >= s) {
    i->free = 0; // Sets to be no longer free.
    pthread_mutex_unlock(&listChange);
    return i;
  }
  pthread_mutex_unlock(&listChange);

  // If our allocation + meta is greater than the page size, use mmap to allocate and return.
  if (s + BLOCK_SIZE >= PAGE_SIZE) {
    return allocMap(s, i);
  }

  // Otherwise, expand the heap to account for our new allocation and return a new block of correct size.
  return expandHeap(s, i);
}

// When the requested memory is larger or equal to a page, use mmap to allocate and return the requested memory (will split if extra left over).
void *allocMap(size_t s, block_t *last) {
  pthread_mutex_lock(&memChange); // Locks the memory change mutex when allocating memory.
  // Retreives the requested memory in units of page size.
  void *pt = mmap(NULL, s + BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  pthread_mutex_unlock(&memChange);

  if (pt == MAP_FAILED) { // If mmap has failed.
    return NULL;
  }

  int totPage = (s + BLOCK_SIZE) / PAGE_SIZE; // The total amount of memory returned by mmap.

  if ((s + BLOCK_SIZE) % PAGE_SIZE != 0) { // If the requested memory doesn't go in evenly, add a full new page (behavior of mmap).
    totPage++;
  }

  pthread_mutex_lock(&listChange); // Locks the list change mutex when splitting (or not) because the linked list is being edited.
  // First condition: If the requested memory goes in exactly to the number of pages, simply add and return the memory
  // or...Second condition: If the second split is less than able to be used, ignore and return only the first.
  if ((s + BLOCK_SIZE) % PAGE_SIZE == 0
  ||  (totPage * PAGE_SIZE) - s - BLOCK_SIZE < BLOCK_SIZE + 1) {
    block_t *expanded = (block_t *) pt;

    // Modifies the last element in the linked list to be the newly created block.
    last->next = expanded;

    // Sets the appropriate metadata values within the new block and returns for use.
    expanded->free = 0;
    expanded->next = NULL;
    expanded->size = s;
    pthread_mutex_unlock(&listChange);
    return expanded;
  }

  // If we have left over memory returned by mmap, split it into two chunks: split1, being the memory we are allocating,
  // and split2 which is the unused and left over memory which we put back into our linked list to be used.
  block_t *split1 = (block_t *) pt;
  block_t *split2 = (block_t *) (pt + s + BLOCK_SIZE); // Splt2 starts after the length of the allocated memory and the block size.

  last->next = split1; 

  split1->free = 0;
  split1->next = split2;
  split1->size = s;

  split2->free = 1;
  split2->next = NULL;
  split2->size = (totPage * PAGE_SIZE) - s - (2 * BLOCK_SIZE); // Equals the total amount returned without the first chunk, or both headers.

  pthread_mutex_unlock(&listChange);
  // Return the allocated memory
  return split1;
}

// Given size and the last block (which isn't available or big enough), allocates a new block for use and returns.
void *expandHeap(size_t s, block_t *last) {
  pthread_mutex_lock(&memChange); // Locks the memory change mutex because sbrk is being called.
  // Expands the program break by the amount requested plus the block header.
  void *pt = sbrk(0);

  // Returns NULL if sbrk has failed, otherwise moves the program break.
  if (sbrk(s + BLOCK_SIZE) == (void *) -1) {
    return NULL;
  }
  pthread_mutex_unlock(&memChange);

  pthread_mutex_lock(&listChange); // Locks the list change mutex as the linked list is being edited.
  // Sets our expanded block to start at the program break, casted to be a block_t.
  block_t *expanded = (block_t *) pt;

  // Modifies the last element in the linked list to be the newly created block.
  last->next = expanded;

  // Sets the appropriate metadata values within the new block and returns for use.
  expanded->free = 0;
  expanded->next = NULL;
  expanded->size = s;
  pthread_mutex_unlock(&listChange);
  return expanded;
}

// Allocates the given number of bytes and returns a block of memory for use of that size.
void *mymalloc(size_t s) {
  pthread_mutex_lock(&listChange); // Locks the list change mutex to prevent any change of "head" when being read in the if statement.
  // First time calling mymalloc, initializes the header and returns.
  if (head == 0) {
    pthread_mutex_unlock(&listChange);
    return (block_t *) init_mem(s) + 1; // Returns one unit more than the block, so the result returned is the first free memory.
  } else {
    pthread_mutex_unlock(&listChange);
  }

  // Gets the next block available, or allocates new memory.
  block_t *next = nextBlock(s);

  // If the allocation fails and returns NULL, print and error and return NULL.
  if (next == NULL) {
    perror("Could not allocate requested memory.");
    return NULL;
  }

  debug_printf("malloc %zu bytes\n", s);
  return next + 1; // Returns one unit more than the block, so the result returned is the first free memory.
}

void *mycalloc(size_t nmemb, size_t s) {
  // Calculates the total space needed by multiplcation.
  size_t total = nmemb * s;

  // Uses mymalloc above to allocate using the needed size.
  void *rslt = mymalloc(total);

  // Sets all values within the allocated memory to 0.
  memset(rslt, 0, total);

  debug_printf("calloc %zu bytes\n", s);
  return rslt;
}

void myfree(void *ptr) {
  // Initializes a pointer to the current block and casts as a block_t.
  block_t *current = (block_t *) ptr;

  // Moves the address back one unit so the pointer points at the header block.
  current = current - 1;

  // Sets the free metadata to be true showing this block is now free.
  current->free = 1;
  debug_printf("Freed some memory\n");
}