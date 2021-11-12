#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/uri.h>
#include <search.h>

// #include <sys/shm.h>
// #include <sys/stat.h>
// #include <sys/wait.h>
#include <semaphore.h>
#include <pthread.h>
// #include <errno.h>
// #include <arpa/inet.h>
#include <time.h>
#include <sys/time.h>
// #include <sys/ipc.h>

#include "crc.h"

#define SEED_URL "http://ece252-1.uwaterloo.ca/lab4/"
#define ECE252_HEADER "X-Ece252-Fragment: "
#define BUF_SIZE 1048576  /* 1024*1024 = 1M */
#define BUF_INC  524288   /* 1024*512  = 0.5M */
#define NUM_URLS 5000

#define CT_PNG  "image/png"
#define CT_HTML "text/html"
#define CT_PNG_LEN  9
#define CT_HTML_LEN 9

#define max(a, b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

typedef struct recv_buf2 {
    char *buf;       /* memory to hold a copy of received data */
    size_t size;     /* size of valid data in buf in bytes*/
    size_t max_size; /* max capacity of buf in bytes*/
    int seq;         /* >=0 sequence number extracted from http header */
                     /* <0 indicates an invalid seq number */
} RECV_BUF;

typedef struct node {
    struct node *next;
    char *url;
} NODE;

typedef struct list {
    NODE *head;
    unsigned int size;
} LIST;


/* global variables */
LIST *frontier;
int pngs_found;
int threads_waiting;
char *log_file = "log.txt";
int t;
int m;

sem_t full_list;
sem_t finished;
sem_t needed_attempts;
sem_t png_urls;
sem_t counter;
sem_t waiting_counter;
pthread_mutex_t mutex;

htmlDocPtr mem_getdoc(char *buf, int size, const char *url);
xmlXPathObjectPtr getnodeset (xmlDocPtr doc, xmlChar *xpath);
int find_http(char *fname, int size, int follow_relative_links, const char *base_url);
size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata);
size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata);
int recv_buf_init(RECV_BUF *ptr, size_t max_size);
int recv_buf_cleanup(RECV_BUF *ptr);
void cleanup(CURL *curl, RECV_BUF *ptr);
int write_file(const char *path, const void *in, size_t len);
CURL *easy_handle_init(RECV_BUF *ptr, const char *url);
int process_data(CURL *curl_handle, RECV_BUF *p_recv_buf);

/* helper function for visited URLs hash table */
int add_to_visited(const char *url);
int is_visited(const char *url);

/* helper functions for frontier list */
void init_list();
int push_to_frontier(const char *url);
char * pop_from_frontier();
int is_empty();
void cleanup_list();
void print_list();

htmlDocPtr mem_getdoc(char *buf, int size, const char *url)
{
    int opts = HTML_PARSE_NOBLANKS | HTML_PARSE_NOERROR | \
               HTML_PARSE_NOWARNING | HTML_PARSE_NONET;
    htmlDocPtr doc = htmlReadMemory(buf, size, url, NULL, opts);
    
    if ( doc == NULL ) {
        return NULL;
    }
    return doc;
}

xmlXPathObjectPtr getnodeset (xmlDocPtr doc, xmlChar *xpath)
{
	
    xmlXPathContextPtr context;
    xmlXPathObjectPtr result;

    context = xmlXPathNewContext(doc);
    if (context == NULL) {
        printf("Error in xmlXPathNewContext\n");
        return NULL;
    }
    result = xmlXPathEvalExpression(xpath, context);
    xmlXPathFreeContext(context);
    if (result == NULL) {
        printf("Error in xmlXPathEvalExpression\n");
        return NULL;
    }
    if(xmlXPathNodeSetIsEmpty(result->nodesetval)){
        xmlXPathFreeObject(result);
        return NULL;
    }
    return result;
}

