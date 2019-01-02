#include <sys/types.h>         
#include <sys/socket.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>



int main(int argc,char** argv)
{
	//定义一个服务端
	int server_fd;
	int new_client_fd;

	//定义一个返回值检测
	int ret;

	//定义一个server_socket 地址
    struct sockaddr_in server_sock_addr;
    struct sockaddr_in client_sock_addr;

	//sockaddr长度
	int sock_len = sizeof(struct sockaddr );

	//服务器连接数
	int connect_num = -1;

	//接收数据缓冲区和长度
	char recv_buff[1000];
	int  recv_len;

    //杀死僵死进程        server
    signal(SIGCHLD,SIG_IGN);

/* socket  创建一个套接字 */
	server_fd = socket(AF_INET,SOCK_STREAM,0);
	if(server_fd < 0) { printf("request for socket failed!\n"); return -1;}

/* 设置网络参数地址 */
    server_sock_addr.sin_addr.s_addr = INADDR_ANY;
    server_sock_addr.sin_family = AF_INET;
    server_sock_addr.sin_port =htons(6666);
    memset(server_sock_addr.sin_zero,0,8);

/* bind 把套接字绑定到本地计算机的某一个端口上*/
    ret = bind(server_fd,( struct sockaddr *)&server_sock_addr, sizeof(struct sockaddr));    // socket创建的套接字里面没有特别声明网络地址，bind就是给嵌套字声明地址
    if(ret <0) {printf("bind server_fd and server socket failed !\n"); return -1;};

/* listen 服务器的这个端口和IP处于监听状态，等待网络中某一客户机的连接请求，*/
    ret = listen(server_fd,10);
    if(ret < 0) {printf("listen from server socket failed !\n");return -1;}

	while(1)
		{
/* accept 接受连接请求，建立起与客户机之间的通信连接(addr,addrlen是用来给客户端的程序填写的,服务器端只要传递指针就可以了) */		
            new_client_fd = accept(server_fd,( struct sockaddr *)&client_sock_addr, &sock_len);
            if(new_client_fd != -1)
            	{
                 connect_num++;
                 printf("clients %d  connect to this server :%s \n",connect_num,inet_ntoa(client_sock_addr.sin_addr));
                 if(!fork()) /* 子进程 */
				 	while(1)
				 		{
/* recv 用新的套接字来接收远端主机传来的数据，并把数据存到由参数buf指向的内存空间*/
							  recv_len = recv(new_client_fd,recv_buff,999,0);  
		                      if(recv_len <= 0){printf("client %d exit!\n",connect_num);close(new_client_fd);return -1;}
		                      else 
							  	{ 
		                            recv_buff[recv_len] = '\0';
		                            printf("recieve data from client %d  : %s",connect_num,recv_buff);/* 此处不用加'/n'       fgets(client.c)把回车换行也读取到buff中*/
							    } 	
				 		}
			    } /* 父进程 */

	   }
       close(server_fd);
	   return 0;
}


