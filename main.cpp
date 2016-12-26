#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <map>
#include <netdb.h>         //gethostbyname
#include <unistd.h>        //read,write
#include <poll.h>             //poll,pollfd
#include <arpa/inet.h>   //inet_addr,inet_aton
//#include <fiostream.h>
#include<fstream>

#include <pthread.h>
#include <assert.h>

#include <event.h>

#include <queue>
//#include <hash_set>


#if __GNUC__>2
#include <ext/hash_set>
#include <ext/hash_map>
using namespace __gnu_cxx;
#else
#include <hash_set>
#include <hash_map>
using namespace stdext;
#endif
using namespace std;

namespace __gnu_cxx
{
template<>
struct hash<std::string>
{
    hash<char*> h;
    size_t operator()(const std::string &s) const
    {
        return h(s.c_str());
    };
};
}

#define DEFAULT_PAGE_BUF_SIZE 1048576
#define PORT        25341
#define BACKLOG     5
#define MEM_SIZE    1024

#define MYPORT  8887
#define BUFFER_SIZE 1024



pthread_mutex_t sum_mutex; //互斥锁

struct url
{
    string surl;
    int num;
};
typedef struct url URL;

struct sock_ev
{
    struct event* read_ev;
    struct event* send_ev;
};

struct socket_source
{
    int num;//url 编号
    string url;
    string port;
    string host;
    string resource;
    int socketfd;
    struct hostent* hp;

    char* buffer;

    struct sock_ev* sockev;
};

struct event_base* base;
map<string,int> hrefUrl;
map<string,int> visitedUrl;
//queue<string> hrefUrl;
//hash_set<string> visitedUrl;
ofstream outfile;

int num = 0;

int depth=0;

/*
*线程池里所有运行和等待的任务都是一个CThread_worker
*由于所有任务都在链表里，所以是一个链表结构
*/
typedef struct worker
{
    /*回调函数，任务运行时会调用此函数，注意也可声明成其它形式*/
    void *(*process) (void *arg);
    void *arg;/*回调函数的参数*/
    struct worker *next;

} CThread_worker;



/*线程池结构*/
typedef struct
{
    pthread_mutex_t queue_lock;
    pthread_cond_t queue_ready;

    /*链表结构，线程池中所有等待任务*/
    CThread_worker *queue_head;

    /*是否销毁线程池*/
    int shutdown;
    pthread_t *threadid;
    /*线程池中允许的活动线程数目*/
    int max_thread_num;
    /*当前等待队列的任务数目*/
    int cur_queue_size;

} CThread_pool;

int pool_add_worker (void *(*process) (void *arg), void *arg);
void *thread_routine (void *arg);


//share resource
static CThread_pool *pool = NULL;

void pool_init (int max_thread_num)
{
    pool = (CThread_pool *) malloc (sizeof (CThread_pool));

    pthread_mutex_init (&(pool->queue_lock), NULL);
    pthread_cond_init (&(pool->queue_ready), NULL);

    pool->queue_head = NULL;

    pool->max_thread_num = max_thread_num;
    pool->cur_queue_size = 0;

    pool->shutdown = 0;

    pool->threadid = (pthread_t *) malloc (max_thread_num * sizeof (pthread_t));
    int i = 0;
    for (i = 0; i < max_thread_num; i++)
    {
        pthread_create (&(pool->threadid[i]), NULL, thread_routine,NULL);
    }
}

/*向线程池中加入任务*/
int pool_add_worker (void *(*process) (void *arg), void *arg)
{
    /*构造一个新任务*/
    CThread_worker *newworker = (CThread_worker *) malloc (sizeof (CThread_worker));
    newworker->process = process;
    newworker->arg = arg;
    newworker->next = NULL;/*别忘置空*/

    pthread_mutex_lock (&(pool->queue_lock));
    /*将任务加入到等待队列中*/
    CThread_worker *member = pool->queue_head;
    if (member != NULL)
    {
        while (member->next != NULL)
            member = member->next;
        member->next = newworker;
    }
    else
    {
        pool->queue_head = newworker;
    }

    assert (pool->queue_head != NULL);

    pool->cur_queue_size++;
    pthread_mutex_unlock (&(pool->queue_lock));
    /*好了，等待队列中有任务了，唤醒一个等待线程；
    注意如果所有线程都在忙碌，这句没有任何作用*/
    pthread_cond_signal (&(pool->queue_ready));
    return 0;
}