int find_http(char *buf, int size, int follow_relative_links, const char *base_url)
{

    int i;
    htmlDocPtr doc;
    xmlChar *xpath = (xmlChar*) "//a/@href";
    xmlNodeSetPtr nodeset;
    xmlXPathObjectPtr result;
    xmlChar *href;
		
    if (buf == NULL) {
        return 1;
    }

    doc = mem_getdoc(buf, size, base_url);
    result = getnodeset (doc, xpath);
    if (result) {
        nodeset = result->nodesetval;
        for (i=0; i < nodeset->nodeNr; i++) {
            href = xmlNodeListGetString(doc, nodeset->nodeTab[i]->xmlChildrenNode, 1);
            if ( follow_relative_links ) {
                xmlChar *old = href;
                href = xmlBuildURI(href, (xmlChar *) base_url);
                xmlFree(old);
            }
            if ( href != NULL && !strncmp((const char *)href, "http", 4) ) {
                char *url = (char *) href;

                sem_wait(&png_urls);
                if (!is_visited(url)) {
                    pthread_mutex_lock(&mutex);
                    push_to_frontier(url);
                    sem_post(&full_list);
                    pthread_mutex_unlock(&mutex);
                }
                sem_post(&png_urls);
            }
            xmlFree(href);
        }
        xmlXPathFreeObject (result);
    }
    xmlFreeDoc(doc);
    xmlCleanupParser();
    return 0;
}
/**
 * @brief  cURL header call back function to extract image sequence number from 
 *         http header data. An example header for image part n (assume n = 2) is:
 *         X-Ece252-Fragment: 2
 * @param  char *p_recv: header data delivered by cURL
 * @param  size_t size size of each memb
 * @param  size_t nmemb number of memb
 * @param  void *userdata user defined data structurea
 * @return size of header data received.
 * @details this routine will be invoked multiple times by the libcurl until the full
 * header data are received.  we are only interested in the ECE252_HEADER line 
 * received so that we can extract the image sequence number from it. This
 * explains the if block in the code.
 */
size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata)
{
    int realsize = size * nmemb;
    RECV_BUF *p = userdata;

#ifdef DEBUG1_
    // printf("%s", p_recv);
#endif /* DEBUG1_ */
    if (realsize > strlen(ECE252_HEADER) &&
	strncmp(p_recv, ECE252_HEADER, strlen(ECE252_HEADER)) == 0) {

        /* extract img sequence number */
	p->seq = atoi(p_recv + strlen(ECE252_HEADER));

    }
    return realsize;
}


/**
 * @brief write callback function to save a copy of received data in RAM.
 *        The received libcurl data are pointed by p_recv, 
 *        which is provided by libcurl and is not user allocated memory.
 *        The user allocated memory is at p_userdata. One needs to
 *        cast it to the proper struct to make good use of it.
 *        This function maybe invoked more than once by one invokation of
 *        curl_easy_perform().
 */

