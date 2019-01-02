#include <sys/types.h>   
#include <sys/socket.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>



int main(int argc ,char** argv)
{

  //定义一个socket 
  struct sockaddr_in server_sock_addr;
  
  //定义socket句柄
  int client_fd;

  //返回值
  int ret=0;

  //输出缓冲区
  char send_buff[1000];

  if(argc != 2) { printf("usage : %s <serverip>",argv[0]);}

  /* 套接字 */ 
  client_fd = socket(AF_INET,SOCK_STREAM,0);
  if(client_fd < 0) {printf("request socket failed !\n"); return -1;}

  /* 设置网络内存地址           这里设置的socket内容是指 希望连接的服务器IP和端口号信息，IP地址来自用户的输入，因此，这里的设置和服务器的设置，要保持内容上的一致*/
  inet_aton(argv[1], &server_sock_addr.sin_addr);
  server_sock_addr.sin_family = AF_INET;
  server_sock_addr.sin_port = htons(6666);
  memset(server_sock_addr.sin_zero, 0, 8);

  /* connect 用来请求连接远程服务器，将参数sockfd的socket连至参数serv_addr所指定的服务器IP和端口号上去*/
  ret = connect(client_fd,( struct sockaddr *)&server_sock_addr,sizeof(struct sockaddr));
  if(ret == -1) {printf("connect to server failed ! \n");  return -1; }

  while(1)
  	{
       if(fgets(send_buff,999,stdin))
	     {
  /*用来发送数据给指定的远端主机*/
           ret = send(client_fd,send_buff,strlen(send_buff),0);     
           if(ret < 0){close(client_fd);return -1;}
		 }
    }
  return 0;
}