/*销毁线程池，等待队列中的任务不会再被执行，但是正在运行的线程会一直
把任务运行完后再退出*/
int pool_destroy ()
{
    if (pool->shutdown)
        return -1;/*防止两次调用*/
    pool->shutdown = 1;

    /*唤醒所有等待线程，线程池要销毁了*/
    pthread_cond_broadcast (&(pool->queue_ready));

    /*阻塞等待线程退出，否则就成僵尸了*/
    int i;
    for (i = 0; i < pool->max_thread_num; i++)
        pthread_join (pool->threadid[i], NULL);
    free (pool->threadid);

    /*销毁等待队列*/
    CThread_worker *head = NULL;
    while (pool->queue_head != NULL)
    {
        head = pool->queue_head;
        pool->queue_head = pool->queue_head->next;
        free (head);
    }
    /*条件变量和互斥量也别忘了销毁*/
    pthread_mutex_destroy(&(pool->queue_lock));
    pthread_cond_destroy(&(pool->queue_ready));

    free (pool);
    /*销毁后指针置空是个好习惯*/
    pool=NULL;
    return 0;
}



void * thread_routine (void *arg)
{
    //printf ("starting thread 0x%x\n", pthread_self ());
    while (1)
    {
        pthread_mutex_lock (&(pool->queue_lock));
        /*如果等待队列为0并且不销毁线程池，则处于阻塞状态; 注意
        pthread_cond_wait是一个原子操作，等待前会解锁，唤醒后会加锁*/
        while (pool->cur_queue_size == 0 && !pool->shutdown)
        {
            //printf ("thread 0x%x is waiting\n", pthread_self ());
            pthread_cond_wait (&(pool->queue_ready), &(pool->queue_lock));
        }

        /*线程池要销毁了*/
        if (pool->shutdown)
        {
            /*遇到break,continue,return等跳转语句，千万不要忘记先解锁*/
            pthread_mutex_unlock (&(pool->queue_lock));
            printf ("thread 0x%x will exit\n", pthread_self ());
            pthread_exit (NULL);
        }

        //printf ("thread 0x%x is starting to work\n", pthread_self ());

        /*assert是调试的好帮手*/
        assert (pool->cur_queue_size != 0);
        assert (pool->queue_head != NULL);

        /*等待队列长度减去1，并取出链表中的头元素*/
        pool->cur_queue_size--;
        CThread_worker *worker = pool->queue_head;
        pool->queue_head = worker->next;
        pthread_mutex_unlock (&(pool->queue_lock));

        /*调用回调函数，执行任务*/
        (*(worker->process)) (worker->arg);
        free (worker);
        worker = NULL;
    }
    /*这一句应该是不可达的*/
    pthread_exit (NULL);
}

bool ParseURL( const string & url, string & host, string & resource)
{
    if ( strlen(url.c_str()) > 2000 )
    {
        return false;
    }

    const char * pos = strstr( url.c_str(), "http://" );
    if( pos==NULL ) pos = url.c_str();
    else pos += strlen("http://");
    if( strstr( pos, "/")==0 )
        return false;

    char pHost[100];
    char pResource[2000];
    sscanf( pos, "%[^/]%s", pHost, pResource );

    host = pHost;
    resource = pResource;
    return true;
}

void release_sock_event(struct socket_source* sock_source)
{
    //cout<<"release start............"<<endl;
    //event_del(ev->read_ev);
    //event_del(sock_source->sockev->read_ev);
    //event_del(sock_source->sockev->send_ev);
    //free(sock_source->sockev->read_ev);
    //free(sock_source->sockev->send_ev);
//    cout<<"----------------------------------------------"<<endl;
//    cout<<sock_source->buffer<<endl;
//    cout<<"----------------------------------------------"<<endl;
    free(sock_source->buffer);
    sock_source->buffer = NULL;
//    free(sock_source->hp);
//    sock_source->hp = NULL;
    free(sock_source);
    sock_source = NULL;
    //free(ev);
    //cout<<"release finished..........."<<endl;
}