size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata)
{
    size_t realsize = size * nmemb;
    RECV_BUF *p = (RECV_BUF *)p_userdata;
 
    if (p->size + realsize + 1 > p->max_size) {/* hope this rarely happens */ 
        /* received data is not 0 terminated, add one byte for terminating 0 */
        size_t new_size = p->max_size + max(BUF_INC, realsize + 1);   
        char *q = realloc(p->buf, new_size);
        if (q == NULL) {
            perror("realloc"); /* out of memory */
            return -1;
        }
        p->buf = q;
        p->max_size = new_size;
    }

    memcpy(p->buf + p->size, p_recv, realsize); /*copy data from libcurl*/
    p->size += realsize;
    p->buf[p->size] = 0;

    return realsize;
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
    ptr->seq = -1;              /* valid seq should be positive */
    return 0;
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

void cleanup(CURL *curl, RECV_BUF *ptr)
{
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        recv_buf_cleanup(ptr);
}
/**
 * @brief output data in memory to a file
 * @param path const char *, output file path
 * @param in  void *, input data to be written to the file
 * @param len size_t, length of the input data in bytes
 */

int write_file(const char *path, const void *in, size_t len)
{
    FILE *fp = NULL;

    if (path == NULL) {
        fprintf(stderr, "write_file: file name is null!\n");
        return -1;
    }

    if (in == NULL) {
        fprintf(stderr, "write_file: input data is null!\n");
        return -1;
    }

    fp = fopen(path, "a+");
    if (fp == NULL) {
        perror("fopen");
        return -2;
    }

    if (fwrite(in, 1, len, fp) != len) {
        fprintf(stderr, "write_file: imcomplete write!\n");
        return -3; 
    }
    return fclose(fp);
}

/**
 * @brief create a curl easy handle and set the options.
 * @param RECV_BUF *ptr points to user data needed by the curl write call back function
 * @param const char *url is the target url to fetch resoruce
 * @return a valid CURL * handle upon sucess; NULL otherwise
 * Note: the caller is responsbile for cleaning the returned curl handle
 */

CURL *easy_handle_init(RECV_BUF *ptr, const char *url)
{
    CURL *curl_handle = NULL;

    if ( ptr == NULL || url == NULL) {
        return NULL;
    }

    /* init user defined call back function buffer */
    if ( recv_buf_init(ptr, BUF_SIZE) != 0 ) {
        return NULL;
    }
    /* init a curl session */
    curl_handle = curl_easy_init();

    if (curl_handle == NULL) {
        fprintf(stderr, "curl_easy_init: returned NULL\n");
        return NULL;
    }

    /* specify URL to get */
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);

    /* register write call back function to process received data */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb_curl3); 
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)ptr);

    /* register header call back function to process received header data */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_cb_curl); 
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)ptr);

    /* some servers requires a user-agent field */
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "ece252 lab4 crawler");

    /* follow HTTP 3XX redirects */
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    /* continue to send authentication credentials when following locations */
    curl_easy_setopt(curl_handle, CURLOPT_UNRESTRICTED_AUTH, 1L);
    /* max numbre of redirects to follow sets to 5 */
    curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 5L);
    /* supports all built-in encodings */ 
    curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, "");

    /* Max time in seconds that the connection phase to the server to take */
    //curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 5L);
    /* Max time in seconds that libcurl transfer operation is allowed to take */
    //curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);
    /* Time out for Expect: 100-continue response in milliseconds */
    //curl_easy_setopt(curl_handle, CURLOPT_EXPECT_100_TIMEOUT_MS, 0L);

    /* Enable the cookie engine without reading any initial cookies */
    curl_easy_setopt(curl_handle, CURLOPT_COOKIEFILE, "");
    /* allow whatever auth the proxy speaks */
    curl_easy_setopt(curl_handle, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
    /* allow whatever auth the server speaks */
    curl_easy_setopt(curl_handle, CURLOPT_HTTPAUTH, CURLAUTH_ANY);

    return curl_handle;
}

int is_png(char* buf) {
    if (buf[1] != 0x50 || buf[2] != 0x4E || buf[3] != 0x47) {
        return 0;
    }
    return 1;
}

void copyBytes(char* dest, char* target, int len) {
    for (int i=0; i<len; i++) {
        dest[i] = target[i];
    }
}

int verifycrc(char* type, char* buf, int ind, int len) {

    unsigned char* data = malloc((len+4)*sizeof(char));
    copyBytes(data, buf + ind, len);
    
    unsigned int crc_val = crc(data, len);
    int orig_crc_val = (int)((unsigned char)buf[ind+len] << 24) + (int)((unsigned char)buf[ind+len+1] << 16) + (int)((unsigned char)buf[ind+len+2] << 8) + (int)((unsigned char)buf[ind+len+3]);

    if (orig_crc_val != crc_val) {
        free (data);
        return 0;
    }
    
    free (data);
    return 1;
}

