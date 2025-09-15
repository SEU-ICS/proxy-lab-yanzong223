#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

static void handle_client(int connfd);
//
static void *worker(void *vargp);

int parse_uri(const char *reqline, char *host, size_t host_sz,
    char *port, size_t port_sz,
    char *path, size_t path_sz);



// 定义缓存对象结构体
typedef struct cache_obj {
    char key[MAXLINE]; // host:port/path
    char *data;        // header + body
    size_t size;
    struct cache_obj *prev, *next;
} cache_obj_t;

// 定义缓存状态结构体
typedef struct cache_state {
    cache_obj_t *head, *tail; // head: most recently used; tail: least recently used
    size_t total_size;
    pthread_rwlock_t lock;
} cache_t;

static cache_t g_cache;

static void cache_init(void) {
    g_cache.head = NULL;
    g_cache.tail = NULL;
    g_cache.total_size = 0;
    pthread_rwlock_init(&g_cache.lock, NULL);
}

static void cache_deinit(void) {
    pthread_rwlock_wrlock(&g_cache.lock);
    cache_obj_t *curr = g_cache.head;
    while (curr) {
        cache_obj_t *next = curr->next;
        Free(curr->data);
        Free(curr);
        curr = next;
    }
    g_cache.head = NULL;
    g_cache.tail = NULL;
    g_cache.total_size = 0;
    pthread_rwlock_unlock(&g_cache.lock);
    pthread_rwlock_destroy(&g_cache.lock);
}

static void lru_detach(cache_obj_t *obj) {
    if (!obj) return; // empty
    if (obj->prev) {
        obj->prev->next = obj->next;
    } else {
        g_cache.head = obj->next;
    }
    if (obj->next) {
        obj->next->prev = obj->prev;
    } else {
        g_cache.tail = obj->prev;
    }
    obj->prev = NULL;
    obj->next = NULL;
}


static void lru_insert_head(cache_obj_t *obj) {
    obj->prev = NULL;
    obj->next = g_cache.head;
    if (g_cache.head) {
        g_cache.head->prev = obj;
    }
    g_cache.head = obj;
    if (g_cache.tail == NULL) {
        g_cache.tail = obj;
    }
}

static cache_obj_t* cache_lookup(const char *key) {
    for (cache_obj_t *curr = g_cache.head; curr != NULL; curr = curr->next) {
        if (!strcmp(curr->key, key)) {
            return curr;
        }
    }
    return NULL;
}

// 命中，复制返回
static int cache_get(const char *key, char **out_data, size_t *out_size) {
    int hit = 0;
    pthread_rwlock_wrlock(&g_cache.lock);
    cache_obj_t *obj = cache_lookup(key);
    if (obj) {
        lru_detach(obj);
        lru_insert_head(obj);

        *out_size = obj->size;
        *out_data = Malloc(obj->size);
        memcpy(*out_data, obj->data, obj->size);

        hit = 1;
    }
    pthread_rwlock_unlock(&g_cache.lock);
    return hit;
}

static void cache_del_until_enough(size_t need){
    while(g_cache.total_size +  need > MAX_CACHE_SIZE){
        cache_obj_t *vic = g_cache.tail;
        if(!vic) break;
        lru_detach(vic);
        g_cache.total_size -= vic->size;
        Free(vic->data);
        Free(vic);
    }
}


static void cache_put(const char *key,const char *data,size_t size){
    if(size>MAX_CACHE_SIZE) return;
    pthread_rwlock_wrlock(&g_cache.lock);

    cache_obj_t *old = cache_lookup(key);
    if(old){
        lru_detach(old);
        g_cache.total_size -= old->size;
        Free(old->data);
        Free(old); 
    }

    cache_del_until_enough(size);

    cache_obj_t *new_obj = Malloc(sizeof(cache_obj_t));
    strncpy(new_obj->key,key,MAXLINE);
    new_obj->data = Malloc(size);
    memcpy(new_obj->data,data,size);
    new_obj->size = size;
    new_obj->prev = NULL;
    new_obj->next = NULL;

    lru_insert_head(new_obj);
    g_cache.total_size += size;

    pthread_rwlock_unlock(&g_cache.lock);
}

int main(int argc,char **argv)
{

    fprintf(stderr, "[debug] argc=%d", argc);
    for (int i = 0; i < argc; i++) {
        fprintf(stderr, "[debug] argv[%d]='%s'", i, argv[i]);
        }
    if (argc!=2){
        fprintf(stderr,"usage: %s <port>\n",argv[0]);
        exit(1);
    }

    Signal(SIGPIPE,SIG_IGN);

    int listenfd = Open_listenfd(argv[1]);
    cache_init();


//多线程版本
    while(1){
        struct sockaddr_storage client_addr;
        socklen_t client_len = sizeof(client_addr);

        int *connfdp = Malloc(sizeof(int));
        *connfdp = Accept(listenfd,(SA *) &client_addr,&client_len);

        pthread_t tid;
        Pthread_create(&tid,NULL,worker,connfdp);
    }

//     printf("%s", user_agent_hdr);
//     return 0;
}