void* HtmlParserProc(void* args)
{
    //cout<<"parsering..........."<<endl;
    struct socket_source*  sock_source = new struct socket_source;
    sock_source = (struct socket_source*)args;
    //printf("recv the client msg: %s", sock_source->buffer);
    try
    {
        const char *p = sock_source->buffer;
        char *tag="href=\"";
        const char *pos = strstr( p, tag );
        const char * nextQ;
        while( pos )
        {
            pos +=strlen(tag);
            nextQ = strstr( pos, "\"" );
            if( nextQ )
            {
                char * url = new char[ nextQ-pos+1 ];

                sscanf( pos, "%[^\"]", url);

                string surl = url ;//

                if((!strstr(url,"http")) && (!strstr(url,".css")) && (!strstr(url,"javascript")))
                {
                    surl ="http://10.108.84.118/news.sohu.com/" +  surl;

                    pthread_mutex_lock( &sum_mutex );

                    map<string, int>::iterator it_find;
                    it_find = visitedUrl.find(surl);
                    if (it_find == visitedUrl.end()){
                        //it_find->second = 20;
                        //没有访问过
                        hrefUrl.insert(pair <string, int> ( surl, ++num ));

                        outfile<<sock_source->num<<"  "<<num<<endl;
                    }else{
                        outfile<<sock_source->num<<"  "<<it_find->second<<endl;
                    }

//                    if( visitedUrl.find( surl ) == visitedUrl.end() )
//                    {
//                        visitedUrl.insert( surl );
//                        hrefUrl.push( surl );
//                    }

                    pthread_mutex_unlock( &sum_mutex );

                }
                else if(strstr(url,"http://10.108.84.118/news.sohu.com/") && (!strstr(url,".css")) && (!strstr(url,"javascript")))
                {

                    pthread_mutex_lock( &sum_mutex );

                    map<string, int>::iterator it_find;
                    it_find = visitedUrl.find(surl);
                    if (it_find == visitedUrl.end()){
                        //没有访问过
                        hrefUrl.insert(pair <string, int> ( surl, ++num ));
                        outfile<<sock_source->num<<"  "<<num<<endl;
                    }else{
                        outfile<<sock_source->num<<"  "<<it_find->second<<endl;
                    }

//                    if( visitedUrl.find( surl ) == visitedUrl.end() )
//                    {
//                        visitedUrl.insert( surl );
//                        hrefUrl.push( surl );
//                    }

                    pthread_mutex_unlock( &sum_mutex );

                }
                pos = strstr(pos, tag );
                delete [] url;
            }
        }

        release_sock_event(sock_source);
        return NULL;
    }
    catch (exception e)
    {
        cout<<"parser exception"<<endl;
    }
    return NULL;
}

void on_read(int sockfd,short event,void* arg)
{
    //cout<<"reading---------------"<<endl;
    struct socket_source*  sock_source = new struct socket_source;
    sock_source = (struct socket_source*)arg;

    char msg[4096*10];//数组的空间在栈中，超出其作用域会自动释放
    int len = recv(sockfd, msg, sizeof(msg) - 1,MSG_DONTWAIT); //非阻塞模式接收
    //int len = read(sockfd, msg, sizeof(msg) - 1);

    if( len <= 0 )
    {
        //cout<<"read end----------"<<endl;
       // outfile<<num++<<"  "<<sock_source->url<<"  "<<strlen(sock_source->buffer)<<endl;
        pool_add_worker (HtmlParserProc, sock_source);



//        pthread_t tid;
//        int presult = pthread_create(&tid, NULL, HtmlParserProc, (void*)sock_source );
//        if( presult != 0 ) //创建线程成功返回0
//        {
//            cout << "pthread_create error:error_code=" << presult << endl;
//        }
//        pthread_join( tid, NULL );
        //HtmlParserProc(sock_source);
        close(sock_source->socketfd);

        event_free(sock_source->sockev->read_ev);
        return ;
    }

    msg[len] = '\0';

    //if(sock_source->buffer != NULL){
        //cout<<"reading---------------"<<strlen(sock_source->buffer)<<endl;
        strcat(sock_source->buffer, msg);
    //}
}

void on_send(int sockfd, short event, void* arg)
{
    struct socket_source*  sock_source = new struct socket_source;
    sock_source = (struct socket_source*)arg;

    string resource = sock_source->resource;
    string host = sock_source->host;

    string request = "GET " +resource + " HTTP/1.1\r\nHost:" + host + "\r\nConnection:Close\r\n\r\n";
    //???????????????????????????
    if( 0 ==send( sockfd, request.c_str(), request.size(), 0 ) )
    {
        cout << "send error" <<endl;
        close( sockfd );
        return ;
    }
    sock_source->sockev->read_ev = (struct event*)malloc(sizeof(struct event));
    event_set(sock_source->sockev->read_ev, sockfd, EV_READ|EV_PERSIST , on_read, sock_source);
    event_base_set(base, sock_source->sockev->read_ev);
    event_add(sock_source->sockev->read_ev, NULL);
    //event_free(sock_source->sockev->send_ev);
}

