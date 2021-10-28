#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <curl/curl.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <errno.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include "crc.h"
#include "zutil.h"

#define IMG_URL "http://ece252-1.uwaterloo.ca:2530/image?img="
#define IMG_URL2 "http://ece252-2.uwaterloo.ca:2530/image?img="
#define IMG_URL3 "http://ece252-3.uwaterloo.ca:2530/image?img="
#define ECE252_HEADER "X-Ece252-Fragment: "

#define PNG_SIG_SIZE    8 /* number of bytes of png image signature data */
#define CHUNK_LEN_SIZE  4 /* chunk length field size in bytes */          
#define CHUNK_TYPE_SIZE 4 /* chunk type field size in bytes */
#define CHUNK_CRC_SIZE  4 /* chunk CRC field size in bytes */
#define DATA_IHDR_SIZE 13 /* IHDR chunk data field size */

#define NUM_SEMS 6
#define SEM_PROC 1
#define BUF_SIZE 10240  /* 1024*10 = 10K */

int B = 1;
int PROD_NUM = 1;
int CONS_NUM = 1;
int DELAY = 0;
int IMG_NUM = 1;

sem_t *sems;

typedef unsigned char U8;
typedef unsigned int  U32;
typedef unsigned long int U64;


// URL : http://ece252-1.uwaterloo.ca:2530/image?img=n&part=m
// n - image number
// m - fragment number

// B - buffer size
// P - # of producers
// C - # of consumers
// X - # of millseconds consumer sleeps before processing data
// N - image number

// ./paster2 B P C X N
// B≥1,P=1,C=1,X≥0,andN=1,2,3
// use gettimeofday (record time before creating first process + after last image segment is consumed and all.png generated)
// output: paster2 execution time: <time in seconds> seconds
// 2, 4, or 6 time decimals acceptable

// redirect output to a file: ./your_program > somefile.txt

// producers make requests + fetch 50 image segments
// placed in buffer of size B (shared with consumer tasks)
// when B image segments are in buffer, producers stop producing
// producers terminate when all segments downloaded
// consumer reads segments out of buffer + sleeps for X ms before each one + processes received data - validate segment + inflate + copy data into memory
// sleep() in seconds
// usleep() in microseconds

// remember - consumer cannot get from buffer if empty, producer cannot add to full buffer
// access to buffer => critical section
// need to create mechanism that determines if a producer has placed the last image segment into buffer
// need to create mechanism that determines if a consumer has read out the last image from the buffer

typedef struct recv_buf_flat {
    unsigned char *buf;       /* memory to hold a copy of received data */
    size_t size;     /* size of valid data in buf in bytes*/
    size_t max_size; /* max capacity of buf in bytes*/
    int seq;         /* >=0 sequence number extracted from http header */
                     /* <0 indicates an invalid seq number */
} RECV_BUF;

size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata);
size_t write_cb_curl(char *p_recv, size_t size, size_t nmemb, void *p_userdata);
int recv_buf_init(RECV_BUF *ptr, size_t max_size);
int recv_buf_cleanup(RECV_BUF *ptr);
int isCollected();
int fragmentsCollected(int *fragment);

int fragmentsCollected(int *fragment) {
    int tf = 0;
    sem_wait(&sems[2]);
    if (*fragment > 49) {
        tf = 1;
    }
    sem_post(&sems[2]);
    return tf;
}

int isCollected(int * count) {
    int tf = 0;
    sem_wait(&sems[6]);
    if (*count > 49) {
        tf = 1;
    }
    sem_post(&sems[6]);
    return tf;
}

int recv_buf_cleanup(RECV_BUF *ptr)
{
    if (ptr == NULL) {
	    return 1;
    }
    
    free(ptr->buf);
    ptr->size = 0;
    ptr->max_size = 0;
    return 0;
}

size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata)
{
    int realsize = size * nmemb;
    RECV_BUF *p = userdata;
    
    if (realsize > strlen(ECE252_HEADER) &&
	strncmp(p_recv, ECE252_HEADER, strlen(ECE252_HEADER)) == 0) {

        /* extract img sequence number */
	p->seq = atoi(p_recv + strlen(ECE252_HEADER));

    }
    return realsize;
}