static void handle_client(int connfd)
{
    rio_t rio;
    Rio_readinitb(&rio,connfd);

    char reqline[MAXLINE];
    char buf[MAXLINE];

    // Read request line

    ssize_t n = Rio_readlineb(&rio, reqline, sizeof(reqline));
    if (n <= 0) return;
    printf("Request line: %s", reqline);


    if (strncmp(reqline, "GET ", 4) != 0) {
        const char *body = "Not Implemented\n";
        char hdr[MAXBUF];
        int blen = (int)strlen(body);
        int m = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.0 501 Not Implemented\r\n"
                         "Content-Type: text/plain\r\n"
                         "Content-Length: %d\r\n"
                         "Connection: close\r\n\r\n", blen);
        Rio_writen(connfd, hdr, m);
        Rio_writen(connfd, body, blen);
        return;
    }

    // read header lines
    char host_hdr_value[MAXLINE]={0};
    while((n = Rio_readlineb(&rio,buf,MAXLINE))>0){
        if (!strcmp(buf,"\r\n"))
        {
            break; 
        }
        printf("Header: %s",buf);
        if(!strncasecmp(buf,"Host:",5)){
            const char *p = buf + 5;
            while( *p == ' ' || *p == '\t') p++;
            size_t m = strcspn(p,"\r\n");
            if (m>=sizeof(host_hdr_value)){
                m = sizeof(host_hdr_value)-1;
            }
            strncpy(host_hdr_value,p,m);
            host_hdr_value[m] = '\0';

        }

    }
    char host[MAXLINE],port[16],path[MAXLINE];
    int ok = -1;
    if (!strncmp(reqline,"GET http://",11)){
        ok = parse_uri(reqline,host,sizeof(host),port,sizeof(port),path,sizeof(path));
    }
    else if (!strncmp(reqline,"GET /",5)){
        if (host_hdr_value[0] =='\0'){
            const char *b = "Bad Request: Host header required\n";
            char hdr[MAXBUF];
            int len = (int) strlen(b);
            int m = snprintf(hdr,MAXBUF,
                "HTTP/1.0 400 Bad Request\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n"
                "\r\n",len);
            Rio_writen(connfd,hdr,m);
            Rio_writen(connfd,b,len);
            return;
        }

        // 提取path
        const char *p0 = reqline + 4;     // "GET "
        const char *sp = strstr(p0," HTTP/");
        if (!sp) return;
        size_t path_len = (size_t)(sp - p0);
        if (path_len >= sizeof(path)) path_len = sizeof(path) - 1;
        strncpy(path, p0, path_len);
        path[path_len] = '\0';

        //从host head提取 host和port
        const char *colon = strchr(host_hdr_value,':');
        if (colon){

            size_t host_len = (size_t)(colon - host_hdr_value);
            if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
            strncpy(host,host_hdr_value,host_len);
            host[host_len] = '\0';

            size_t port_len = strlen(colon+1);
            if (port_len >= sizeof(port)) port_len = sizeof(port) - 1;
            strncpy(port,colon+1,port_len);
            port[port_len] = '\0';
        }
        else{
            strncpy(host, host_hdr_value, sizeof(host) - 1);
            host[sizeof(host)- 1] = '\0';
            strcpy(port,"80");
        }
        ok = 0;
    }
    else{
        ok = -1;
    }

    if (ok != 0){
        const char *body = "Bad Request\n";
        char hdr[MAXBUF];
        int len = (int) strlen(body);
        int m = snprintf(hdr,MAXBUF,
            "HTTP/1.0 400 Bad Request\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n",len);
        Rio_writen(connfd,hdr,m);
        Rio_writen(connfd,body,len);
        return;

    }
    printf("[parse] host=%s, port=%s, path=%s\n", host, port, path);

    if (path[0] == '\0'){
        strcpy(path,"/");
    }

    char cache_key[MAXLINE];
    snprintf(cache_key, sizeof(cache_key), "%s:%s%s", host, port, path);

    char *cache_data = NULL;
    size_t cache_size = 0;
    if (cache_get(cache_key, &cache_data, &cache_size)) {
        Rio_writen(connfd, cache_data, cache_size);
        Free(cache_data);
        printf("[cache] hit for %s\n", cache_key);
        return;
    }

    printf("[parse] host=%s, port=%s, path=%s\n", host, port, path);


    // 4     
    int serverfd = Open_clientfd(host,port);

    if (serverfd<0){
        const char *body = "Bad Gateway\n";
        char hdr[MAXBUF];
        int len = (int) strlen(body);
        int m = snprintf(hdr,MAXBUF,
            "HTTP/1.0 502 Bad Gateway\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n",len);
        Rio_writen(connfd,hdr,m);
        Rio_writen(connfd,body,len);
        return;
    }
    // 构造发送请求
    char outreq[MAXBUF];
    int len = snprintf(outreq, sizeof(outreq),
             "GET %s HTTP/1.0\r\n"
             "Host: %s%s%s\r\n"
             "%s"
             "Connection: close\r\n"
             "Proxy-Connection: close\r\n"
             "\r\n",
             path,
             host, strcmp(port, "80")==0 ? "" : ":", strcmp(port, "80")==0 ? "" : port,
             user_agent_hdr);

    Rio_writen(serverfd,outreq,len);

    rio_t server_rio;
    Rio_readinitb(&server_rio,serverfd);

    char xbuf[MAXLINE];
    ssize_t readnum;
    size_t obj_size=0;
    int can_cache = 1;
    char *obj_buf = Malloc(MAX_OBJECT_SIZE);


    while((readnum = Rio_readnb(&server_rio,xbuf,MAXLINE))>0){
        Rio_writen(connfd,xbuf,readnum);
        if(can_cache){
            if(obj_size + readnum <= MAX_OBJECT_SIZE){
                memcpy(obj_buf + obj_size,xbuf,readnum);
                obj_size += (size_t)readnum;
            }
            else{
                can_cache = 0;
            }
        }
    }
    Close(serverfd);

    if (can_cache && obj_size > 0) {
        cache_put(cache_key, obj_buf, obj_size);
    }
    Free(obj_buf);

}

