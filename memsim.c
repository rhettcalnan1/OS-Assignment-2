
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct
{
    int pageNo;
    int modified;
} page;
enum repl
{
    repl_random,
    fifo,
    lru,
    repl_clock
};

const int pageoffset = 12; /* Page size is fixed to 4 KB */

typedef struct
{
    int valid;
    int pageNo;
    int modified;
    int referenceBit;             /* for clock */
    unsigned long lastAccessTime; /* for LRU (last used tick) */
} FrameEntry;

static FrameEntry *frames = NULL;
static int numFrames = 0;

/* Pointers/State for algorithms */
static int fifo_hand = 0;
static int clock_hand = 0;
static unsigned long tick = 0; /* increases on each memory access */

/* Internal helpers */
static int findFrameByPage(int page_number)
{
    for (int i = 0; i < numFrames; i++)
    {
        if (frames[i].valid && frames[i].pageNo == page_number)
        {
            return i;
        }
    }
    return -1;
}

static int findFreeFrame()
{
    for (int i = 0; i < numFrames; i++)
    {
        if (!frames[i].valid)
            return i;
    }
    return -1;
}

/* Creates the page table structure to record memory allocation */
int createMMU(int framesCount)
{
    numFrames = framesCount;
    frames = (FrameEntry *)calloc(numFrames, sizeof(FrameEntry));
    if (!frames)
        return -1;
    for (int i = 0; i < numFrames; i++)
    {
        frames[i].valid = 0;
        frames[i].pageNo = -1;
        frames[i].modified = 0;
        frames[i].referenceBit = 0;
        frames[i].lastAccessTime = 0;
    }
    fifo_hand = 0;
    clock_hand = 0;
    tick = 0;
    /* make rand deterministic for testing */
    srand(1);
    return 0;
}

/* Checks for residency: returns frame no or -1 if not found */
int checkInMemory(int page_number)
{
    int idx = findFrameByPage(page_number);
    if (idx >= 0)
    {
        /* on hit, update LRU/clock metadata */
        tick++;
        frames[idx].lastAccessTime = tick;
        frames[idx].referenceBit = 1;
        return idx;
    }
    return -1;
}

/* allocate page to the next free frame and record where it put it */
int allocateFrame(int page_number)
{
    int idx = findFreeFrame();
    if (idx < 0)
        return -1; /* should not happen if caller tracked "allocated" */
    tick++;
    frames[idx].valid = 1;
    frames[idx].pageNo = page_number;
    frames[idx].modified = 0;
    frames[idx].referenceBit = 1;
    frames[idx].lastAccessTime = tick;
    return idx;
}

/* Evict frame i and load new page into it. Return victim info. */
static page replaceInFrame(int i, int new_page_number)
{
    page victim;
    victim.pageNo = frames[i].pageNo;
    victim.modified = frames[i].modified;

    /* Load new page into the same frame */
    tick++;
    frames[i].valid = 1;
    frames[i].pageNo = new_page_number;
    frames[i].modified = 0;
    frames[i].referenceBit = 1;
    frames[i].lastAccessTime = tick;

    return victim;
}

/* Selects a victim for eviction/discard according to the replacement algorithm,  returns chosen victim */
page selectVictim(int page_number, enum repl mode)
{
    if (numFrames <= 0)
    {
        page none = {0, 0};
        return none;
    }

    int victim_index = -1;

    if (mode == repl_random)
    {
        victim_index = rand() % numFrames;
    }
    else if (mode == fifo)
    {
        /* simple round-robin hand */
        victim_index = fifo_hand;
        fifo_hand = (fifo_hand + 1) % numFrames;
    }
    else if (mode == lru)
    {
        /* choose frame with smallest age among valid frames */
        unsigned long oldestAccessTime = (unsigned long)(-1);
        for (int i = 0; i < numFrames; i++)
        {
            if (frames[i].valid && frames[i].lastAccessTime < oldestAccessTime)
            {
                oldestAccessTime = frames[i].lastAccessTime;
                victim_index = i;
            }
        }
        if (victim_index < 0)
            victim_index = 0; /* fallback */
    }
    else
    {
        /* standard second-chance/clock using referenceBit */
        int scans = 0;
        while (1)
        {
            if (!frames[clock_hand].referenceBit)
            {
                victim_index = clock_hand;
                clock_hand = (clock_hand + 1) % numFrames;
                break;
            }
            else
            {
                frames[clock_hand].referenceBit = 0; /* give second chance */
                clock_hand = (clock_hand + 1) % numFrames;
            }
            /* avoid infinite loop */
            scans++;
            if (scans > numFrames * 4)
            {
                victim_index = clock_hand;
                clock_hand = (clock_hand + 1) % numFrames;
                break;
            }
        }
    }

    if (victim_index < 0)
        victim_index = 0;
    return replaceInFrame(victim_index, page_number);
}