int verify_png(RECV_BUF* recv_buf) {

    if (!is_png(recv_buf->buf)) {
        return 0;
    }
    /* this code might not be necessary for determining pngs, since all fakes have been found by the above check
    int new_chunk = 1;

    int chunk_len = 0;
    int nextChunkInd = 0;
    char* type_code = malloc(4*sizeof(char));
    int i = 8;

    while (i < recv_buf->size) {
        if (new_chunk) {
            chunk_len = (int)((unsigned char)recv_buf->buf[i] << 24) + (int)((unsigned char)recv_buf->buf[i+1] << 16) + (int)((unsigned char)recv_buf->buf[i+2] << 8) + (int)((unsigned char)recv_buf->buf[i+3]);
            strncpy(type_code, recv_buf->buf + i + 4, 4);
            new_chunk = 0;
            i += 8;
            nextChunkInd = i + chunk_len + 4;
            if (!verifycrc(type_code, recv_buf->buf, i-4, chunk_len + 4)) {
                return 0;
            }
        } else {
            i++;
            if (i == nextChunkInd) {
                new_chunk = 1;
            }
        }
    }
    free (type_code);
    */
    return 1;
}

int process_html(CURL *curl_handle, RECV_BUF *p_recv_buf)
{
    int follow_relative_link = 1;
    char *url = NULL; 

    curl_easy_getinfo(curl_handle, CURLINFO_EFFECTIVE_URL, &url);
    find_http(p_recv_buf->buf, p_recv_buf->size, follow_relative_link, url); 
   
    return 0;
}

int process_png(CURL *curl_handle, RECV_BUF *p_recv_buf)
{
    // pid_t pid =getpid();
    char *fname = "png_urls.txt";
    char *eurl = NULL;          /* effective URL */
    curl_easy_getinfo(curl_handle, CURLINFO_EFFECTIVE_URL, &eurl);
    if ( eurl != NULL) {
        // printf("The PNG url is: %s\n", eurl);
    }
    
    if (!verify_png(p_recv_buf)) {
        //printf("not real png\n");
        return -1;
    }
    

    char tmp[256];
    sprintf(tmp, "%s\n", eurl);
    sem_wait(&counter);
    pngs_found++;
    printf("pngs found: %d\n", pngs_found);
    sem_post(&counter);

    return write_file(fname, tmp, strlen(tmp));
}
/**
 * @brief process teh download data by curl
 * @param CURL *curl_handle is the curl handler
 * @param RECV_BUF p_recv_buf contains the received data. 
 * @return 0 on success; non-zero otherwise
 */

int process_data(CURL *curl_handle, RECV_BUF *p_recv_buf)
{
    CURLcode res;
    long response_code;

    res = curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
    if ( res == CURLE_OK ) {
	    // printf("Response code: %ld\n", response_code);
    }

    if ( response_code >= 400 ) { 
    	// fprintf(stderr, "Error.\n");
        return 1;
    }

    char *ct = NULL;
    res = curl_easy_getinfo(curl_handle, CURLINFO_CONTENT_TYPE, &ct);
    if ( res == CURLE_OK && ct != NULL ) {
    	// printf("Content-Type: %s, len=%ld\n", ct, strlen(ct));
    } else {
        // fprintf(stderr, "Failed obtain Content-Type\n");
        return 2;
    }

    if ( strstr(ct, CT_HTML) ) {
        sem_post(&needed_attempts);
        return process_html(curl_handle, p_recv_buf);
    } else if ( strstr(ct, CT_PNG) ) {
        return process_png(curl_handle, p_recv_buf);
    } else {
        sem_post(&needed_attempts);
    }

    return 0;
}

int add_to_visited(const char *url) {
    char tmp[256];
    sprintf(tmp, "%s\n", url);
    write_file(log_file, tmp, strlen(tmp));
    return 0;
}