static void *worker(void *vargp){

    int connfd = *(int *) vargp;
    Free(vargp);
    Pthread_detach(pthread_self());
    handle_client(connfd);
    Close(connfd);
    return NULL;

}


int parse_uri(const char *reqline, char *host, size_t host_sz,
    char *port, size_t port_sz,
    char *path, size_t path_sz)
{
    if (strncmp(reqline, "GET ", 4) != 0) {
        return -1;
    }
    const char *url_start = reqline + 4;
    const char *http_pos = strstr(url_start, "HTTP/");
    if (http_pos == NULL) {
        return -1;
    }

    char url[MAXLINE];
    size_t len = http_pos - url_start; // 修复：去掉 -1
    if (len >= MAXLINE) {
        return -1;
    }
    strncpy(url, url_start, len);
    url[len] = '\0';

    printf("[debug] url=%s\n", url); // 调试输出

    const char *http_prefix = "http://";
    size_t http_prefix_len = strlen(http_prefix);
    if (strncmp(url, http_prefix, http_prefix_len) != 0) {
        return -1;
    }
    char *host_start = url + http_prefix_len;

    char *path_start = strchr(host_start, '/');
    if (!path_start) {
        if (path_sz > 0) {
            path[0] = '\0';
            // path[1] = '\0';
        }
    } else {
        size_t path_len = strlen(path_start);
        if (path_len >= path_sz) {
            return -1;
        }
        strncpy(path, path_start, path_len);
        path[path_len] = '\0';
    }

    printf("[debug] host_start=%s\n", host_start); // 调试输出
    if (path_start) {
        printf("[debug] path_start=%s\n", path_start); // 调试输出
    } else {
        printf("[debug] path_start=NULL\n");
    }

    const char *colon = NULL;
    for (char *p = host_start; *p != '\0' && p != path_start; p++) {
        if (*p == ':') {
            colon = p;
            break;
        }
    }

    if (!colon) {
        size_t host_len = (size_t)(path_start - host_start);
        if (host_len >= host_sz) {
            return -1;
        }
        strncpy(host, host_start, host_len);
        host[host_len] = '\0';

        if (port_sz < 3) {
            return -1;
        }
        port[0] = '8';
        port[1] = '0';
        port[2] = '\0';
    } else {
        size_t host_len = (size_t)(colon - host_start);
        size_t port_len = (size_t)(path_start - colon - 1);
        if (host_len >= host_sz || port_len >= port_sz) {
            return -1;
        }
        strncpy(host, host_start, host_len);
        host[host_len] = '\0';
        strncpy(port, colon + 1, port_len);
        port[port_len] = '\0';
        if (port[0] == '\0') {
            return -1;
        }
    }

// printf("[debug] url=%s\n", url);
// printf("[debug] host_start=%s\n", host_start);
// if (path_start) {
//     printf("[debug] path_start=%s\n", path_start);
// } else {
//     printf("[debug] path_start=NULL\n");
// }
// if (colon) {
//     printf("[debug] colon=%s\n", colon);
// } else {
//     printf("[debug] colon=NULL\n");
// }


    return 0;
}
//