struct socket_source*  tcp_connect_server(struct socket_source *sock_source)
{
    if(!ParseURL( sock_source->url, sock_source->host, sock_source->resource ))
    {
        cout << "Can not parse the url"<<endl;
        return NULL;
    }

    sock_source->hp= gethostbyname( sock_source->host.c_str() );
    if( sock_source->hp==NULL )
    {
        cout<< "Can not find host address"<<endl;
        return NULL;
    }

    int sockfd = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if( sockfd == -1 || sockfd == -2 )
    {
        cout << "Can not create sock."<<endl;
        return NULL;
    }
    else
    {
        struct sockaddr_in sa;
        sa.sin_family = AF_INET;
        sa.sin_port = htons( 80 );

        memcpy( &sa.sin_addr,sock_source->hp->h_addr, 4 );

        if( 0!= connect( sockfd, (sockaddr*)&sa, sizeof(sa) ) )
        {
            cout << "Can not connect: "<< sock_source->url <<endl;
            close(sockfd);
            return NULL;
        };

        //cout<<"connect to server successful"<<endl;
        sock_source->socketfd = sockfd;
        return sock_source;
    }

}

void handleUrl(string url)
{
    struct socket_source* sock_source = new struct socket_source;
    sock_source->sockev = new struct sock_ev;
    sock_source->buffer = (char *)malloc(DEFAULT_PAGE_BUF_SIZE);

    sock_source->url = url;
    sock_source->port = "80";

    sock_source = tcp_connect_server(sock_source);

    if(sock_source != NULL)
    {
        sock_source->sockev->send_ev =  (struct event*)malloc(sizeof(struct event));
        sock_source->sockev->send_ev = event_new(base, sock_source->socketfd,  EV_WRITE ,  on_send, sock_source);
        event_add(sock_source->sockev->send_ev, NULL);

        event_base_dispatch(base);
    }
    else
    {
        cout<<"socket init fail!!!"<<endl;
    }

}

int main(int argc, char* argv[])
{
    pthread_mutex_init( &sum_mutex, NULL ); //对锁进行初始化
    //create base
    base = event_base_new();

    //init thread pool...
    pool_init (20);/*线程池中最多三个活动线程*/

    outfile.open("./Result.txt");

    if(outfile.is_open()){
        cout<<"file open success..."<<endl;
    }else{
        cout<<"file open fail..."<<endl;
    }

//    map <string, int> m1;
//    map <string, int>::iterator m1_Iter;
//    m1.insert ( pair <string, int>  ( "1", 20 ) );
//    m1.insert ( pair <string, int>  ( "4", 40 ) );
//    m1.insert ( pair <string, int>  ( "3", 60 ) );
//    m1.insert ( pair <string, int>  ( "2", 50 ) );
//    m1.insert ( pair <string, int>  ( "6", 40 ) );
//    m1.insert ( pair <string, int>  ( "7", 30 ) );
//    cout << "The original map m1 is:"<<m1.size()<<endl;
//    for ( m1_Iter = m1.begin( ); m1_Iter != m1.end( ); m1_Iter++ )
//        cout <<  m1_Iter->first<<" "<<m1_Iter->second<<endl;



    string startUrl = "http://10.108.84.118/news.sohu.com/";
    hrefUrl.insert(pair <string, int>  ( startUrl, ++num ) );

    int i = 0;
    while(true)
    {
        while(hrefUrl.size()>0)
        {
            //cout<<"-------------hrefurl.length:"<<hrefUrl.size()<<endl;
            pthread_mutex_lock( &sum_mutex );

            map <string, int>::iterator urlMapIter = hrefUrl.begin();

            string url =  urlMapIter->first;

            visitedUrl.insert (pair<string, int>(urlMapIter->first,urlMapIter->second));

            hrefUrl.erase(urlMapIter);
            //visitedUrl.insert(url);

            pthread_mutex_unlock( &sum_mutex );

            cout<<i++<<"   "<<url<<endl;
            char* urltmp  = new char[url.size()+1];
            strcpy(urltmp,url.c_str());

            handleUrl(urltmp);
            //cout<<"next........"<<endl;
        }
        if(!hrefUrl.size()>0 && i > 100){
            break;
        }

    }

    cout<<"the crawler has finished!"<<endl;
    outfile.close();

    sleep (5);

    pool_destroy ();

    return 0;
}
