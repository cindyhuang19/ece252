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

#define NUM_SEMS 10
#define SEM_PROC 1
#define BUF_SIZE 10240  /* 1024*10 = 10K */
#define MAX_SIZE 1000000

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
int fragmentsCollected(int *fragment);
int allInflated(int *count);

int fragmentsCollected(int *fragment) {
    int tf = 0;
    sem_wait(&sems[2]);
    if (*fragment > 49) {
        tf = 1;
    }
    sem_post(&sems[2]);
    return tf;
}

int allInflated(int * count) {
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
        printf("size: %d, realsize: %d, max_size: %d\n", (int)(p->size), (int)(realsize), (int)(p->max_size));
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

int recv_buf_init(RECV_BUF *ptr, size_t max_size)
{
    void *p = NULL;
    
    if (ptr == NULL) {
        return 1;
    }

    p = malloc(max_size);
    if (p == NULL) {
	    return 2;
    }
    
    ptr->buf = p;
    ptr->size = 0;
    ptr->max_size = max_size;
    ptr->seq = -1;              /* valid seq should be non-negative */
    return 0;
}

int main( int argc, char** argv ) {

    double times[2];
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }

    times[0] = (tv.tv_sec) + tv.tv_usec/1000000.;

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
    U64 *total_len_inf;
    U8 *idat_data;
    unsigned char *first_buf;

    unsigned int *idat_length1;

    int shmid;
    int shmid_png_buf;
    int shmid_sems;
    int shmid_fragment;
    int shmid_front;
    int shmid_rear;
    int shmid_count;
    int shmid_total_len_inf;
    int shmid_idat_data;
    int shmid_idat_length1;
    int shmid_first_buf;

    int shm_size = sizeof_shm_recv_buf(BUF_SIZE);

    pid_t pid = 0;
    pid_t ppids[PROD_NUM];
    pid_t cpids[CONS_NUM];
    
    /* create shared memory segment */
    printf("shm_size = %d.\n", shm_size);
    shmid = shmget(IPC_PRIVATE, B * shm_size, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_png_buf = shmget(IPC_PRIVATE, B * BUF_SIZE, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_sems = shmget(IPC_PRIVATE, sizeof(sem_t) * NUM_SEMS, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_fragment = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_front = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_rear = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_count = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_total_len_inf = shmget(IPC_PRIVATE, sizeof(U64), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_total_len_inf = shmget(IPC_PRIVATE, sizeof(char) * BUF_SIZE, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_idat_data = shmget(IPC_PRIVATE, MAX_SIZE, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_idat_length1 = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    shmid_first_buf = shmget(IPC_PRIVATE, BUF_SIZE, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);

    if (shmid_idat_data == -1 || shmid == -1 || shmid_png_buf == -1 || shmid_sems == -1 || shmid_fragment == -1 || shmid_front == -1 || shmid_rear == -1 || shmid_count == -1 || shmid_total_len_inf == -1) {
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
    total_len_inf = shmat(shmid_total_len_inf, NULL, 0);
    idat_data = shmat(shmid_idat_data, NULL, 0);
    idat_length1 = shmat(shmid_idat_length1, NULL, 0);
    first_buf = shmat(shmid_first_buf, NULL, 0);

    if (idat_data == (void *) -1 || p_shm_recv_buf == (void *) -1 || png_buf == (void *) -1 || sems == (void *) -1 || fragment == (void *) -1 || front == (void *) -1 || rear == (void *) -1 || count == (void *) -1 || total_len_inf == (void *) -1 || idat_length1 == (void *)-1) {
        perror("shmat");
        abort();
    }

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

    /* initialize semaphore for total_len_inf */
    sem_init(&sems[7], SEM_PROC, 1);

    /* initialize semaphore for idat_data */
    sem_init(&sems[8], SEM_PROC, 1);

    /* initialize semaphore for idat_length1 & first buf*/
    sem_init(&sems[9], SEM_PROC, 1);


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

    /* initialize png_buf */
    sem_wait(&sems[5]);
    memset(png_buf, 0, B * BUF_SIZE);
    sem_post(&sems[5]);

    /* initialize count to 0 */
    sem_wait(&sems[6]);
    *count = 0;
    sem_post(&sems[6]);

    /* initialize file and idat length to 0 */
    sem_wait(&sems[7]);
    *total_len_inf = 0;
    sem_post(&sems[7]);

    sem_wait(&sems[9]);
    *idat_length1 = 0;
    sem_post(&sems[9]);

    /* declare variables for catpng logic in consumer */
    U8 *p_buffer = NULL;

    int idat_init = 0;
    
    curl_global_init(CURL_GLOBAL_DEFAULT);

    for (int i = 0; i < (PROD_NUM + CONS_NUM); i++) {

        pid = fork();

        if (i < PROD_NUM) {

            if ( pid == 0 ) {          /* child proc */

                /* run as a producer */

                while (!fragmentsCollected(fragment)) {

                    CURL *curl_handle;
                    CURLcode res;
                    char url[256];

                    // printf("producer # %d running...\n", i);

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
                    // printf("fragment: %d\n", *fragment);
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
                        /*
                        printf("%lu bytes received in memory %p, seq=%d.\n",  \
                            recv_buf.size, recv_buf.buf, recv_buf.seq);
                        */
                    }

                    // printf("wait for space to be available\n");
                    sem_wait(&sems[0]);
                    // printf("space available\n");
                    sem_wait(&sems[3]);
                    sem_wait(&sems[4]);
                    // printf("rear available\n");

                    // for (int j = 0; j < recv_buf.size; j++) {
                    //     printf("%02x", recv_buf.buf[j]);
                    // }
                    printf("\n");
                    
                    p_shm_recv_buf[*rear].size = recv_buf.size;
                    p_shm_recv_buf[*rear].max_size = recv_buf.max_size;
                    p_shm_recv_buf[*rear].seq = recv_buf.seq;

                    //printf("data: rear:%d, size:%d", (int)(*rear), (int)(recv_buf.size));

                    sem_wait(&sems[5]);
                    memcpy(png_buf + *rear, recv_buf.buf, recv_buf.size);

                    sem_post(&sems[5]);

                    *rear = (*rear + 1); //% B;

                    // printf("%p\n", png_buf + *rear);
                    // for (int j = 0; j < recv_buf.size; j++) {
                    //     printf("%02x", *(png_buf + j +(*rear)));
                    // }
                    // printf("\n");

                    recv_buf_cleanup(&recv_buf);

                    sem_post(&sems[4]);
                    sem_post(&sems[3]);
                    // printf("post rear\n");
                    sem_post(&sems[1]);
                    // printf("item added\n");
                    /* end critical section */

                    /* cleaning up */
                    curl_easy_cleanup(curl_handle);

                }

                exit(0);

            } else if ( pid > 0 ) {    /* parent proc */
                ppids[i] = pid;
            } else {
                perror("fork");
                abort();
            }
        
        } else {

            if ( pid == 0 ) {          /* child proc */

                /* run as a consumer */

                while(!allInflated(count)) {

                    // printf("consumer # %d running...\n", i - PROD_NUM);
                    usleep(DELAY * 1000);

                    U64 len_inf = 0;

                    /* start critical section */
                    sem_wait(&sems[1]);
                    // printf("item available\n");
                    sem_wait(&sems[3]);
                    sem_wait(&sems[4]);
                    // printf("front available\n");

                    // for (int j = 0; j < p_shm_recv_buf[*front].size; j++) {
                    //     printf("%02x", *(png_buf + j +(*front)));
                    // }
                    // printf("\n");

                    sem_wait(&sems[5]);

                    unsigned int file_length = p_shm_recv_buf[*front].size;
                    p_buffer = malloc(file_length * sizeof(unsigned char));

                    if (p_buffer == NULL) {
                        perror("malloc");
                        return errno;
                    }

                    //printf("p_buffer: \n");
                    //printf("\n");
                    memcpy(p_buffer, png_buf + *front, file_length);
                    for (int j = 0; j < p_shm_recv_buf[*front].size/100; j++) {
                        //p_buffer[j] = p_shm_recv_buf[*front].buf[j];
                        //p_buffer[j] = *(png_buf + j +(*front));
                        //printf("%02x", *(p_buffer + j));
                    }

                    sem_post(&sems[5]);
                    /* calculate width */
                    // printf("calculate width\n");

                    U8 width[9];
                    sprintf(width, "%02x%02x%02x%02x", p_buffer[16], p_buffer[17], p_buffer[18], p_buffer[19]);
                    int w = strtol(width, NULL, 16);

                    /* calculate height */

                    U8 height[9];
                    sprintf(height, "%02x%02x%02x%02x", p_buffer[20], p_buffer[21], p_buffer[22], p_buffer[23]);
                    int h = strtol(height, NULL, 16);

                    // printf("calculate data length\n");

                    /* calculate data length */

                    U8 data_length[9];
                    sprintf(data_length, "%02x%02x%02x%02x", p_buffer[33], p_buffer[34], p_buffer[35], p_buffer[36]);
                    unsigned int len = strtol(data_length, NULL, 16);

                    sem_wait(&sems[9]);
                    if (p_shm_recv_buf[*front].seq == 1) {
                        printf("first seq\n");
                        *idat_length1 = len;
                        memcpy(first_buf, png_buf + *front, file_length);
                    }

                    sem_post(&sems[9]);

                    // printf("i is %d\n", i);
                    // printf("num of producers is %d\n", PROD_NUM);

                    // sem_wait(&sems[6]);
                    // if (!idat_init) {
                    //     idat_init = 1;
                    //     sem_wait(&sems[8]);
                    //     //idat_data = malloc((h*(w*4+1))*50*2);
                    //     printf("total: %d\n", ((h*(w*4+1))*50*2));
                    //     sem_post(&sems[8]);
                    // }
                    // sem_post(&sems[6]);

                    U8 *buf_inf = malloc(h * (w * 4 + 1));

                    /* inflate data */
                    int ret = mem_inf(buf_inf, &len_inf, (p_buffer + 41), len);

                    if (ret != 0) { /* failure */
                        fprintf(stderr,"mem_inf failed. ret = %d.\n", ret);
                        return ret;
                    }

                    /* PUT INFLATED DATA IN IDAT_DATA */

                    sem_wait(&sems[7]);
                    sem_wait(&sems[8]);
                    memcpy((idat_data + (*total_len_inf)), buf_inf, len_inf);
                    // for (int i2=0; i2<50; i2++) {
                    //     printf("%02x", *(idat_data + i2));
                    // }
                    printf("\nbuf_inf bits: %02x%02x, len_inf:%d\n", *(buf_inf), *(buf_inf + 1), (int)len_inf);
                    sem_post(&sems[8]);
                    *total_len_inf += len_inf;
                    sem_post(&sems[7]);

                    *front = (*front + 1) % B;

                    sem_wait(&sems[6]);
                    // printf("increment count\n");
                    (*count)++;
                    // printf("count: %d\n", *count);
                    sem_post(&sems[6]);

                    free(buf_inf);
                    free(p_buffer);

                    sem_post(&sems[4]);
                    sem_post(&sems[3]);
                    // printf("post front\n");
                    sem_post(&sems[0]);
                    // printf("space available\n");
                    /* end critical section */

                }

                exit(0);

            } else if ( pid > 0 ) {    /* parent proc */
                cpids[i - PROD_NUM] = pid;
            } else {
                perror("fork");
                abort();
            }
        }
    }

    int state;

    if ( pid > 0 ) {            /* parent process */
        for (int i = 0; i < PROD_NUM; i++ ) {
            waitpid(ppids[i], &state, 0);
            if (WIFEXITED(state)) {
                printf("Child ppid[%d]=%d terminated with state: %d.\n", i, ppids[i], state);
            }
        }
        for (int i = 0; i < CONS_NUM; i++ ) {
            waitpid(cpids[i], &state, 0);
            if (WIFEXITED(state)) {
                printf("Child cpid[%d]=%d terminated with state: %d.\n", i, cpids[i], state);
            }
        }
    }

    printf("doing catpng logic\n");

    /* catpng logic */

    char *outputFile = "all.png";
    FILE *out_fp = fopen(outputFile, "wb+");

    U32 crc_val = 0;
    /* stage png header */
    U8 png_id[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(&png_id, 1, PNG_SIG_SIZE, out_fp);

    /* stage IHDR */
    U8 ihdr_length[] = {0x00, 0x00, 0x00, 0x0d};
    U8 ihdr_type[] = {0x49, 0x48, 0x44, 0x52};
    U8 ihdr_chunk[] = {0x00, 0x00, 0x01, 0x90, 0x00, 0x00, 0x01, 0x2C, 0x08, 0x06, 0x00, 0x00, 0x00};
    U8 ihdr_crc[4];

    printf("IHDR CRC\n");

    /* calculate and copy IHDR CRC to file */
    // crc_val = crc((p_buffer + 12), (CHUNK_LEN_SIZE + DATA_IHDR_SIZE));
    // U32 transform_crc = htonl(crc_val);
    // memcpy(ihdr_crc, &transform_crc, 4);

    // fwrite(&ihdr_length, 1, CHUNK_LEN_SIZE, out_fp);
    // fwrite(&ihdr_type, 1, CHUNK_TYPE_SIZE, out_fp);
    // fwrite(&ihdr_chunk, 1, 13, out_fp);
    // fwrite(&ihdr_crc, 1, CHUNK_CRC_SIZE, out_fp);

    printf("deflate data\n");

    /* deflate data */
    U64 len_def = 0;
    sem_wait(&sems[7]);
    U8 *gp_buf_def = malloc((*total_len_inf));

    printf("mem def with total_len_inf:%d and rear is at %d\n", (int)(*total_len_inf), (int)(*rear));

    sem_wait(&sems[8]);
    for (int i=0; i<50; i++) {
        printf("%02x", *(idat_data + i));
    }
    printf("done print\n");
    int ret = mem_def(gp_buf_def, &len_def, idat_data, *total_len_inf, Z_DEFAULT_COMPRESSION);
    sem_post(&sems[8]);

    sem_post(&sems[7]);

     /* failure */
    if (ret != 0) { 
        fprintf(stderr,"mem_def failed. ret = %d.\n", ret);
        return ret;
    }

    printf("IDAT stuff\n");

    sem_wait(&sems[9]);

    U8 *png_buffer = malloc((57+len_def)*sizeof(U8));
    memcpy(png_buffer, first_buf, 20);

    /* copy IHDR height to buffer */
    unsigned int total_height = 6*50;
    unsigned int total_width = 400*50;

    total_height = htonl(total_height);
    memcpy((png_buffer + 20), &total_height, 4);

    /* copy rest of IHDR to buffer */
    memcpy((png_buffer + 24), (first_buf + 24), 5);

    /* calculate and copy IHDR CRC to file */
    crc_val = crc((png_buffer + 12), (CHUNK_LEN_SIZE + DATA_IHDR_SIZE));
    U32 transform_crc = htonl(crc_val);
    memcpy((png_buffer + 29), &transform_crc, 4);

    /* write IDAT length to file */
    U32 transform_len_def = htonl(len_def);
    memcpy((png_buffer + 33), &transform_len_def, 4);

    /* copy IDAT type to buffer */
    memcpy((png_buffer + 37), (first_buf + 37), CHUNK_TYPE_SIZE);

    /* copy IDAT data to buffer */
    memcpy((png_buffer + 41), gp_buf_def, len_def);

    /* calculate and copy IDAT CRC to buffer */
    crc_val = crc((png_buffer + 37), (len_def + CHUNK_TYPE_SIZE));
    U32 transform_idat_crc = htonl(crc_val);
    memcpy((png_buffer + 41 + len_def), &transform_idat_crc, CHUNK_CRC_SIZE);

    /* copy IEND chunk to buffer */
    memcpy((png_buffer + 45 + len_def), (first_buf + 45 + *idat_length1) , 12);

    printf("checkpoint2\n");
    sem_post(&sems[9]);

    /* write IEND chunk to file */
    fwrite(png_buffer, (57+len_def)*sizeof(U8), 1, out_fp);

    

    fclose(out_fp);

    /* timing */
    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }
    times[1] = (tv.tv_sec) + tv.tv_usec/1000000.;
    printf("%s execution time: %.6lf seconds\n", argv[0], times[1] - times[0]);

    free(p_buffer);
    free(gp_buf_def);

    /* detach from shared memory */
    shmdt(p_shm_recv_buf);
    shmdt(png_buf);
    shmdt(sems);
    shmdt(fragment);
    shmdt(front);
    shmdt(rear);
    shmdt(count);
    shmdt(total_len_inf);
    shmdt(idat_length1);

    shmctl(shmid, IPC_RMID, NULL);
    shmctl(shmid_png_buf, IPC_RMID, NULL);
    shmctl(shmid_sems, IPC_RMID, NULL);
    shmctl(shmid_fragment, IPC_RMID, NULL);
    shmctl(shmid_front, IPC_RMID, NULL);
    shmctl(shmid_rear, IPC_RMID, NULL);
    shmctl(shmid_count, IPC_RMID, NULL);
    shmctl(shmid_total_len_inf, IPC_RMID, NULL);
    shmctl(shmid_idat_length1, IPC_RMID, NULL);

    for (int i = 0; i < NUM_SEMS; i++) {
        sem_destroy(&sems[i]);
    }

    curl_global_cleanup();

    return 0;

}

// check if sem[5] is necessary
// check if timing info in right spot
// clean up extra semaphores
