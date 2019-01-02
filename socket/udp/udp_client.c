#include <sys/types.h>          
#include <sys/socket.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>


int main(int argc,char **argv)
{
   //定义服务器socket
   struct sockaddr_in server_sock_addr;

   //定义客户机socket_fd
   int client_fd;

   //定义发送缓冲区
   char send_buff[1000];
   int send_len;

   if(argc != 2) {printf("usage: <%s> <server ip>",argv[0]); return -1;}

   /* socket */
   client_fd = socket(AF_INET,SOCK_DGRAM,0);
   if(client_fd < 0){printf("request for socket failed !\n"); return -1;}

   /* 设置网络内存地址*/
   inet_aton(argv[1],&server_sock_addr.sin_addr);
   server_sock_addr.sin_family = AF_INET;
   server_sock_addr.sin_port = htons(6666);
   memset(server_sock_addr.sin_zero,0,8);

   while(1)
   	{
       if(fgets(send_buff,999,stdin))
       	{
          send_len = sendto(client_fd, send_buff, strlen(send_buff), 0,(const struct sockaddr *)&server_sock_addr, sizeof(struct sockaddr));
          if(send_len <= 0){close(client_fd);return -1;}
	     }
   	}
   return 0;
}








































