#ifndef FILEBLOCKMANAGER_H__
#define FILEBLOCKMANAGER_H__

/*
* Use CreateFileA()
* ReadFileScatter() -- Windows 
* readv() -- Linux
* WriteFileScatter() -- Windows
* writev() -- Linux
* 
* DirectStorage on windows
* ReadFilesWithIoRing()
* io_uring() for IO operation.
*
signed main(){
    int listenfd = socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in server,client;
    socklen_t addrlen;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(listenfd,(struct sockaddr*)&server,sizeof(server));
    listen(listenfd,6);

    struct io_uring ring;
	struct io_uring_cqe *cqe;
    struct io_uring_sqe *sqe;
    int ret = io_uring_queue_init(QD,&ring,0);
    assert(ret>=0);

    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_poll_add(sqe,listenfd,POLLIN);
    sqe->user_data = LISTENFD;
    io_uring_submit(&ring);

    char buffer[4100];
    buffer[0] = '\0';
    while(1){
        io_uring_wait_cqe(&ring,&cqe);
        if(cqe->user_data == LISTENFD){
            int connectfd = accept(listenfd,(struct sockaddr*)&client,&addrlen);
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_read(sqe,connectfd,buffer,1000,0);
            sqe->user_data = SOCKETIN;
        }
        else if(cqe->user_data == SOCKETIN){
            printf("%s\n",buffer);
        }
        io_uring_cqe_seen(&ring, cqe);
        io_uring_submit(&ring);
    }
    
    return 0;
}
————————————————
版权声明：本文为CSDN博主「FawkesLi」的原创文章，遵循CC 4.0 BY-SA版权协议，转载请附上原文出处链接及本声明。
原文链接：https://blog.csdn.net/Fawkess/article/details/114418795
*/
class FileBlockManager
{
    

};

#endif