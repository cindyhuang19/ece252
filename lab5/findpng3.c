#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifndef WIN32
#include <unistd.h>
#endif
#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/uri.h>
#include <search.h>

#include <time.h>
#include <sys/time.h>
#include <curl/multi.h>


// #include "crc.h"

#define SEED_URL "http://ece252-1.uwaterloo.ca/lab4/"
#define ECE252_HEADER "X-Ece252-Fragment: "
#define BUF_SIZE 1048576  /* 1024*1024 = 1M */
#define BUF_INC  524288   /* 1024*512  = 0.5M */
#define MAX_WAIT_MSECS 30*1000 /* Wait max. 30 seconds */

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
char *log_file = "log.txt";
int t;
int m;

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

/* helper functions for multi curl */
static void init(CURLM *cm, char *url);

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

                if (!is_visited(url)) {
                    push_to_frontier(url);
                }
            }
            xmlFree(href);
        }
        xmlXPathFreeObject (result);
    }
    xmlFreeDoc(doc);
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

int verify_png(RECV_BUF* recv_buf) {

    if (!is_png(recv_buf->buf)) {
        return 0;
    }
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
    pngs_found++;
    // printf("pngs found: %d\n", pngs_found);

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
        return process_html(curl_handle, p_recv_buf);
    } else if ( strstr(ct, CT_PNG) ) {
        return process_png(curl_handle, p_recv_buf);
    } else {
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

static size_t cb(char *d, size_t n, size_t l, void *p)
{
    /* take care of the data here, ignored in this example */
    (void)d;
    (void)p;
    return n*l;
}

static void init(CURLM *cm, char *url) {
    CURL *eh = curl_easy_init();
    curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, cb);
    curl_easy_setopt(eh, CURLOPT_HEADER, 0L);
    curl_easy_setopt(eh, CURLOPT_URL, url);
    curl_easy_setopt(eh, CURLOPT_PRIVATE, url);
    curl_easy_setopt(eh, CURLOPT_VERBOSE, 0L);
    curl_multi_add_handle(cm, eh);
}

void web_crawler(char * url) {

    CURL *curl_handle;
    CURLcode res;
    RECV_BUF recv_buf;

    curl_handle = easy_handle_init(&recv_buf, url);

    if ( curl_handle == NULL ) {
        fprintf(stderr, "Curl initialization failed. Exiting...\n");
        return;
    }

    /* get it! */
    res = curl_easy_perform(curl_handle);

    if( res != CURLE_OK) {
        cleanup(curl_handle, &recv_buf);
        return;
    }

    /* process the download data */
    process_data(curl_handle, &recv_buf);
    cleanup(curl_handle, &recv_buf);
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

    /* timing */
    double times[2];
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }

    times[0] = (tv.tv_sec) + tv.tv_usec/1000000.;

    /* web crawler logic */

    CURLM *cm = NULL;
    CURL *eh = NULL;
    CURLMsg *msg = NULL;
    int still_running = 0;
    int msgs_left = 0;
    const char *szUrl;
    char *urls[t];

    int total_connections = 0;

    /* allocate urls array */
    for (int i = 0; i < t; i++) {
        urls[i] = calloc(256, sizeof(char));
    }

    curl_global_init(CURL_GLOBAL_ALL);
    cm = curl_multi_init();

    xmlInitParser();

    push_to_frontier(seed_url);

    while (pngs_found < m && !is_empty()) {

        int connections = 0;
        
        while (connections < t && !is_empty()) {
            char *current_url = pop_from_frontier();

            /* check if already visited */  
            if (!is_visited(current_url)) {
                urls[connections] = current_url;
                init(cm, urls[connections]);
                connections++;
                total_connections++;
                add_to_visited(current_url);
            }
        }

        // printf("connections made: %d\n", connections);

        curl_multi_perform(cm, &still_running);

        do {
            int numfds = 0;
            int res = curl_multi_wait(cm, NULL, 0, MAX_WAIT_MSECS, &numfds);
            if (res != CURLM_OK) {
                fprintf(stderr, "error: curl_multi_wait() returned %d\n", res);
                return EXIT_FAILURE;
            }
            curl_multi_perform(cm, &still_running);
        } while(still_running);

        while ((msg = curl_multi_info_read(cm, &msgs_left)) && pngs_found < m) {
            if (msg->msg == CURLMSG_DONE) {
                eh = msg->easy_handle;

                szUrl = NULL;
                curl_easy_getinfo(eh, CURLINFO_PRIVATE, &szUrl);

                web_crawler(szUrl);
                
                curl_multi_remove_handle(cm, eh);
                curl_easy_cleanup(eh);

            } else {
                fprintf(stderr, "error: after curl_multi_info_read(), CURLMsg=%d\n", msg->msg);
            }
        }
    }

    curl_multi_cleanup(cm);
    curl_global_cleanup();

    // printf("total connections: %d\n", total_connections);

    /* timing */
    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }
    times[1] = (tv.tv_sec) + tv.tv_usec/1000000.;
    printf("findpng3 execution time: %.6lf seconds\n", (times[1] - times[0]));

    if (!v) {
        remove(log_file);        
    }
    /* cleaning up */
    cleanup_list();

    /* free urls array */
    for (int i = 0; i < t; i++) {
        free(urls[i]);
    }

    xmlCleanupParser();

    return 0;
}