int is_visited(const char *url) {
    FILE *fp;
    fp = fopen(log_file, "r");
    if (fp == NULL) {
        perror("fopen");
        return -2;
    }
    char str[256];
    while (fgets(str, sizeof(str), fp)) {
        char tmp[256];
        sprintf(tmp, "%s\n", url);
        if (strcmp(str, tmp) == 0) {
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

void init_list() {
    frontier = (LIST*) malloc (sizeof(LIST));
    frontier->head = NULL;
    frontier->size = 0;
}

int push_to_frontier(const char *url) {

    NODE *tmp = frontier->head;
    NODE *new_node = (NODE*) malloc(sizeof(NODE));
    new_node->url = malloc(sizeof(char) * (strlen(url) + 1));
    strcpy(new_node->url, url);
    new_node->next = tmp;
    frontier->head = new_node;
    frontier->size++;
    return 0;
}

void print_list() {
    NODE *current = frontier->head;
    while (current != NULL) {
        printf("url is: %s\n", current->url);
        current = current->next;
    }
}

char * pop_from_frontier() {
    if (!is_empty()) {
        NODE *tmp = NULL;
        tmp = frontier->head;

        NODE *next = tmp->next;
        char *url = malloc(sizeof(char) * (strlen(tmp->url) + 1));
        strcpy(url, tmp->url);
        free(tmp->url);
        free(tmp);
        frontier->head = next;
        frontier->size--;
        return url;
    }
    return NULL;
}

int is_empty() {
    return (frontier->size == 0);
}

void cleanup_list() {
    while (!is_empty()) {
        char *url = pop_from_frontier();
        free(url);
    }
    free(frontier);
}

void web_crawler(char * url) {

    CURL *curl_handle;
    CURLcode res;
    RECV_BUF recv_buf;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_handle = easy_handle_init(&recv_buf, url);

    if ( curl_handle == NULL ) {
        fprintf(stderr, "Curl initialization failed. Exiting...\n");
        curl_global_cleanup();
        abort();
    }
    /* get it! */
    res = curl_easy_perform(curl_handle);

    if( res != CURLE_OK) {
        // fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        cleanup(curl_handle, &recv_buf);
        sem_post(&needed_attempts);
        return;
    } else {
        // printf("%lu bytes received in memory %p, seq=%d.\n", recv_buf.size, recv_buf.buf, recv_buf.seq);
    }

    /* process the download data */
    int pres = process_data(curl_handle, &recv_buf);
    if (pres != 0) {
        sem_post(&needed_attempts);
    }
    cleanup(curl_handle, &recv_buf);

}

void* crawler(void* arg) {

    int oldstate = 0;
    while (1) {
        
        /* testing code
        int temp = 0;
        sem_getvalue(&needed_attempts, &temp);
        sem_wait(&waiting_counter);
        printf("waiting on attempt, there are %d with %d running\n", temp, threads_waiting);
        sem_post(&waiting_counter);
        */

        sem_wait(&waiting_counter);
        threads_waiting ++;
        if (threads_waiting == t) {
            pthread_mutex_lock(&mutex);
            if (is_empty()) {
                sem_post(&finished);
            }
            pthread_mutex_unlock(&mutex);
        }
        sem_post(&waiting_counter);

        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);
        sem_wait(&needed_attempts);
        
        sem_wait(&full_list);

        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

        sem_wait(&waiting_counter);
        threads_waiting --;
        sem_post(&waiting_counter);

        pthread_mutex_lock(&mutex);
        char *current_url = pop_from_frontier();
        pthread_mutex_unlock(&mutex);

        int valid = 0;

        sem_wait(&png_urls);
        if (!is_visited(current_url)) {
            add_to_visited(current_url);
            valid = 1;
        }
        sem_post(&png_urls);

        if (valid) {
            web_crawler(current_url);
        } else {
            sem_post(&needed_attempts);
        }
        free(current_url);

        sem_wait(&counter);
        if (pngs_found == m) {
            sem_post(&finished);
        }
        sem_post(&counter);
    }
    return NULL;
}

int main( int argc, char** argv ) {

    /* variables for parameters */
    int c;
    t = 1;
    m = 50;
    int v = 0;
    char *seed_url = NULL;
    char *png_file = "png_urls.txt";

    char *str = "option requires an argument";

    while ((c = getopt(argc, argv, "t:m:v:")) != -1) {
        switch (c) {
        case 't':
            t = strtoul(optarg, NULL, 10);
            // printf("option -t specifies a value of %d.\n", t);
            if (t <= 0) {
                fprintf(stderr, "%s: %s > 0 -- 't'\n", argv[0], str);
                return -1;
            }
            break;
        case 'm':
            m = strtoul(optarg, NULL, 10);
	        // printf("option -m specifies a value of %d.\n", m);
            if (m < 0) {
                fprintf(stderr, "%s: %s >= 0 -- 'm'\n", argv[0], str);
                return -1;
            }
            break;
        case 'v':
            v = 1;
            log_file = optarg;
	        // printf("option -v specifies a value of %s.\n", log_file);
            break;
        default:
            return -1;
        }
    }

    /* get and verify seed_url parameter */
    seed_url = argv[optind];
    if (seed_url == NULL) {
        printf("Usage: findpng2 <url>\n");
        return 1;
    }

    /* print options and parameters (remove at end) */
    // printf("t: %d, m: %d, v: %d, log_file: %s\n", t, m, v, log_file);

    /* variables for web crawler logic */

    init_list();
    pngs_found = 0;
    threads_waiting = 0;

    /* create empty png_urls file */
    FILE *fp = NULL;
    fp = fopen(png_file, "w+");
    if (fp == NULL) {
        perror("fopen");
        return -2;
    }
    fclose(fp);

    /* create empty log file */
    fp = fopen(log_file, "w+");
    if (fp == NULL) {
        perror("fopen");
        return -2;
    }
    fclose(fp);

    /* init sems */
    pthread_mutex_init(&mutex, NULL);
    sem_init(&full_list, 0, 1);
    sem_init(&needed_attempts, 0, 0);
    sem_init(&counter, 0, 1);
    sem_init(&finished, 0, 0);
    sem_init(&png_urls, 0, 1);
    sem_init(&waiting_counter, 0, 1);

    /* timing */

    double times[2];
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }

    times[0] = (tv.tv_sec) + tv.tv_usec/1000000.;

    /* web crawler logic */

    if (t == 1) {
        // printf("running single-threaded version\n");

        push_to_frontier(seed_url);

        while (pngs_found < m && !is_empty()) {
            
            char *current_url = pop_from_frontier();
            
            /* check if already visited */  
            if (!is_visited(current_url)) {
                web_crawler(current_url);
                add_to_visited(current_url);
            }
            free(current_url);
        }
    } else if (m != 0) {
        // printf("running multi-threaded version\n");
        push_to_frontier(seed_url);
        
        /* initialize variables */

        // printf("m: %d\n", m);
        for (int i=0; i<m; i++) {
            sem_post(&needed_attempts);
        }

        pthread_t pid[t];
        void *vr;
        for (int i=0; i<t; i++) {
            pthread_create(&pid[i], NULL, crawler, NULL);
        }
        sem_wait(&finished);
        // printf("done crawling, ending threads\n");
        for (int i=0; i<t; i++) {
            pthread_cancel(pid[i]);
        }
        for (int i=0; i<t; i++) {
            pthread_join(pid[i], &vr);
        }

    }

    /* timing */
    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }
    times[1] = (tv.tv_sec) + tv.tv_usec/1000000.;
    printf("findpng2 execution time: %.6lf seconds\n", (times[1] - times[0]));

    if (!v) {
        remove(log_file);        
    }
    /* cleaning up */
    cleanup_list();

    pthread_mutex_destroy(&mutex);
    sem_destroy(&full_list);
    sem_destroy(&needed_attempts);
    sem_destroy(&counter);
    sem_destroy(&finished);
    sem_destroy(&png_urls);
    sem_destroy(&waiting_counter);

    return 0;
}

// lists:
// visited URLs
// PNG URLs

// lists shared between threads
// reading - multiple threads
// writing - one thread
// terminate when no more URLs in frontier (to be visited) or number of PNGs found
// counters - PNG URLs found, threads waiting for new URL

// if visited, don't add to frontier

// empty png_urls.txt file if empty search result
// -t: number of threads (single-threaded if not specified)
// -m: number of unique PNG URLs to find (50 is not specified)
// -v: name of file - log URLs visited in file (don't when not specified)

// print: findpng2 execution time: S seconds