size_t write_cb_curl(char *p_recv, size_t size, size_t nmemb, void *p_userdata)
{
    size_t realsize = size * nmemb;
    RECV_BUF *p = (RECV_BUF *)p_userdata;
 
    if (p->size + realsize + 1 > p->max_size) {/* hope this rarely happens */ 
        printf("size: %d, realsize: %d, max_size: %d\n", p->size, realsize, p->max_size);
        fprintf(stderr, "User buffer is too small, abort...\n");
        abort();
    }

    memcpy(p->buf + p->size, p_recv, realsize); /*copy data from libcurl*/
    p->size += realsize;
    p->buf[p->size] = 0;

    return realsize;
}

int sizeof_shm_recv_buf(size_t nbytes)
{
    return (sizeof(RECV_BUF) + sizeof(char) * nbytes);
}

int recv_buf_init(RECV_BUF *ptr, size_t nbytes)
{
    if ( ptr == NULL ) {
        return 1;
    }
    
    ptr->buf = (char *)ptr + sizeof(RECV_BUF);
    ptr->size = 0;
    ptr->max_size = nbytes;
    ptr->seq = -1;              /* valid seq should be non-negative */
    
    return 0;
}

int main( int argc, char** argv ) {

    /* check if all arguments are provided */
    if (argc != 6) {
        printf("usage: ./paster2 <B> <P> <C> <X> <N>\n");
        return 1;
    }

    /* collect and verify arguments B, P, C, X, N */
    B = atoi(argv[1]);
    PROD_NUM = atoi(argv[2]);
    CONS_NUM = atoi(argv[3]);
    DELAY = atoi(argv[4]);
    IMG_NUM = atoi(argv[5]);

    if (B <= 0) {
        printf("Buffer size must be greater than 0\n");
        return 1;
    }

    if (PROD_NUM <= 0) {
        printf("Number of producers must be greater than 0\n");
        return 1;
    }

    if (CONS_NUM <= 0) {
        printf("Number of consumers must be greater than 0\n");
        return 1;
    }

    if (DELAY < 0) {
        printf("Delay must be at least 0\n");
        return 1;
    }

    if (IMG_NUM < 1 || IMG_NUM > 3) {
        printf("Image number must be 1, 2, or 3\n");
        return 1;
    }
 
    /* declare variables for CURL, shared memory, process ids */

    RECV_BUF *p_shm_recv_buf;
    unsigned char *png_buf;
    int *fragment;
    int *front;
    int *rear;
    int *count;
    unsigned int *total_height;
    unsigned int *file_length1;
    unsigned int *idat_length1;

    int shmid;
    int shmid_png_buf;
    int shmid_sems;
    int shmid_fragment;
    int shmid_front;
    int shmid_rear;
    int shmid_count;
    int shmid_total_height;
    int shmid_file_length1;
    int shmid_idat_length1;

    int shm_size = B * sizeof_shm_recv_buf(BUF_SIZE);

    pid_t pid = 0;
    pid_t cpids[PROD_NUM + CONS_NUM];
    
    /* create shared memory segment */
    printf("shm_size = %d.\n", shm_size);
    shmid = shmget(IPC_PRIVATE, shm_size, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_png_buf = shmget(IPC_PRIVATE, B * BUF_SIZE, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_sems = shmget(IPC_PRIVATE, sizeof(sem_t) * NUM_SEMS, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_fragment = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_front = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_rear = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_count = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_total_height = shmget(IPC_PRIVATE, sizeof(unsigned int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_file_length1 = shmget(IPC_PRIVATE, sizeof(unsigned int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_idat_length1 = shmget(IPC_PRIVATE, sizeof(unsigned int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);

    if (shmid == -1 || shmid_png_buf == -1 || shmid_sems == -1 || shmid_fragment == -1 || shmid_front == -1 || shmid_rear == -1 || shmid_count == -1 || shmid_total_height == -1 || shmid_file_length1 == -1 || shmid_idat_length1 == -1) {
        perror("shmget");
        abort();
    }

    /* attach to shared memory regions and initialize buffer */
    p_shm_recv_buf = shmat(shmid, NULL, 0);
    png_buf = shmat(shmid_png_buf, NULL, 0);
    sems = shmat(shmid_sems, NULL, 0);
    fragment = shmat(shmid_fragment, NULL, 0);
    front = shmat(shmid_front, NULL, 0);
    rear = shmat(shmid_rear, NULL, 0);
    count = shmat(shmid_count, NULL, 0);
    total_height = shmat(shmid_total_height, NULL, 0);
    file_length1 = shmat(shmid_file_length1, NULL, 0);
    idat_length1 = shmat(shmid_idat_length1, NULL, 0);

    if ( p_shm_recv_buf == (void *) -1 || sems == (void *) -1 || fragment == (void *) -1 || front == (void *) -1 || rear == (void *) -1) {
        perror("shmat");
        abort();
    }

    // /* initialize p_shm_recv_buf */
    // for (int i = 0; i < B; i++) {
    //     RECV_BUF tmp;
    //     tmp.size = 0;
    //     tmp.max_size = 0;
    //     tmp.seq = -1;
    //     p_shm_recv_buf[i] = tmp;
    // }

    /* initialize png_buf */
    memset(png_buf, 0, B * BUF_SIZE);

    /* initialize semaphore for spaces */
    sem_init(&sems[0], SEM_PROC, BUF_SIZE);

    /* initialize semaphore for items */
    sem_init(&sems[1], SEM_PROC, 0);

    /* initialize semaphore for fragment number */
    sem_init(&sems[2], SEM_PROC, 1);

    /* initialize semaphore for front number */
    sem_init(&sems[3], SEM_PROC, 1);

    /* initialize semaphore for rear number */
    sem_init(&sems[4], SEM_PROC, 1);

    /* initialize semaphore for png_buf */
    sem_init(&sems[5], SEM_PROC, 1);

    /* initialize semaphore for count */
    sem_init(&sems[6], SEM_PROC, 1);

    /* initialize semaphore for total height */
    sem_init(&sems[7], SEM_PROC, 1);

    /* initialize fragment number to 0 */
    sem_wait(&sems[2]);
    *fragment = 0;
    sem_post(&sems[2]);

    /* initialize front to 0 */
    sem_wait(&sems[3]);
    *front = 0;
    sem_post(&sems[3]);

    /* initialize rear to 0 */
    sem_wait(&sems[4]);
    *rear = 0;
    sem_post(&sems[4]);

    /* initialize file and idat length to 0 */
    *file_length1 = 0;
    *idat_length1 = 0;

    // /* initialize png_buf */
    // sem_wait(&sems[5]);
    // for (int i = 0; i < 50; i++) {
    //     RECV_BUF tmp;
    //     unsigned char buf[BUF_SIZE];
    //     memset(buf, 0, BUF_SIZE);
    //     tmp.buf = buf;
    //     tmp.size = 0;
    //     tmp.max_size = 0;
    //     tmp.seq = -1;
    //     png_buf[i] = tmp;
    // }
    // sem_post(&sems[5]);

    /* initialize count to 0 */
    sem_wait(&sems[6]);
    *count = 0;
    sem_post(&sems[6]);

    /* initialize total height to 0*/
    sem_wait(&sems[7]);
    *total_height = 0;
    sem_post(&sems[7]);

    /* declare variables for catpng logic */
    char *outputFile = "all.png";

    U64 len_def = 0;
    U64 len_inf = 0;
    U64 total_len_inf = 0;
    unsigned int allocation_size = 0;

    U8 *p_buffer = NULL;
    
    U8 *idat_data;
    U32 crc_val = 0;

    for (int i = 0; i < (PROD_NUM + CONS_NUM); i++) {

        pid = fork();

        if ( pid == 0 ) {          /* child proc */

            if (i < PROD_NUM) {
                /* run as a producer */

                while (fragmentsCollected(fragment) == 0) {

                    CURL *curl_handle;
                    CURLcode res;
                    char url[256];

                    curl_global_init(CURL_GLOBAL_DEFAULT);

                    printf("producer # %d running...\n", i);

                    /* init a curl session */
                    curl_handle = curl_easy_init();

                    if (curl_handle == NULL) {
                        fprintf(stderr, "curl_easy_init: returned NULL\n");
                        return 1;
                    }

                    /* assign one of the three servers to the url */
                    if (i % 3 == 0) {
                        strcpy(url, IMG_URL3);
                    } else if (i % 3 == 1) {
                        strcpy(url, IMG_URL);
                    } else {
                        strcpy(url, IMG_URL2);
                    }

                    sprintf(url + strlen(IMG_URL), "%d", IMG_NUM);
                    char part_string[] = "&part=";
                    sem_wait(&sems[2]);
                    sprintf((url + strlen(IMG_URL) + 1), "%s%d", part_string, *fragment);
                    (*fragment)++;
                    printf("fragment: %d\n", *fragment);
                    sem_post(&sems[2]);

                    RECV_BUF recv_buf;
                    recv_buf_init(&recv_buf, BUF_SIZE);

                    /* specify URL to get */
                    curl_easy_setopt(curl_handle, CURLOPT_URL, url);

                    /* register write call back function to process received data */
                    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb_curl); 
                    /* user defined data structure passed to the call back function */
                    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&recv_buf);

                    /* register header call back function to process received header data */
                    curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_cb_curl); 
                    /* user defined data structure passed to the call back function */
                    curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)&recv_buf);

                    /* some servers requires a user-agent field */
                    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

                    /* get it! */
                    res = curl_easy_perform(curl_handle);

                    if( res != CURLE_OK) {
                        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
                    } else {
                        printf("%lu bytes received in memory %p, seq=%d.\n",  \
                            recv_buf.size, recv_buf.buf, recv_buf.seq);
                    }

                    printf("wait for space to be available\n");
                    sem_wait(&sems[0]);
                    printf("space available\n");
                    sem_wait(&sems[3]);
                    sem_wait(&sems[4]);
                    printf("rear available\n");

                    for (int j = 0; j < recv_buf.size; j++) {
                        printf("%02x", recv_buf.buf[j]);
                    }
                    printf("\n");

                    p_shm_recv_buf[*rear].size = recv_buf.size;
                    p_shm_recv_buf[*rear].max_size = recv_buf.max_size;
                    p_shm_recv_buf[*rear].seq = recv_buf.seq;
                    memcpy(png_buf + (*rear), recv_buf.buf, recv_buf.size);

                    *rear = (*rear + 1) % B;

                    printf("%p\n", png_buf + *rear);
                    for (int j = 0; j < recv_buf.size; j++) {
                        printf("%02x", *(png_buf + j +(*rear)));
                    }
                    printf("\n");

                    sem_post(&sems[4]);
                    sem_post(&sems[3]);
                    printf("post rear\n");
                    sem_post(&sems[1]);
                    printf("item added\n");
                    /* end critical section */

                    /* cleaning up */
                    curl_easy_cleanup(curl_handle);
                    curl_global_cleanup();
                }
            
            } else {
                /* run as a consumer */

                while(isCollected(count) == 0) {

                    printf("consumer running...\n");

                    usleep(DELAY * 1000);

                    /* start critical section */
                    sem_wait(&sems[1]);
                    printf("item available\n");
                    sem_wait(&sems[3]);
                    sem_wait(&sems[4]);
                    printf("front available\n");

                    for (int j = 0; j < p_shm_recv_buf[*front].size; j++) {
                        printf("%02x", *(png_buf + j +(*front)));
                    }
                    printf("\n");

                    unsigned int file_length = p_shm_recv_buf[*front].size;
                    p_buffer = malloc(file_length * sizeof(unsigned char));

                    if (p_buffer == NULL) {
                        perror("malloc");
                        return errno;
                    }

                    printf("p_buffer: \n");
                    for (int j = 0; j < p_shm_recv_buf[*front].size; j++) {
                        p_buffer[j] = *(png_buf + j +(*front));
                        printf("%02x", p_buffer[j]);
                    }
                    printf("\n");

                    /* calculate width */
                    printf("calculate width\n");

                    U8 width[9];
                    sprintf(width, "%02x%02x%02x%02x", p_buffer[16], p_buffer[17], p_buffer[18], p_buffer[19]);
                    int w = strtol(width, NULL, 16);

                    /* calculate height */

                    U8 height[9];
                    sprintf(height, "%02x%02x%02x%02x", p_buffer[20], p_buffer[21], p_buffer[22], p_buffer[23]);
                    int h = strtol(height, NULL, 16);

                    sem_wait(&sems[7]);
                    total_height += h;
                    sem_post(&sems[7]);

                    printf("calculate data length\n");

                    /* calculate data length */

                    U8 data_length[9];
                    sprintf(data_length, "%02x%02x%02x%02x", p_buffer[33], p_buffer[34], p_buffer[35], p_buffer[36]);
                    unsigned int len = strtol(data_length, NULL, 16);

                    printf("i is %d\n", i);
                    printf("num of producers is %d\n", PROD_NUM);

                    if (i == PROD_NUM) {
                        file_length1 = file_length;
                        idat_length1 = len;
                    }

                    U8 *buf_inf = malloc(h * (w * 4 + 1));

                    /* inflate data */
                    int ret = mem_inf(buf_inf, &len_inf, (p_buffer + 41), len);

                    if (ret != 0) { /* failure */
                        fprintf(stderr,"mem_inf failed. ret = %d.\n", ret);
                        return ret;
                    }

                    // need to copy inflated buffer to shared buffer (to be concatenated later)               

                    *front = (*front + 1) % B;

                    sem_wait(&sems[6]);
                    printf("increment count\n");
                    (*count)++;
                    sem_post(&sems[6]);

                    free(buf_inf);
                    free(p_buffer);

                    sem_post(&sems[4]);
                    sem_post(&sems[3]);
                    printf("post front\n");
                    sem_post(&sems[0]);
                    printf("space available\n");
                    /* end critical section */
                }
            }

        } else if ( pid > 0 ) {    /* parent proc */
            cpids[i] = pid;
        } else {
            perror("fork");
            abort();
        }
    }

    int state;

    if ( pid > 0 ) {            /* parent process */
        for (int i = 0; i < (PROD_NUM + CONS_NUM); i++ ) {
            waitpid(cpids[i], &state, 0);
            if (WIFEXITED(state)) {
                printf("Child cpid[%d]=%d terminated with state: %d.\n", i, cpids[i], state);
            }
        }

        printf("file length 1: %d\n", file_length1);

    }

    shmdt(p_shm_recv_buf);
    shmdt(png_buf);
    shmdt(sems);
    shmdt(fragment);
    shmdt(front);
    shmdt(rear);
    shmdt(count);
    shmdt(total_height);

    shmctl(shmid, IPC_RMID, NULL);
    shmctl(shmid_png_buf, IPC_RMID, NULL);
    shmctl(shmid_sems, IPC_RMID, NULL);
    shmctl(shmid_fragment, IPC_RMID, NULL);
    shmctl(shmid_front, IPC_RMID, NULL);
    shmctl(shmid_rear, IPC_RMID, NULL);
    shmctl(shmid_count, IPC_RMID, NULL);
    shmctl(shmid_total_height, IPC_RMID, NULL);

    sem_destroy(&sems[0]);
    sem_destroy(&sems[1]);
    sem_destroy(&sems[2]);
    sem_destroy(&sems[3]);
    sem_destroy(&sems[4]);
    sem_destroy(&sems[5]);
    sem_destroy(&sems[6]);
    sem_destroy(&sems[7]);

    return 0;

}