int main(int argc, char *argv[])
{
    char *tracename;
    int page_number, frame_no, done;
    int do_line;
    int no_events, disk_writes, disk_reads;
    int debugmode;
    enum repl replace;
    int allocated = 0;
    unsigned address;
    char rw;
    page Pvictim;
    FILE *trace;

    if (argc < 5)
    {
        printf("Usage: ./memsim inputfile numberframes replacementmode debugmode \n");
        exit(-1);
    }
    else
    {
        tracename = argv[1];
        trace = fopen(tracename, "r");
        if (trace == NULL)
        {
            printf("Cannot open trace file %s \n", tracename);
            exit(-1);
        }
        numFrames = atoi(argv[2]);
        if (numFrames < 1)
        {
            printf("Frame number must be at least 1\n");
            exit(-1);
        }
        if (strcmp(argv[3], "lru") == 0)
            replace = lru;
        else if (strcmp(argv[3], "rand") == 0)
            replace = repl_random;
        else if (strcmp(argv[3], "clock") == 0)
            replace = repl_clock;
        else if (strcmp(argv[3], "fifo") == 0)
            replace = fifo;
        else
        {
            printf("Replacement algorithm must be rand/fifo/lru/clock  \n");
            exit(-1);
        }

        if (strcmp(argv[4], "quiet") == 0)
            debugmode = 0;
        else if (strcmp(argv[4], "debug") == 0)
            debugmode = 1;
        else
        {
            printf("Replacement algorithm must be quiet/debug  \n");
            exit(-1);
        }
    }

    done = createMMU(numFrames);
    if (done == -1)
    {
        printf("Cannot create MMU");
        exit(-1);
    }
    no_events = 0;
    disk_writes = 0;
    disk_reads = 0;

    do_line = fscanf(trace, "%x %c", &address, &rw);
    while (do_line == 2)
    {
        page_number = address >> pageoffset;
        frame_no = checkInMemory(page_number); /* ask for physical address */

        if (frame_no == -1)
        {
            disk_reads++; /* Page fault, need to load it into memory */
            if (debugmode)
                printf("Page fault %8d \n", page_number);
            if (allocated < numFrames) /* allocate it to an empty frame */
            {
                frame_no = allocateFrame(page_number);
                allocated++;
            }
            else
            {
                Pvictim = selectVictim(page_number, replace); /* returns page number of the victim  */
                frame_no = checkInMemory(page_number);        /* find out the frame the new page is in */
                if (Pvictim.modified)                         /* need to know victim page and modified  */
                {
                    disk_writes++;
                    if (debugmode)
                        printf("Disk write %8d \n", Pvictim.pageNo);
                }
                else
                {
                    if (debugmode)
                        printf("Discard    %8d \n", Pvictim.pageNo);
                }
            }
        }
        if (rw == 'R')
        {
            if (debugmode)
                printf("reading    %8d \n", page_number);
        }
        else if (rw == 'W')
        {
            /* mark page in page table as written - modified  */
            int idx = findFrameByPage(page_number);
            if (idx >= 0)
            {
                frames[idx].modified = 1;
                /* mark as referenced/used */
                tick++;
                frames[idx].lastAccessTime = tick;
                frames[idx].referenceBit = 1;
            }
            if (debugmode)
                printf("writting   %8d \n", page_number);
        }
        else
        {
            printf("Badly formatted file. Error on line %d\n", no_events + 1);
            exit(-1);
        }

        no_events++;
        do_line = fscanf(trace, "%x %c", &address, &rw);
    }

    printf("total memory frames:  %d\n", numFrames);
    printf("events in trace:      %d\n", no_events);
    printf("total disk reads:     %d\n", disk_reads);
    printf("total disk writes:    %d\n", disk_writes);
    printf("page fault rate:      %.4f\n", (float)disk_reads / no_events);

    fclose(trace);
    free(frames);
    return 0;
}
